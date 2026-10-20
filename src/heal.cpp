// STEP WRITER SPLITS — the port of step2glb.py's block of the same name (see
// heal.h). Kept a 1:1 transliteration: same steps, same order, same verdicts.
#include "heal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>
#include <unordered_map>

#include <BRepAdaptor_Curve2d.hxx>
#include <BRepBuilderAPI_Copy.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepTools_History.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <Geom_Surface.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <ShapeBuild_ReShape.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <TDF_LabelSequence.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopTools_MapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <TopoDS_Edge.hxx>
#include <TopLoc_Location.hxx>
#include <gp_Pnt2d.hxx>

HealAlias HEAL_ALIAS;

// R3: the cut's pcurve sits on the period origin to this FRACTION of the period
// (or of the closed range). A parameter tolerance, never a length: the file's
// printed digits leave <= 4e-9 rad on the 14 artist STEPs (2026-10-19).
static const double HEAL_PARAM_FRAC = 1e-7;

// ── the text half ────────────────────────────────────────────────────────────

// {#ident: body} of the DATA section, every '…' string blanked to '' (names
// never decide a match, and a '#' inside one is not a reference), whitespace
// dropped.
static std::unordered_map<int, std::string> step_records(const std::string &path)
{
  std::unordered_map<int, std::string> recs;
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return recs;
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string txt = ss.str();
  const size_t i = txt.find("DATA;");
  if (i == std::string::npos)
    return recs;
  size_t j = txt.find("ENDSEC;", i);
  if (j == std::string::npos)
    j = txt.size();
  std::string rec;
  for (size_t k = i + 5; k < j; ++k)
  {
    const char c = txt[k];
    if (c == '\'')
    {
      // a whole string, '' escapes a quote: skip it, keep an empty literal
      size_t m = k + 1;
      while (m < j)
      {
        if (txt[m] == '\'')
        {
          if (m + 1 < j && txt[m + 1] == '\'')
          {
            m += 2;
            continue;
          }
          break;
        }
        ++m;
      }
      rec += "''";
      k = m;
      continue;
    }
    if (c == ';')
    {
      // `#N=body`, whitespace out
      std::string flat;
      flat.reserve(rec.size());
      for (char d : rec)
        if (!std::isspace(static_cast<unsigned char>(d)))
          flat += d;
      rec.clear();
      if (flat.size() < 3 || flat[0] != '#')
        continue;
      size_t p = 1;
      while (p < flat.size() && std::isdigit(static_cast<unsigned char>(flat[p])))
        ++p;
      if (p == 1 || p >= flat.size() || flat[p] != '=')
        continue;
      recs[std::atoi(flat.substr(1, p - 1).c_str())] = flat.substr(p + 1);
      continue;
    }
    rec += c;
  }
  return recs;
}

static std::vector<int> refs_of(const std::string &body)
{
  std::vector<int> out;
  for (size_t k = 0; k < body.size(); ++k)
  {
    if (body[k] != '#')
      continue;
    size_t m = k + 1;
    while (m < body.size() && std::isdigit(static_cast<unsigned char>(body[m])))
      ++m;
    if (m > k + 1)
      out.push_back(std::atoi(body.substr(k + 1, m - k - 1).c_str()));
    k = m - 1;
  }
  return out;
}

namespace
{
struct StepText
{
  std::unordered_map<int, std::string> recs;
  std::unordered_map<int, std::string> memo;

  const std::string &body(int i) const
  {
    static const std::string empty;
    const auto it = recs.find(i);
    return it == recs.end() ? empty : it->second;
  }
  std::string kind(int i) const
  {
    const std::string &b = body(i);
    if (!b.empty() && b[0] == '(')   // complex record (rational spline)
      return b.find("B_SPLINE_SURFACE") != std::string::npos ? "B_SPLINE_SURFACE"
                                                             : "COMPLEX";
    size_t p = 0;
    while (p < b.size() && (std::isupper(static_cast<unsigned char>(b[p])) ||
                            std::isdigit(static_cast<unsigned char>(b[p])) ||
                            b[p] == '_'))
      ++p;
    return b.substr(0, p);
  }
  std::vector<int> refs(int i) const { return refs_of(body(i)); }
  const std::string &resolve(int i)
  {
    const auto it = memo.find(i);
    if (it != memo.end())
      return it->second;
    memo[i] = "#cyc";                // cycle guard
    const std::string &b = body(i);
    std::string out;
    out.reserve(b.size());
    for (size_t k = 0; k < b.size(); ++k)
    {
      if (b[k] != '#')
      {
        out += b[k];
        continue;
      }
      size_t m = k + 1;
      while (m < b.size() && std::isdigit(static_cast<unsigned char>(b[m])))
        ++m;
      if (m == k + 1)
      {
        out += b[k];
        continue;
      }
      const int r = std::atoi(b.substr(k + 1, m - k - 1).c_str());
      out += "{";
      out += resolve(r);
      out += "}";
      k = m - 1;
    }
    memo[i] = out;
    return memo[i];
  }
};
} // namespace

HealPairs writer_split_pairs(const std::string &step_path)
{
  StepText st;
  st.recs = step_records(step_path);
  std::vector<int> ids;
  ids.reserve(st.recs.size());
  for (const auto &kv : st.recs)
    ids.push_back(kv.first);
  std::sort(ids.begin(), ids.end());
  HealPairs out;
  for (int sid : ids)
  {
    const std::string k = st.kind(sid);
    if (k != "CLOSED_SHELL" && k != "OPEN_SHELL")
      continue;
    std::vector<int> faces;
    for (int r : st.refs(sid))
      if (st.kind(r) == "ADVANCED_FACE")
        faces.push_back(r);
    std::unordered_map<int, int> pos, surf;
    for (size_t p = 0; p < faces.size(); ++p)
      pos[faces[p]] = static_cast<int>(p);
    std::map<int, std::vector<int>> uses;
    for (int f : faces)
    {
      const std::vector<int> fr = st.refs(f);
      if (fr.empty())
        continue;
      surf[f] = fr.back();
      for (size_t q = 0; q + 1 < fr.size(); ++q)
      {
        const std::vector<int> lp = st.refs(fr[q]);
        if (lp.empty() || st.kind(lp[0]) != "EDGE_LOOP")
          continue;                   // a VERTEX_LOOP (pole): no edge
        for (int oe : st.refs(lp[0]))
        {
          const std::vector<int> ec = st.refs(oe);
          if (!ec.empty())
            uses[ec.back()].push_back(f);
        }
      }
    }
    std::vector<std::pair<int, int>> cands;
    for (const auto &kv : uses)
    {
      const std::vector<int> &fs = kv.second;
      if (fs.size() != 2 || fs[0] == fs[1])
        continue;                     // a boundary, or a seam (once x2)
      int a = fs[0], b = fs[1];
      if (pos[b] < pos[a])
        std::swap(a, b);
      const auto sa = surf.find(a), sb = surf.find(b);
      if (sa == surf.end() || sb == surf.end() || st.kind(sa->second) == "PLANE")
        continue;
      if (sa->second == sb->second ||
          st.resolve(sa->second) == st.resolve(sb->second))
        cands.emplace_back(a, b);
    }
    std::set<int> members;
    for (const auto &c : cands)
    {
      members.insert(c.first);
      members.insert(c.second);
    }
    int block0 = static_cast<int>(faces.size());
    while (block0 > 0)
    {
      const int f = faces[block0 - 1];
      const auto s = surf.find(f);
      const std::string sk = s == surf.end() ? std::string() : st.kind(s->second);
      if (members.count(f) || sk.rfind("B_SPLINE_SURFACE", 0) == 0)
        --block0;
      else
        break;
    }
    for (const auto &c : cands)
      if (pos[c.second] >= block0)
        out[c] = std::make_pair(pos[c.first], pos[c.second]);
  }
  return out;
}

// ── the shape half ───────────────────────────────────────────────────────────

// R3 on one side: (axis, deviation) — the edge's pcurve on `face` is an
// iso-line of the surface's U (0) or V (1) parameter at its period origin (or
// at either end of a closed, non-periodic range). (-1, inf) when the surface
// closes in neither direction.
static std::pair<int, double> origin_iso(const TopoDS_Edge &edge,
                                         const TopoDS_Face &face)
{
  const Handle(Geom_Surface) surf = BRep_Tool::Surface(face);
  double u1, u2, v1, v2;
  surf->Bounds(u1, u2, v1, v2);
  BRepAdaptor_Curve2d c2(edge, face);
  const double t0 = c2.FirstParameter(), t1 = c2.LastParameter();
  std::vector<gp_Pnt2d> pts;
  for (int k = 0; k < 9; ++k)
    pts.push_back(c2.Value(t0 + (t1 - t0) * k / 8.0));
  std::pair<int, double> best(-1, std::numeric_limits<double>::infinity());
  for (int axis = 0; axis < 2; ++axis)
  {
    const bool periodic = axis == 0 ? surf->IsUPeriodic() : surf->IsVPeriodic();
    const bool closed = axis == 0 ? surf->IsUClosed() : surf->IsVClosed();
    if (!(periodic || closed))
      continue;
    const double lo = axis == 0 ? u1 : v1, hi = axis == 0 ? u2 : v2;
    const double per = periodic ? (axis == 0 ? surf->UPeriod() : surf->VPeriod())
                                : hi - lo;
    if (!(per > 0.0))
      continue;
    double dev = 0.0;
    for (const gp_Pnt2d &p : pts)
    {
      const double x = axis == 0 ? p.X() : p.Y();
      const double d = periodic ? std::fabs(std::remainder(x - lo, per))
                                : std::min(std::fabs(x - lo), std::fabs(x - hi));
      dev = std::max(dev, d);
    }
    dev /= per;
    if (dev < best.second)
      best = std::make_pair(axis, dev);
  }
  return best;
}

// `merge_split_faces`' builder, on whatever pieces it is handed.
static TopoDS_Shape build_merged(const std::vector<TopoDS_Face> &faces,
                                 const TopTools_MapOfShape &cuts,
                                 const Handle(ShapeBuild_ReShape) & ctx)
{
  const TopoDS_Face &a = faces[0];
  for (const TopoDS_Face &f : faces)
    if (f.Orientation() != a.Orientation())
      return TopoDS_Shape();
  TopLoc_Location loc;
  const Handle(Geom_Surface) surf = BRep_Tool::Surface(a, loc);
  Handle(TopTools_HSequenceOfShape) seq = new TopTools_HSequenceOfShape();
  for (const TopoDS_Face &f : faces)
    for (TopExp_Explorer ex(f.Oriented(TopAbs_FORWARD), TopAbs_EDGE); ex.More();
         ex.Next())
      if (!cuts.Contains(ex.Current()))
        seq->Append(ex.Current());
  Handle(TopTools_HSequenceOfShape) wires = new TopTools_HSequenceOfShape();
  ShapeAnalysis_FreeBounds::ConnectEdgesToWires(seq, 1e-7, Standard_True, wires);
  TopoDS_Face face;
  BRep_Builder bb;
  bb.MakeFace(face, surf, loc, BRep_Tool::Tolerance(a));
  for (int i = 1; i <= wires->Length(); ++i)
    bb.Add(face, wires->Value(i));
  ShapeFix_Face fix(face);
  fix.SetContext(ctx);
  fix.Perform();
  const TopoDS_Face fixed = fix.Face();
  if (fixed.IsNull() || !BRepCheck_Analyzer(fixed).IsValid())
    return TopoDS_Shape();
  return fixed.Oriented(a.Orientation());
}

// ONE face on the surface of `faces[0]` bounded by every edge of `faces` except
// the cut edges, or a null shape when the pieces do not make one valid face (a
// helical strip that winds past a full turn — slide.stp — cannot: the pieces
// then stay as the file has them). ShapeFix writes pcurves INTO the edges it
// fixes, so the attempt runs on a COPY first: a refused merge must leave the
// pieces exactly as they came.
static TopoDS_Shape merge_split_faces(const std::vector<TopoDS_Face> &faces,
                                      const TopTools_MapOfShape &cuts,
                                      const Handle(ShapeBuild_ReShape) & ctx)
{
  TopoDS_Compound comp;
  BRep_Builder bb;
  bb.MakeCompound(comp);
  for (const TopoDS_Face &f : faces)
    bb.Add(comp, f);
  BRepBuilderAPI_Copy cp(comp, Standard_True, Standard_False);
  TopTools_MapOfShape trial_cuts;
  for (const TopoDS_Face &f : faces)
    for (TopExp_Explorer ex(f, TopAbs_EDGE); ex.More(); ex.Next())
      if (cuts.Contains(ex.Current()))
        trial_cuts.Add(cp.ModifiedShape(ex.Current()));
  std::vector<TopoDS_Face> trial;
  for (const TopoDS_Face &f : faces)
    trial.push_back(TopoDS::Face(cp.ModifiedShape(f)));
  const Handle(ShapeBuild_ReShape) scratch = new ShapeBuild_ReShape();
  if (build_merged(trial, trial_cuts, scratch).IsNull())
    return TopoDS_Shape();
  return build_merged(faces, cuts, ctx);
}

static bool contains_same(const std::vector<TopoDS_Shape> &v, const TopoDS_Shape &s)
{
  for (const TopoDS_Shape &u : v)
    if (u.IsSame(s))
      return true;
  return false;
}

static std::vector<TopoDS_Shape> uniq(const TopTools_ListOfShape &lst)
{
  std::vector<TopoDS_Shape> out;
  for (TopTools_ListOfShape::Iterator it(lst); it.More(); it.Next())
    if (!contains_same(out, it.Value()))
      out.push_back(it.Value());
  return out;
}

// The cut-end vertices where the rim may be joined: exactly two edges meet
// there and both bound the same two faces.
static TopTools_MapOfShape joinable_ends(const TopoDS_Shape &shape,
                                         const TopTools_MapOfShape &ends)
{
  TopTools_IndexedDataMapOfShapeListOfShape v2e, e2f;
  TopExp::MapShapesAndAncestors(shape, TopAbs_VERTEX, TopAbs_EDGE, v2e);
  TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, e2f);
  TopTools_MapOfShape out;
  for (int i = 1; i <= v2e.Extent(); ++i)
  {
    const TopoDS_Shape &v = v2e.FindKey(i);
    if (!ends.Contains(v))
      continue;
    const std::vector<TopoDS_Shape> edges = uniq(v2e.FindFromIndex(i));
    if (edges.size() != 2)
      continue;
    const std::vector<TopoDS_Shape> fa = uniq(e2f.FindFromKey(edges[0]));
    const std::vector<TopoDS_Shape> fb = uniq(e2f.FindFromKey(edges[1]));
    if (fa.size() != 2 || fb.size() != 2)
      continue;
    if (contains_same(fb, fa[0]) && contains_same(fb, fa[1]))
      out.Add(v);
  }
  return out;
}

// Join the two rim edges at every `joinable` vertex. The join runs on a COPY:
// UnifySameDomain rewrites the pcurves of the faces it touches IN PLACE, safe-
// input mode or not (slide.stp's blanking plate: the healed wall's rim on the
// wrong copy of the period, the face invalid, 44 tears). Every face holding a
// joined edge is judged: ShapeFix may mend its period shift in place; a face it
// cannot mend drops the copy whole — the cut ends stay vertices.
struct Joined
{
  TopoDS_Shape shape;
  BRepBuilderAPI_Copy *copy = nullptr;  // null = not joined
  Handle(BRepTools_History) hist;
};

static Joined join_rims(const TopoDS_Shape &shape,
                        const TopTools_MapOfShape &joinable,
                        BRepBuilderAPI_Copy &cp)
{
  Joined none;
  none.shape = shape;
  if (joinable.Extent() == 0)
    return none;
  cp.Perform(shape, Standard_True, Standard_False);
  const TopoDS_Shape work = cp.Shape();
  TopTools_MapOfShape keep;
  for (TopExp_Explorer ex(shape, TopAbs_VERTEX); ex.More(); ex.Next())
    if (!joinable.Contains(ex.Current()))
      keep.Add(cp.ModifiedShape(ex.Current()));
  ShapeUpgrade_UnifySameDomain usd(work, Standard_True, Standard_False,
                                   Standard_True);
  usd.KeepShapes(keep);
  usd.Build();
  const TopoDS_Shape final_ = usd.Shape();
  TopTools_IndexedMapOfShape old_e;
  TopExp::MapShapes(work, TopAbs_EDGE, old_e);
  TopTools_IndexedDataMapOfShapeListOfShape e2f;
  TopExp::MapShapesAndAncestors(final_, TopAbs_EDGE, TopAbs_FACE, e2f);
  for (int i = 1; i <= e2f.Extent(); ++i)
  {
    if (old_e.Contains(e2f.FindKey(i)))
      continue;                       // not a joined edge
    for (TopTools_ListOfShape::Iterator it(e2f.FindFromIndex(i)); it.More();
         it.Next())
    {
      if (BRepCheck_Analyzer(it.Value()).IsValid())
        continue;
      ShapeFix_Face fix(TopoDS::Face(it.Value()));
      const Handle(ShapeBuild_ReShape) scratch = new ShapeBuild_ReShape();
      fix.SetContext(scratch);
      fix.Perform();
      if (!fix.Face().IsSame(it.Value()) ||
          !BRepCheck_Analyzer(fix.Face()).IsValid())
        return none;
    }
  }
  Joined out;
  out.shape = final_;
  out.copy = &cp;
  out.hist = usd.History();
  return out;
}

static std::vector<TopoDS_Shape> solids_of(const TopoDS_Shape &shape)
{
  std::vector<TopoDS_Shape> out;
  for (TopExp_Explorer ex(shape, TopAbs_SOLID); ex.More(); ex.Next())
    out.push_back(ex.Current());
  return out;
}

HealResult heal_writer_splits(
    const Handle(XCAFDoc_ShapeTool) & shape_tool, const HealPairs &pairs,
    const std::function<int(const TopoDS_Face &)> &face_eid,
    const std::function<std::string(const TopoDS_Shape &)> &solid_name)
{
  HealResult res;
  TDF_LabelSequence labels;
  shape_tool->GetShapes(labels);
  for (int li = 1; li <= labels.Length(); ++li)
  {
    const TDF_Label lab = labels.Value(li);
    if (XCAFDoc_ShapeTool::IsAssembly(lab))
      continue;
    const TopoDS_Shape shape = XCAFDoc_ShapeTool::GetShape(lab);
    if (shape.IsNull())
      continue;
    std::map<int, TopoDS_Face> by_eid;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
    {
      const TopoDS_Face f = TopoDS::Face(ex.Current());
      const int e = face_eid(f);
      if (e && !by_eid.count(e))
        by_eid[e] = f;
    }
    TopTools_MapOfShape cuts;
    std::map<int, int> parent;
    auto find = [&parent](int x) {
      for (auto it = parent.find(x); it != parent.end() && it->second != x;
           it = parent.find(x))
        x = it->second;
      return x;
    };
    for (const auto &kv : pairs)
    {
      const int a = kv.first.first, b = kv.first.second;
      const auto ia = by_eid.find(a), ib = by_eid.find(b);
      if (ia == by_eid.end() || ib == by_eid.end())
        continue;
      TopTools_IndexedMapOfShape ea;
      TopExp::MapShapes(ia->second, TopAbs_EDGE, ea);
      for (TopExp_Explorer ex(ib->second, TopAbs_EDGE); ex.More(); ex.Next())
      {
        const TopoDS_Edge edge = TopoDS::Edge(ex.Current());
        if (!ea.Contains(edge) || cuts.Contains(edge))
          continue;
        const std::pair<int, double> ra = origin_iso(edge, ia->second);
        const std::pair<int, double> rb = origin_iso(edge, ib->second);
        if (ra.first >= 0 && ra.first == rb.first &&
            std::max(ra.second, rb.second) < HEAL_PARAM_FRAC)
        {
          cuts.Add(edge);
          const int ra_ = find(a), rb_ = find(b);
          if (ra_ != rb_)
            parent[rb_] = ra_;
        }
      }
    }
    if (cuts.Extent() == 0)
      continue;
    std::map<int, std::set<int>> comps;
    for (const auto &kv : parent)
      comps[find(kv.first)].insert(kv.first);
    for (auto &kv : comps)
      kv.second.insert(kv.first);
    std::map<int, int> pos;
    for (const auto &kv : pairs)
    {
      pos[kv.first.first] = kv.second.first;
      pos[kv.first.second] = kv.second.second;
    }
    // components in a fixed order (by their earliest piece's eid) — the python
    // reference walks the same order, so `healed` lists the same way
    std::vector<std::vector<int>> ordered;
    for (const auto &kv : comps)
    {
      std::vector<int> order(kv.second.begin(), kv.second.end());
      std::sort(order.begin(), order.end(), [&pos](int x, int y) {
        return pos[x] != pos[y] ? pos[x] < pos[y] : x < y;
      });
      ordered.push_back(order);
    }
    std::sort(ordered.begin(), ordered.end(),
              [](const std::vector<int> &x, const std::vector<int> &y) {
                return x[0] < y[0];
              });
    Handle(ShapeBuild_ReShape) ctx = new ShapeBuild_ReShape();
    std::vector<std::pair<std::vector<int>, TopoDS_Shape>> done;
    for (const std::vector<int> &order : ordered)
    {
      // the EARLIER piece is the face of the model's own order — the merged
      // face keeps its eid; the appended pieces were the writer's
      std::vector<TopoDS_Face> faces;
      for (int e : order)
        faces.push_back(by_eid[e]);
      const TopoDS_Shape merged = merge_split_faces(faces, cuts, ctx);
      if (merged.IsNull())
      {
        res.kept.push_back(order);
        continue;
      }
      done.emplace_back(order, merged);
    }
    if (done.empty())
      continue;
    for (const auto &d : done)
    {
      ctx->Replace(by_eid[d.first[0]], d.second);
      for (size_t k = 1; k < d.first.size(); ++k)
        ctx->Remove(by_eid[d.first[k]]);
    }
    const TopoDS_Shape new_ = ctx->Apply(shape);
    // the rim join: only the vertices at the ends of the HEALED cuts may go
    TopTools_MapOfShape ends;
    std::set<int> healed_eids;
    for (const auto &d : done)
      healed_eids.insert(d.first.begin(), d.first.end());
    for (const auto &kv : pairs)
    {
      if (!healed_eids.count(kv.first.first))
        continue;
      TopTools_IndexedMapOfShape ea;
      TopExp::MapShapes(by_eid[kv.first.first], TopAbs_EDGE, ea);
      for (int k = 1; k <= ea.Extent(); ++k)
        if (cuts.Contains(ea.FindKey(k)))
        {
          TopTools_IndexedMapOfShape vm;
          TopExp::MapShapes(ea.FindKey(k), TopAbs_VERTEX, vm);
          for (int j = 1; j <= vm.Extent(); ++j)
            ends.Add(vm.FindKey(j));
        }
    }
    const TopTools_MapOfShape joinable = joinable_ends(new_, ends);
    BRepBuilderAPI_Copy cp;
    const Joined joined = join_rims(new_, joinable, cp);
    // identity: every face of the label -> its eid (through EVERY rebuild: the
    // replace/remove, which also rebuilds every neighbour whose edge ShapeFix
    // touched, then the rim join's copy and its own history); every body ->
    // its name
    const Handle(BRepTools_History) rh = ctx->History();
    for (const auto &kv : by_eid)
    {
      const TopoDS_Shape &f = kv.second;
      if (rh->IsRemoved(f))
        continue;                     // a piece merged into an earlier one
      TopoDS_Shape f1 = f;
      const TopTools_ListOfShape &mod = rh->Modified(f);
      if (!mod.IsEmpty())
        f1 = mod.First();
      if (joined.copy != nullptr)
      {
        f1 = joined.copy->ModifiedShape(f1);
        const TopTools_ListOfShape &mod2 = joined.hist->Modified(f1);
        if (!mod2.IsEmpty())
          f1 = mod2.First();
      }
      if (!f1.IsSame(f) && HEAL_ALIAS.faces.FindIndex(f1) == 0)
      {
        HEAL_ALIAS.faces.Add(f1);     // an untouched face: the map has it
        HEAL_ALIAS.face_eids.push_back(kv.first);
      }
    }
    const std::vector<TopoDS_Shape> before = solids_of(shape);
    const std::vector<TopoDS_Shape> after = solids_of(joined.shape);
    if (before.size() == after.size())  // the heal keeps every body
      for (size_t k = 0; k < before.size(); ++k)
      {
        if (after[k].IsSame(before[k]) || HEAL_ALIAS.solids.FindIndex(after[k]))
          continue;
        const std::string name = solid_name(before[k]);  // BEFORE it joins
        HEAL_ALIAS.solids.Add(after[k]);
        HEAL_ALIAS.solid_names.push_back(name);
      }
    shape_tool->SetShape(lab, joined.shape);
    for (const auto &d : done)
    {
      HealRecord r;
      r.kept = d.first[0];
      r.gone.assign(d.first.begin() + 1, d.first.end());
      res.healed.push_back(r);
      res.removed += static_cast<int>(d.first.size()) - 1;
    }
  }
  if (!res.healed.empty())
    shape_tool->UpdateAssemblies();
  return res;
}
