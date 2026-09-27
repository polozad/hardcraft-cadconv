#include "oracle.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Curve2d.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepTools.hxx>
#include <BRepTools_WireExplorer.hxx>
#include <BRep_Tool.hxx>
#include <Geom2d_Curve.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedDataMapOfShapeListOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <TopoDS_Wire.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Elips.hxx>
#include <gp_Dir.hxx>
#include <gp_Pnt.hxx>
#include <gp_Pnt2d.hxx>

double ORACLE_SCALE = 0.001;   // pre-convert() placeholder; set per file

// python's round(x, nd), exactly: correctly-rounded DECIMAL rounding of the true
// binary value. The obvious nearbyint(v * 1e6) / 1e6 is NOT the same thing — the
// multiplication moves the value across the tie (measured on live striker.stp:
// one UV came out 1.649146 against the reference's 1.649147), so the round trip
// goes through the printf/strtod pair, which is correctly rounded on both ends.
static double round_to(double v, int nd)
{
  char b[64];
  std::snprintf(b, sizeof(b), "%.*f", nd, v);
  return std::strtod(b, nullptr);
}

static const char *surf_kind(GeomAbs_SurfaceType t)
{
  switch (t)
  {
  case GeomAbs_Plane:               return "plane";
  case GeomAbs_Cylinder:            return "cylinder";
  case GeomAbs_Cone:                return "cone";
  case GeomAbs_Sphere:              return "sphere";
  case GeomAbs_Torus:               return "torus";
  case GeomAbs_BSplineSurface:      return "bspline";
  case GeomAbs_BezierSurface:       return "bspline";
  case GeomAbs_SurfaceOfRevolution: return "revolution";
  case GeomAbs_SurfaceOfExtrusion:  return "extrusion";
  case GeomAbs_OffsetSurface:       return "offset";
  default:                          return "?";
  }
}

static const char *curve_kind(GeomAbs_CurveType t)
{
  switch (t)
  {
  case GeomAbs_Line:        return "line";
  case GeomAbs_Circle:      return "circle";
  case GeomAbs_Ellipse:     return "ellipse";
  case GeomAbs_BSplineCurve: return "bspline";
  case GeomAbs_BezierCurve: return "bspline";
  default:                  return "?";
  }
}

// Exact-double token for clone signatures: copy-paste clones parse to bit-equal
// doubles and this format round-trips them. %.17g under the C locale (main()
// pins it) — a comma-decimal locale would make two clones look different.
static void tok(std::string &out, double x)
{
  char b[32];
  std::snprintf(b, sizeof(b), "%.17g", x);
  out += b;
  out += '|';
}

static void tok_i(std::string &out, int x)
{
  out += std::to_string(x);
  out += '|';
}

// Clone signature of a face's surface, or "" (this kind has no signature).
// Bit-equal defining data: type + full placement + radii, bspline poles/weights/
// knots included. Genuinely distinct features never collide (their placement
// origin differs).
static std::string surf_sig(BRepAdaptor_Surface &ad)
{
  const GeomAbs_SurfaceType t = ad.GetType();
  std::string s;
  if (t == GeomAbs_Plane || t == GeomAbs_Cylinder || t == GeomAbs_Cone
      || t == GeomAbs_Sphere || t == GeomAbs_Torus)
  {
    gp_Ax3 pos;
    switch (t)
    {
    case GeomAbs_Plane:    pos = ad.Plane().Position(); break;
    case GeomAbs_Cylinder: pos = ad.Cylinder().Position(); break;
    case GeomAbs_Cone:     pos = ad.Cone().Position(); break;
    case GeomAbs_Sphere:   pos = ad.Sphere().Position(); break;
    default:               pos = ad.Torus().Position(); break;
    }
    const gp_Pnt L = pos.Location();
    const gp_Dir D = pos.Direction();
    const gp_Dir X = pos.XDirection();
    tok_i(s, static_cast<int>(t));
    tok(s, L.X()); tok(s, L.Y()); tok(s, L.Z());
    tok(s, D.X()); tok(s, D.Y()); tok(s, D.Z());
    tok(s, X.X()); tok(s, X.Y()); tok(s, X.Z());
    if (t == GeomAbs_Cylinder)
      tok(s, ad.Cylinder().Radius());
    else if (t == GeomAbs_Sphere)
      tok(s, ad.Sphere().Radius());
    else if (t == GeomAbs_Cone)
    {
      tok(s, ad.Cone().RefRadius());
      tok(s, ad.Cone().SemiAngle());
    }
    else if (t == GeomAbs_Torus)
    {
      tok(s, ad.Torus().MajorRadius());
      tok(s, ad.Torus().MinorRadius());
    }
    return s;
  }
  if (t == GeomAbs_BSplineSurface)
  {
    const Handle(Geom_BSplineSurface) bs = ad.BSpline();
    if (bs.IsNull())
      return std::string();
    s = "bs|";
    tok_i(s, bs->UDegree());
    tok_i(s, bs->VDegree());
    for (int i = 1; i <= bs->NbUPoles(); ++i)
      for (int j = 1; j <= bs->NbVPoles(); ++j)
      {
        const gp_Pnt p = bs->Pole(i, j);
        tok(s, p.X()); tok(s, p.Y()); tok(s, p.Z());
        tok(s, bs->Weight(i, j));
      }
    for (int i = 1; i <= bs->NbUKnots(); ++i)
    {
      tok(s, bs->UKnot(i));
      tok_i(s, bs->UMultiplicity(i));
    }
    for (int i = 1; i <= bs->NbVKnots(); ++i)
    {
      tok(s, bs->VKnot(i));
      tok_i(s, bs->VMultiplicity(i));
    }
    return s;
  }
  return std::string();
}

// (pu, pv): the parametric period of each axis the FACE wraps (0 = open).
// Mirror of step2glb.py `_closed_periods`. `IsUClosed` alone is the
// trim-match trap `face_seams` already left (9d26de5): it compares the face's
// trimmed range with the surface's bounds inside PConfusion (1e-9), and a
// wrapped face's trim can UNDERSHOOT that (slide.stp face 72: u[-1.5e-9, 2pi])
// or OVERSHOOT it (striker.stp face 68: u[0, 6.283221], 3.6e-5 past 2pi) —
// both answered «open» and the period table lost a full cylinder. A closed
// answer keeps its legacy value. Else the authority is topological, as in
// `face_seams`: the face wraps iff its wire carries a seam edge
// (`BRep_Tool::IsClosed(edge, face)`); the wrapped axis is the periodic one
// whose trim spans a full period (±1e-4 of it), and the period is the
// SURFACE's own, not the trimmed width.
static void closed_periods(const TopoDS_Face &face, double &pu, double &pv)
{
  BRepAdaptor_Surface ad(face);
  const double us = std::fabs(ad.LastUParameter() - ad.FirstUParameter());
  const double vs = std::fabs(ad.LastVParameter() - ad.FirstVParameter());
  pu = ad.IsUClosed() ? us : 0.0;
  pv = ad.IsVClosed() ? vs : 0.0;
  if ((pu != 0.0 && pv != 0.0) || !(ad.IsUPeriodic() || ad.IsVPeriodic()))
    return;
  TopTools_IndexedMapOfShape emap;
  TopExp::MapShapes(face, TopAbs_EDGE, emap);
  bool seam = false;
  for (int i = 1; i <= emap.Extent() && !seam; ++i)
  {
    const TopoDS_Edge edge = TopoDS::Edge(emap.FindKey(i));
    seam = !BRep_Tool::Degenerated(edge) && BRep_Tool::IsClosed(edge, face);
  }
  if (!seam)
    return;
  if (pu == 0.0 && ad.IsUPeriodic())
  {
    const double per = ad.UPeriod();
    if (std::fabs(us - per) <= 1e-4 * per)
      pu = per;
  }
  if (pv == 0.0 && ad.IsVPeriodic())
  {
    const double per = ad.VPeriod();
    if (std::fabs(vs - per) <= 1e-4 * per)
      pv = per;
  }
}

UvRecord face_uv_record(const TopoDS_Face &face,
                        const Handle(Poly_Triangulation) & tri)
{
  UvRecord rec;
  if (tri.IsNull() || !tri->HasUVNodes())
    return rec;                     // absent = the consumer drops the channel
  rec.present = true;
  rec.n = tri->NbNodes();
  rec.uv.reserve(static_cast<size_t>(rec.n) * 2);
  for (int i = 1; i <= rec.n; ++i)
  {
    const gp_Pnt2d p = tri->UVNode(i);
    rec.uv.push_back(round_to(p.X(), 6));
    rec.uv.push_back(round_to(p.Y(), 6));
  }
  closed_periods(face, rec.pu, rec.pv);
  return rec;
}

// PROTOCOL 3 placement — the numeric twin of `surf_sig`'s defining data.
static void fill_geom(BRepAdaptor_Surface &ad, GeomAbs_SurfaceType t,
                      FaceGeom &g)
{
  auto put = [](double *dst, const gp_Pnt &p, double k) {
    dst[0] = p.X() * k; dst[1] = p.Y() * k; dst[2] = p.Z() * k;
  };
  auto putd = [](double *dst, const gp_Dir &d) {
    dst[0] = d.X(); dst[1] = d.Y(); dst[2] = d.Z();
  };
  if (t == GeomAbs_Plane || t == GeomAbs_Cylinder || t == GeomAbs_Cone
      || t == GeomAbs_Sphere || t == GeomAbs_Torus)
  {
    gp_Ax3 pos;
    switch (t)
    {
    case GeomAbs_Plane:    pos = ad.Plane().Position(); break;
    case GeomAbs_Cylinder: pos = ad.Cylinder().Position(); break;
    case GeomAbs_Cone:     pos = ad.Cone().Position(); break;
    case GeomAbs_Sphere:   pos = ad.Sphere().Position(); break;
    default:               pos = ad.Torus().Position(); break;
    }
    put(g.o, pos.Location(), ORACLE_SCALE);   g.has_o = true;
    putd(g.d, pos.Direction());               g.has_d = true;
    putd(g.x, pos.XDirection());              g.has_x = true;
    if (t == GeomAbs_Cylinder)
    {
      g.rmaj = ad.Cylinder().Radius() * ORACLE_SCALE; g.has_rmaj = true;
    }
    else if (t == GeomAbs_Sphere)
    {
      g.rmaj = ad.Sphere().Radius() * ORACLE_SCALE; g.has_rmaj = true;
    }
    else if (t == GeomAbs_Cone)
    {
      g.rmaj = ad.Cone().RefRadius() * ORACLE_SCALE; g.has_rmaj = true;
      g.semi = ad.Cone().SemiAngle();                g.has_semi = true;
    }
    else if (t == GeomAbs_Torus)
    {
      g.rmaj = ad.Torus().MajorRadius() * ORACLE_SCALE; g.has_rmaj = true;
      g.rmin = ad.Torus().MinorRadius() * ORACLE_SCALE; g.has_rmin = true;
    }
  }
  else if (t == GeomAbs_SurfaceOfExtrusion)
  {
    putd(g.d, ad.Direction());                g.has_d = true;
  }
  else if (t == GeomAbs_SurfaceOfRevolution)
  {
    const gp_Ax1 ax = ad.AxeOfRevolution();
    put(g.o, ax.Location(), ORACLE_SCALE);    g.has_o = true;
    putd(g.d, ax.Direction());                g.has_d = true;
  }
}

FaceRecord face_oracle(const TopoDS_Face &face, CloneSigs &sigs,
                       std::vector<std::string> &sig_order, int idx)
{
  BRepAdaptor_Surface ad(face);
  const GeomAbs_SurfaceType t = ad.GetType();
  FaceRecord rec;
  rec.kind = surf_kind(t);
  if (t == GeomAbs_Cylinder)
  {
    rec.has_r = true;
    rec.r = ad.Cylinder().Radius() * ORACLE_SCALE;
  }
  else if (t == GeomAbs_Sphere)
  {
    rec.has_r = true;
    rec.r = ad.Sphere().Radius() * ORACLE_SCALE;
  }
  else if (t == GeomAbs_Cone)
  {
    rec.has_r = true;
    rec.r = ad.Cone().RefRadius() * ORACLE_SCALE;
  }
  try
  {
    fill_geom(ad, t, rec.g);
  }
  catch (const Standard_Failure &)
  {
    rec.g = FaceGeom();        // a mute surface loses its placement, not the run
  }
  try
  {
    rec.orient_rev = (face.Orientation() == TopAbs_REVERSED);
    rec.has_orient = true;
  }
  catch (const Standard_Failure &)
  {
    rec.has_orient = false;         // no flag = unknown, never a wrong sign
  }
  try
  {
    const double bb[4] = {ad.FirstUParameter(), ad.LastUParameter(),
                          ad.FirstVParameter(), ad.LastVParameter()};
    bool fin = true;
    for (double q : bb)
      if (!std::isfinite(q))
        fin = false;
    if (fin)                        // an unbounded (infinite) trim is no fact
    {
      for (int k = 0; k < 4; ++k)
        rec.b[k] = bb[k];
      rec.has_bounds = true;
    }
  }
  catch (const Standard_Failure &)
  {
    rec.has_bounds = false;
  }
  std::string sig;
  try
  {
    sig = surf_sig(ad);
  }
  catch (const Standard_Failure &)
  {
    sig.clear();                    // a mute surface loses cloning, not the run
  }
  if (!sig.empty())
  {
    auto it = sigs.find(sig);
    if (it == sigs.end())
    {
      // insertion order is preserved separately: the python's dict order is
      // what puts the clone groups into the sidecar, and a parity diff against
      // it should not have to sort.
      sigs.emplace(sig, std::vector<int>{idx});
      sig_order.push_back(sig);
    }
    else
      it->second.push_back(idx);
  }
  return rec;
}

// How densely a CURVED pcurve is sampled. 16 is `face_seams`'s count for the 3D
// seam trace on purpose: the two traces of one seam edge then have the same
// station count and can be compared point for point. A 2D LINE is exact at 2
// points and takes them (most of a hard-surface model's pcurves are lines: a
// ruling on a cylinder, a rim at v=const, any edge of a plane).
static const int PCURVE_SAMPLES = 16;

// One (u,v) polyline of `edge` on `face`, in the face's RAW OCC parameters —
// or false when the edge has no curve on that surface.
//
// Sampled, not analytic, and deliberately so: the consumer walks a POLYLINE
// against the mesh boundary, and handing it a line here and a circle there
// would make every reader carry an evaluator per 2D curve type. Rounded to 6
// decimals, the SAME quantum the uv chart this trace has to land in uses.
static bool pcurve_trace(const TopoDS_Edge &edge, const TopoDS_Face &face,
                         std::vector<double> &out)
{
  out.clear();
  try
  {
    Standard_Real cf = 0.0, cl = 0.0;
    const Handle(Geom2d_Curve) c = BRep_Tool::CurveOnSurface(edge, face, cf, cl);
    if (c.IsNull())
      return false;                 // no pcurve here — absent, not wrong
    BRepAdaptor_Curve2d c2(edge, face);
    const double t0 = c2.FirstParameter(), t1 = c2.LastParameter();
    if (!(std::isfinite(t0) && std::isfinite(t1)))
      return false;
    const int n = (c2.GetType() == GeomAbs_Line) ? 1 : PCURVE_SAMPLES;
    for (int k = 0; k <= n; ++k)
    {
      const gp_Pnt2d q = c2.Value(t0 + (t1 - t0) * k / static_cast<double>(n));
      if (!(std::isfinite(q.X()) && std::isfinite(q.Y())))
        return false;
      out.push_back(round_to(q.X(), 6));
      out.push_back(round_to(q.Y(), 6));
    }
  }
  catch (const Standard_Failure &)
  {
    out.clear();
    return false;                   // one mute pcurve costs one trace only
  }
  return !out.empty();
}

// `edges[].p` — the edge's (u,v) trace on each adjacent face. The value is a
// LIST of traces per face, and its length is exactly how many times the edge
// appears in that face's wires (`faces[].w`): once normally, TWICE on a
// periodic face's SEAM edge, where OCC stores two pcurves — one at each end of
// the period — and picks between them by the edge's ORIENTATION. FORWARD
// first, then REVERSED, so the order is the file's and not the traversal's.
//
// UNITS: raw OCC parameters, NOT scaled by ORACLE_SCALE — the same space as
// `faces[].b` and the uv chart (a plane's u/v are OCC mm; a cylinder's u is
// radians, and one factor cannot be right for both).
static void edge_pcurves(const TopoDS_Edge &edge,
                         const std::map<int, TopoDS_Face> &adj,
                         std::vector<EdgePcurve> &out)
{
  for (const auto &kv : adj)
  {
    const TopoDS_Face &face = kv.second;
    bool seam = false;
    try
    {
      seam = (BRep_Tool::IsClosed(edge, face) == Standard_True);
    }
    catch (const Standard_Failure &)
    {
      seam = false;
    }
    TopAbs_Orientation ors[2];
    int nors = 1;
    if (seam)
    {
      ors[0] = TopAbs_FORWARD;
      ors[1] = TopAbs_REVERSED;
      nors = 2;
    }
    else
      ors[0] = edge.Orientation();  // only one representation to find
    EdgePcurve rec;
    rec.face = kv.first;
    for (int i = 0; i < nors; ++i)
    {
      TopoDS_Edge eo;
      try
      {
        eo = TopoDS::Edge(edge.Oriented(ors[i]));
      }
      catch (const Standard_Failure &)
      {
        continue;
      }
      std::vector<double> t;
      if (pcurve_trace(eo, face, t))
        rec.traces.push_back(t);
    }
    if (!rec.traces.empty())
      out.push_back(rec);
  }
}

void block_verts(const TopoDS_Shape &shape, std::vector<VertRecord> &pts,
                 TopTools_IndexedMapOfShape &vmap)
{
  pts.clear();
  vmap.Clear();
  TopExp::MapShapes(shape, TopAbs_VERTEX, vmap);
  const double s = ORACLE_SCALE;
  for (int i = 1; i <= vmap.Extent(); ++i)
  {
    VertRecord v;
    try
    {
      const gp_Pnt p = BRep_Tool::Pnt(TopoDS::Vertex(vmap.FindKey(i)));
      v.p[0] = p.X() * s; v.p[1] = p.Y() * s; v.p[2] = p.Z() * s;
      v.ok = true;
    }
    catch (const Standard_Failure &)
    {
      v.ok = false;                 // a mute 0-cell keeps the INDEXING honest
    }
    pts.push_back(v);
  }
}

EdgeOracle edge_oracle(const TopoDS_Shape &shape,
                       const std::vector<TopoDS_Face> *faces, int f0, int v0,
                       const TopTools_IndexedMapOfShape *vmap)
{
  TopTools_IndexedMapOfShape emap;
  TopExp::MapShapes(shape, TopAbs_EDGE, emap);
  // the block's faces as a LOCAL index, so an ancestor can be named globally
  TopTools_IndexedMapOfShape fmap;
  if (faces)
    for (const TopoDS_Face &f : *faces)
      fmap.Add(f);
  TopTools_IndexedDataMapOfShapeListOfShape amap;
  bool has_amap = false;
  if (faces && !faces->empty())
  {
    try
    {
      TopExp::MapShapesAndAncestors(shape, TopAbs_EDGE, TopAbs_FACE, amap);
      has_amap = true;
    }
    catch (const Standard_Failure &)
    {
      has_amap = false;             // no adjacency = one absent key, not a fail
    }
  }
  EdgeOracle out;
  for (int i = 1; i <= emap.Extent(); ++i)
  {
    const TopoDS_Edge edge = TopoDS::Edge(emap.FindKey(i));
    if (BRep_Tool::Degenerated(edge))
      continue;                     // apex/seam degenerates carry no curve
    EdgeRecord rec;
    TopoDS_Vertex va, vb;
    try
    {
      BRepAdaptor_Curve ad(edge);
      rec.kind = curve_kind(ad.GetType());
      // endpoints = the BREP vertex points, i.e. what the mesher pins its
      // boundary nodes to
      va = TopExp::FirstVertex(edge);
      vb = TopExp::LastVertex(edge);
      const gp_Pnt p0 = BRep_Tool::Pnt(va);
      const gp_Pnt p1 = BRep_Tool::Pnt(vb);
      const double s = ORACLE_SCALE;
      rec.a[0] = p0.X() * s; rec.a[1] = p0.Y() * s; rec.a[2] = p0.Z() * s;
      rec.b[0] = p1.X() * s; rec.b[1] = p1.Y() * s; rec.b[2] = p1.Z() * s;
      if (rec.kind == "circle")
      {
        const gp_Circ c = ad.Circle();
        rec.has_circle = true;
        rec.r = c.Radius() * s;
        const gp_Pnt o = c.Location();
        const gp_Dir ax = c.Axis().Direction();
        rec.c[0] = o.X() * s; rec.c[1] = o.Y() * s; rec.c[2] = o.Z() * s;
        rec.ax[0] = ax.X(); rec.ax[1] = ax.Y(); rec.ax[2] = ax.Z();
      }
      else if (rec.kind == "ellipse")
      {
        // typed and thrown away until 2026-09-18 — the same four lines the
        // circle branch has, plus the minor radius and the major-axis
        // direction an ellipse needs to be one
        try
        {
          const gp_Elips el = ad.Ellipse();
          const gp_Pnt o = el.Location();
          const gp_Dir ax = el.Axis().Direction();
          const gp_Dir xd = el.XAxis().Direction();   // the MAJOR axis
          rec.has_circle = true;
          rec.has_ellipse = true;
          rec.r = el.MajorRadius() * s;
          rec.r2 = el.MinorRadius() * s;
          rec.c[0] = o.X() * s; rec.c[1] = o.Y() * s; rec.c[2] = o.Z() * s;
          rec.ax[0] = ax.X(); rec.ax[1] = ax.Y(); rec.ax[2] = ax.Z();
          rec.xd[0] = xd.X(); rec.xd[1] = xd.Y(); rec.xd[2] = xd.Z();
        }
        catch (const Standard_Failure &)
        {
          rec.has_circle = false;
          rec.has_ellipse = false;  // typed ellipse without numbers, as before
        }
      }
    }
    catch (const Standard_Failure &)
    {
      continue;                     // one mute edge drops one record only
    }
    if (has_amap)
    {
      try
      {
        std::map<int, TopoDS_Face> seen;   // ascending + deduped by construction
        const TopTools_ListOfShape &anc = amap.FindFromKey(edge);
        for (TopTools_ListOfShape::Iterator it(anc); it.More(); it.Next())
        {
          const int j = fmap.FindIndex(it.Value());
          if (j)                    // a face outside this block cannot be named
            seen.emplace(f0 + j - 1, TopoDS::Face(it.Value()));
        }
        if (!seen.empty())
        {
          for (const auto &kv : seen)
            rec.f.push_back(kv.first);
          // the pcurves ride the SAME adjacency (one traversal, one answer):
          // `p`'s faces are `f`'s entries
          edge_pcurves(edge, seen, rec.p);
        }
      }
      catch (const Standard_Failure &)
      {
        rec.f.clear();
        rec.p.clear();
      }
    }
    if (vmap)
    {
      try
      {
        const int ia = vmap->FindIndex(va), ib = vmap->FindIndex(vb);
        if (ia && ib)
        {
          rec.v[0] = v0 + ia - 1;
          rec.v[1] = v0 + ib - 1;
          rec.has_v = true;
        }
      }
      catch (const Standard_Failure &)
      {
        rec.has_v = false;
      }
    }
    out.recs.push_back(rec);
    out.recmap.Add(edge);
  }
  return out;
}

FaceWires face_wires(const TopoDS_Face &face,
                     const TopTools_IndexedMapOfShape &recmap, int e0)
{
  TopoDS_Shape outer;
  try
  {
    outer = BRepTools::OuterWire(face);
  }
  catch (const Standard_Failure &)
  {
    outer = TopoDS_Shape();
  }

  // the face-aware WireExplorer ctor, or a seam wire mis-orders
  auto loop = [&](const TopoDS_Wire &w) {
    std::vector<int> idx;
    try
    {
      for (BRepTools_WireExplorer ex(w, face); ex.More(); ex.Next())
      {
        const int j = recmap.FindIndex(ex.Current());
        if (j)                      // no record (degenerate / mute) = omitted
          idx.push_back(e0 + j - 1);
      }
    }
    catch (const Standard_Failure &)
    {
      return idx;
    }
    return idx;
  };

  FaceWires out;
  bool got_outer = false;
  std::vector<std::vector<int>> inner;
  try
  {
    for (TopExp_Explorer ex(face, TopAbs_WIRE); ex.More(); ex.Next())
    {
      const TopoDS_Wire w = TopoDS::Wire(ex.Current());
      std::vector<int> l = loop(w);
      if (!outer.IsNull() && w.IsSame(outer))
      {
        out.outer = l;
        got_outer = true;
      }
      else
        inner.push_back(l);
    }
  }
  catch (const Standard_Failure &)
  {
    return FaceWires();             // absent key, never a wrong wire
  }
  if (!got_outer && inner.empty())
    return FaceWires();
  out.inner = inner;
  out.present = true;
  return out;
}

std::vector<SeamRecord> face_seams(const TopoDS_Face &face, int idx)
{
  std::vector<SeamRecord> out;
  try
  {
    BRepAdaptor_Surface ad(face);
    // PERIODICITY, not «closed» (mirror of step2glb.py `_face_seams`,
    // 9d26de5): `BRepAdaptor_Surface::IsUClosed` compares the FACE's trimmed
    // u-range with the surface's natural bounds inside `Precision::PConfusion()`
    // (1e-9), so a full-period cylinder whose trim carries a few 1e-9 of
    // parametric dust answers False and loses its seam silently (live
    // D:/p320/slide.stp face 72, the tube wall: u[-1.538e-9 .. 2pi] — 3 of that
    // file's 16 seam-bearing cylinders were dropped this way). Periodicity is a
    // property of the SURFACE and carries no trim dust; the authoritative test
    // is the loop's own `BRep_Tool::IsClosed(edge, face)` below, untouched — a
    // periodic surface trimmed to a mere sector simply has no such edge and
    // still emits nothing. Additive: the record shape is unchanged.
    if (!(ad.IsUClosed() || ad.IsVClosed() ||
          ad.IsUPeriodic() || ad.IsVPeriodic()))
      return out;
  }
  catch (const Standard_Failure &)
  {
    return out;
  }
  // dedup: the seam sits in the wire TWICE, and is mapped once
  TopTools_IndexedMapOfShape emap;
  TopExp::MapShapes(face, TopAbs_EDGE, emap);
  for (int i = 1; i <= emap.Extent(); ++i)
  {
    const TopoDS_Edge edge = TopoDS::Edge(emap.FindKey(i));
    if (BRep_Tool::Degenerated(edge))
      continue;                     // cone apex / sphere pole: no 3D curve
    const int n = 16;
    std::vector<double> pts;
    try
    {
      if (!BRep_Tool::IsClosed(edge, face))
        continue;
      BRepAdaptor_Curve c(edge);
      const double f0 = c.FirstParameter(), l0 = c.LastParameter();
      pts.reserve(3 * (n + 1));
      for (int k = 0; k <= n; ++k)
      {
        const gp_Pnt p = c.Value(f0 + (l0 - f0) * k / static_cast<double>(n));
        pts.push_back(round_to(p.X() * ORACLE_SCALE, 9));
        pts.push_back(round_to(p.Y() * ORACLE_SCALE, 9));
        pts.push_back(round_to(p.Z() * ORACLE_SCALE, 9));
      }
    }
    catch (const Standard_Failure &)
    {
      continue;                     // one mute seam drops one trace only
    }
    double len = 0.0;
    for (int k = 0; k < n; ++k)
    {
      const double dx = pts[3 * k + 3] - pts[3 * k + 0];
      const double dy = pts[3 * k + 4] - pts[3 * k + 1];
      const double dz = pts[3 * k + 5] - pts[3 * k + 2];
      len += std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    if (len < 1e-9)
      continue;                     // zero-length seam (degenerate wrap)
    SeamRecord rec;
    rec.face = idx;
    rec.pts.swap(pts);
    out.push_back(rec);
  }
  return out;
}
