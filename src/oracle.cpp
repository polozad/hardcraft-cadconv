#include "oracle.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRep_Tool.hxx>
#include <Geom_BSplineSurface.hxx>
#include <Standard_Failure.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Edge.hxx>
#include <TopoDS_Vertex.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
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
  BRepAdaptor_Surface ad(face);
  if (ad.IsUClosed())
    rec.pu = std::fabs(ad.LastUParameter() - ad.FirstUParameter());
  if (ad.IsVClosed())
    rec.pv = std::fabs(ad.LastVParameter() - ad.FirstVParameter());
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

std::vector<EdgeRecord> edge_oracle(const TopoDS_Shape &shape)
{
  TopTools_IndexedMapOfShape emap;
  TopExp::MapShapes(shape, TopAbs_EDGE, emap);
  std::vector<EdgeRecord> out;
  for (int i = 1; i <= emap.Extent(); ++i)
  {
    const TopoDS_Edge edge = TopoDS::Edge(emap.FindKey(i));
    if (BRep_Tool::Degenerated(edge))
      continue;                     // apex/seam degenerates carry no curve
    EdgeRecord rec;
    try
    {
      BRepAdaptor_Curve ad(edge);
      rec.kind = curve_kind(ad.GetType());
      // endpoints = the BREP vertex points, i.e. what the mesher pins its
      // boundary nodes to
      const gp_Pnt p0 = BRep_Tool::Pnt(TopExp::FirstVertex(edge));
      const gp_Pnt p1 = BRep_Tool::Pnt(TopExp::LastVertex(edge));
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
    }
    catch (const Standard_Failure &)
    {
      continue;                     // one mute edge drops one record only
    }
    out.push_back(rec);
  }
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
