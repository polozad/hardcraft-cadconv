// MESH REPAIR — the port of step2glb.py's block of the same name (see
// repair.h). Kept a 1:1 transliteration: same steps, same order, same
// verdicts — the parity gate compares the two outputs.
#include "repair.h"

#include "oracle.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <BRepGProp.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <GProp_GProps.hxx>
#include <Poly_PolygonOnTriangulation.hxx>
#include <Poly_Triangle.hxx>
#include <Poly_Triangulation.hxx>
#include <TColStd_Array1OfInteger.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

// a face's outline longer than this many nodes stays as the mesher left it —
// the ear clip is quadratic and a hole that size is not a neck to bridge
static const int REPAIR_MAX_NODES = 1000;

namespace
{
using Seg = std::pair<int, int>;
using Tri = std::array<int, 3>;
struct UV
{
  double u, v;
};
struct XYZ
{
  double x, y, z;
};

Seg seg(int a, int b) { return a < b ? Seg(a, b) : Seg(b, a); }

// {(lo, hi) node pair} of every edge polygon of `face` on `tri`; false when an
// edge cannot be read (a seam, a degenerate edge, a missing polygon).
bool edge_segments(const TopoDS_Face &face, const Handle(Poly_Triangulation) & tri,
                   const TopLoc_Location &loc, std::set<Seg> &segs)
{
  for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next())
  {
    const TopoDS_Edge edge = TopoDS::Edge(ex.Current());
    if (BRep_Tool::Degenerated(edge) || BRep_Tool::IsClosed(edge, face))
      return false;
    const Handle(Poly_PolygonOnTriangulation) pol =
        BRep_Tool::PolygonOnTriangulation(edge, tri, loc);
    if (pol.IsNull())
      return false;
    const TColStd_Array1OfInteger &nodes = pol->Nodes();
    for (int k = 1; k < pol->NbNodes(); ++k)
    {
      const int a = nodes.Value(k), b = nodes.Value(k + 1);
      if (a != b)
        segs.insert(seg(a, b));
    }
  }
  return true;
}

std::map<Seg, int> tri_edge_count(const std::vector<Tri> &tris)
{
  std::map<Seg, int> cnt;
  for (const Tri &t : tris)
    for (int k = 0; k < 3; ++k)
      ++cnt[seg(t[k], t[(k + 1) % 3])];
  return cnt;
}

// The triangles close exactly over the outline: every edge segment is a
// triangle edge once, every other triangle edge is shared by two.
bool closes(const std::vector<Tri> &tris, const std::set<Seg> &segs)
{
  const std::map<Seg, int> cnt = tri_edge_count(tris);
  for (const Seg &s : segs)
  {
    const auto it = cnt.find(s);
    if (it == cnt.end() || it->second != 1)
      return false;
  }
  for (const auto &kv : cnt)
    if (!segs.count(kv.first) && kv.second != 2)
      return false;
  return true;
}

std::vector<Tri> face_tris(const Handle(Poly_Triangulation) & tri)
{
  std::vector<Tri> out;
  for (int k = 1; k <= tri->NbTriangles(); ++k)
  {
    int a, b, c;
    tri->Triangle(k).Get(a, b, c);
    out.push_back(Tri{a, b, c});
  }
  return out;
}

// Node ids of the one outer wire in wire order; false when it cannot be read
// (several wires, a polygon missing or not chained).
bool outline(const TopoDS_Face &face, const Handle(Poly_Triangulation) & tri,
             const TopLoc_Location &loc, std::vector<int> &loop)
{
  int nw = 0;
  for (TopExp_Explorer ex(face, TopAbs_WIRE); ex.More(); ex.Next())
    ++nw;
  if (nw != 1)
    return false;
  for (BRepTools_WireExplorer we(BRepTools::OuterWire(face), face); we.More();
       we.Next())
  {
    const Handle(Poly_PolygonOnTriangulation) pol =
        BRep_Tool::PolygonOnTriangulation(we.Current(), tri, loc);
    if (pol.IsNull())
      return false;
    std::vector<int> ids;
    for (int k = 1; k <= pol->NbNodes(); ++k)
      ids.push_back(pol->Nodes().Value(k));
    if (we.Orientation() == TopAbs_REVERSED)
      std::reverse(ids.begin(), ids.end());
    size_t from = 0;
    if (!loop.empty())
    {
      if (loop.back() != ids[0])
        return false;
      from = 1;
    }
    loop.insert(loop.end(), ids.begin() + from, ids.end());
  }
  if (loop.size() > 1 && loop.front() == loop.back())
    loop.pop_back();
  return loop.size() >= 3;
}

double uv_area(const std::vector<int> &ids, std::map<int, UV> &uv)
{
  const size_t n = ids.size();
  double s = 0.0;
  for (size_t k = 0; k < n; ++k)
    s += uv[ids[k]].u * uv[ids[(k + 1) % n]].v - uv[ids[(k + 1) % n]].u * uv[ids[k]].v;
  return 0.5 * s;
}

// True when the open segments p1p2 and p3p4 cross.
bool crossing(const UV &p1, const UV &p2, const UV &p3, const UV &p4)
{
  const double d = (p2.u - p1.u) * (p4.v - p3.v) - (p2.v - p1.v) * (p4.u - p3.u);
  if (d == 0.0)
    return false;
  const double t =
      ((p3.u - p1.u) * (p4.v - p3.v) - (p3.v - p1.v) * (p4.u - p3.u)) / d;
  const double u =
      ((p3.u - p1.u) * (p2.v - p1.v) - (p3.v - p1.v) * (p2.u - p1.u)) / d;
  return 0.0 < t && t < 1.0 && 0.0 < u && u < 1.0;
}

double cross2(const UV &o, const UV &a, const UV &b)
{
  return (a.u - o.u) * (b.v - o.v) - (a.v - o.v) * (b.u - o.u);
}

// Triangles (counter-clockwise in uv) of the simple polygon `ids`; false when
// no ear is left (the polygon is not simple after all).
bool ear_clip(const std::vector<int> &ids, std::map<int, UV> &uv, std::vector<Tri> &out)
{
  std::vector<int> idx(ids);
  if (uv_area(ids, uv) < 0.0)
    std::reverse(idx.begin(), idx.end());
  while (idx.size() > 3)
  {
    bool found = false;
    const int m = static_cast<int>(idx.size());
    for (int k = 0; k < m; ++k)
    {
      const int i0 = idx[(k - 1 + m) % m], i1 = idx[k], i2 = idx[(k + 1) % m];
      const UV &a = uv[i0], &b = uv[i1], &c = uv[i2];
      if (cross2(a, b, c) <= 0.0)
        continue;
      bool inside = false;
      for (int j : idx)
      {
        if (j == i0 || j == i1 || j == i2)
          continue;
        const UV &p = uv[j];
        if (cross2(a, b, p) >= 0.0 && cross2(b, c, p) >= 0.0 && cross2(c, a, p) >= 0.0)
        {
          inside = true;
          break;
        }
      }
      if (inside)
        continue;
      out.push_back(Tri{i0, i1, i2});
      idx.erase(idx.begin() + k);
      found = true;
      break;
    }
    if (!found)
      return false;
  }
  out.push_back(Tri{idx[0], idx[1], idx[2]});
  return true;
}

// 3D distance from point p to segment ab.
double seg_dist(const XYZ &p, const XYZ &a, const XYZ &b)
{
  const double abx = b.x - a.x, aby = b.y - a.y, abz = b.z - a.z;
  const double apx = p.x - a.x, apy = p.y - a.y, apz = p.z - a.z;
  const double ll = abx * abx + aby * aby + abz * abz;
  double t = 0.0;
  if (ll != 0.0)
    t = std::max(0.0, std::min(1.0, (apx * abx + apy * aby + apz * abz) / ll));
  const double dx = apx - t * abx, dy = apy - t * aby, dz = apz - t * abz;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

std::vector<int> slice(const std::vector<int> &v, size_t a, size_t b)
{
  a = std::min(a, v.size());
  b = std::min(b, v.size());
  return a < b ? std::vector<int>(v.begin() + a, v.begin() + b) : std::vector<int>();
}

std::vector<int> cat(std::vector<int> a, const std::vector<int> &b)
{
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

// Triangulate the closed outline `ids` into `out`; false on failure. At the
// first crossing of two segments the end lying closest (3D) to the OTHER
// segment is the pivot: that other segment is replaced by the path through the
// pivot and closed by ONE sliver triangle, and the outline falls apart at the
// pivot into two lobes. No new node: every boundary segment stays the one the
// neighbour holds.
bool fill_outline(const std::vector<int> &L, std::map<int, UV> &uv,
                  std::map<int, XYZ> &xyz, std::vector<Tri> &out)
{
  const size_t n = L.size();
  if (n < 3)
    return true;                      // a two-node lobe: its chord IS a segment
  for (size_t i = 0; i < n; ++i)
    for (size_t j = i + 2; j < n; ++j)
    {
      if (i == 0 && j == n - 1)
        continue;
      const size_t i1 = i + 1, j1 = (j + 1) % n;
      if (!crossing(uv[L[i]], uv[L[i1]], uv[L[j]], uv[L[j1]]))
        continue;
      // python's min() over (distance, name): ties go to "i" < "i1" < "j" < "j1"
      const std::pair<double, std::string> cand[4] = {
          {seg_dist(xyz[L[i1]], xyz[L[j]], xyz[L[j1]]), "i1"},
          {seg_dist(xyz[L[i]], xyz[L[j]], xyz[L[j1]]), "i"},
          {seg_dist(xyz[L[j1]], xyz[L[i]], xyz[L[i1]]), "j1"},
          {seg_dist(xyz[L[j]], xyz[L[i]], xyz[L[i1]]), "j"}};
      std::pair<double, std::string> best = cand[0];
      for (int k = 1; k < 4; ++k)
        if (cand[k] < best)
          best = cand[k];
      std::vector<int> a, b;
      if (best.second == "i1")        // s_j becomes j -> i+1 -> j+1
      {
        out.push_back(Tri{L[j], L[i1], L[j1]});
        a = slice(L, i1, j + 1);
        b = cat(cat({L[i1]}, slice(L, j + 1, n)), slice(L, 0, i + 1));
      }
      else if (best.second == "i")    // s_j becomes j -> i -> j+1
      {
        out.push_back(Tri{L[j], L[i], L[j1]});
        a = slice(L, i, j + 1);
        b = cat(cat({L[i]}, slice(L, j + 1, n)), slice(L, 0, i));
      }
      else if (best.second == "j1")   // s_i becomes i -> j+1 -> i+1
      {
        out.push_back(Tri{L[i], L[j1], L[i1]});
        a = cat({L[j1]}, slice(L, i1, j + 1));
        b = cat(slice(L, j + 1, n), slice(L, 0, i + 1));
      }
      else                            // s_i becomes i -> j -> i+1
      {
        out.push_back(Tri{L[i], L[j], L[i1]});
        a = cat({L[j]}, slice(L, i1, j));
        b = cat(slice(L, j, n), slice(L, 0, i + 1));
      }
      return fill_outline(a, uv, xyz, out) && fill_outline(b, uv, xyz, out);
    }
  return ear_clip(L, uv, out);
}

double tris_area(const std::vector<Tri> &tris, std::map<int, XYZ> &xyz)
{
  double s = 0.0;
  for (const Tri &t : tris)
  {
    const XYZ &p = xyz[t[0]], &q = xyz[t[1]], &r = xyz[t[2]];
    const double ux = q.x - p.x, uy = q.y - p.y, uz = q.z - p.z;
    const double vx = r.x - p.x, vy = r.y - p.y, vz = r.z - p.z;
    s += 0.5 * std::sqrt((uy * vz - uz * vy) * (uy * vz - uz * vy) +
                         (uz * vx - ux * vz) * (uz * vx - ux * vz) +
                         (ux * vy - uy * vx) * (ux * vy - uy * vx));
  }
  return s;
}

// Put `tris` on `face` as its new triangulation over the outline nodes, and
// re-point every edge polygon at it (same nodes, new numbering).
void install(const TopoDS_Face &face, const Handle(Poly_Triangulation) & tri,
             const TopLoc_Location &loc, const std::vector<int> &ids,
             const std::vector<Tri> &tris)
{
  const std::set<int> order(ids.begin(), ids.end());
  std::map<int, int> num;
  int k = 0;
  for (int old : order)
    num[old] = ++k;
  Handle(Poly_Triangulation) nt =
      new Poly_Triangulation(static_cast<int>(order.size()),
                             static_cast<int>(tris.size()), Standard_True);
  for (const auto &kv : num)
  {
    nt->SetNode(kv.second, tri->Node(kv.first));
    nt->SetUVNode(kv.second, tri->UVNode(kv.first));
  }
  k = 0;
  for (const Tri &t : tris)
    nt->SetTriangle(++k, Poly_Triangle(num[t[0]], num[t[1]], num[t[2]]));
  nt->Deflection(tri->Deflection());
  BRep_Builder bb;
  for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next())
  {
    const TopoDS_Edge edge = TopoDS::Edge(ex.Current());
    const Handle(Poly_PolygonOnTriangulation) pol =
        BRep_Tool::PolygonOnTriangulation(edge, tri, loc);
    TColStd_Array1OfInteger nodes(1, pol->NbNodes());
    for (int j = 1; j <= pol->NbNodes(); ++j)
      nodes.SetValue(j, num[pol->Nodes().Value(j)]);
    Handle(Poly_PolygonOnTriangulation) npol =
        pol->HasParameters() ? new Poly_PolygonOnTriangulation(nodes, pol->Parameters()->Array1())
                             : new Poly_PolygonOnTriangulation(nodes);
    npol->Deflection(pol->Deflection());
    bb.UpdateEdge(edge, npol, nt, loc);
  }
  bb.UpdateFace(face, nt);
}

// Re-triangulate `face` when its mesh does not close over its own edge
// polygons. True (with the areas) on a repair.
bool repair_face(const TopoDS_Face &face, double &a0, double &a1, double &cad)
{
  TopLoc_Location loc;
  const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
  if (tri.IsNull() || tri->NbTriangles() == 0 || !tri->HasUVNodes())
    return false;
  std::set<Seg> segs;
  if (!edge_segments(face, tri, loc, segs) || segs.empty())
    return false;
  const std::vector<Tri> old = face_tris(tri);
  if (closes(old, segs))
    return false;                     // the mesher closed it: never touched
  std::vector<int> ids;
  if (!outline(face, tri, loc, ids) || static_cast<int>(ids.size()) > REPAIR_MAX_NODES)
    return false;
  std::map<int, UV> uv;
  for (int i : ids)
  {
    const gp_Pnt2d p = tri->UVNode(i);
    uv[i] = UV{p.X(), p.Y()};
  }
  std::map<int, XYZ> xyz;
  std::set<int> all(ids.begin(), ids.end());
  for (const Tri &t : old)
    all.insert(t.begin(), t.end());
  for (int i : all)
  {
    const gp_Pnt p = tri->Node(i);
    xyz[i] = XYZ{p.X(), p.Y(), p.Z()};
  }
  std::vector<Tri> fresh;
  if (!fill_outline(ids, uv, xyz, fresh) || !closes(fresh, segs))
    return false;
  GProp_GProps gp;
  BRepGProp::SurfaceProperties(face, gp);
  const double sf = loc.Transformation().ScaleFactor();
  cad = gp.Mass() / (sf * sf);
  a0 = tris_area(old, xyz);
  a1 = tris_area(fresh, xyz);
  if (cad <= 0.0 || std::fabs(a1 / cad - 1.0) >= std::fabs(a0 / cad - 1.0))
    return false;                     // judged by the result, never by hope
  install(face, tri, loc, ids, fresh);
  return true;
}
} // namespace

int repair_holed_faces(const TopoDS_Shape &shape,
                       const std::function<int(const TopoDS_Face &)> &eid_of,
                       TopTools_IndexedMapOfShape &done)
{
  int n = 0;
  for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next())
  {
    const TopoDS_Face face = TopoDS::Face(ex.Current());
    const TopoDS_Shape key = face.Located(TopLoc_Location());
    if (done.Contains(key))
      continue;
    done.Add(key);
    double a0 = 0.0, a1 = 0.0, cad = 0.0;
    if (!repair_face(face, a0, a1, cad))
      continue;
    ++n;
    const int eid = eid_of(face);
    const std::string id = eid ? std::to_string(eid) : std::string("?");
    std::printf("hc-cadconv: face #%s (%s) was meshed with holes — re-triangulated "
                "from its own boundary (covers %.0f%% -> %.0f%% of its area)\n",
                id.c_str(), face_kind(face), 100.0 * a0 / cad, 100.0 * a1 / cad);
  }
  return n;
}
