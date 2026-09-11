#include "refine.h"
#include "parts.h"

#include <cstdio>

#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepTools.hxx>
#include <BRep_Tool.hxx>
#include <Poly_Triangulation.hxx>
#include <TopLoc_Location.hxx>

static const double REFINE_TRI_CAP = 20.0;   // a torn body may grow this much

int body_tears(const std::vector<TopoDS_Face> &faces)
{
  EdgeTally ed;
  for (const TopoDS_Face &face : faces)
  {
    TopLoc_Location loc;
    const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    if (!tri.IsNull() && tri->NbTriangles())
      tally_edges(tri, loc, ed);
  }
  return tally_tears(ed);
}

static int count_tris(const std::vector<TopoDS_Face> &faces)
{
  int n = 0;
  TopLoc_Location loc;          // NAMED: Triangulation takes a non-const ref and
                                // a temporary binds to one only under MSVC's
                                // permissive mode — clang/gcc reject it outright
  for (const TopoDS_Face &face : faces)
  {
    const Handle(Poly_Triangulation) tri = BRep_Tool::Triangulation(face, loc);
    n += tri.IsNull() ? 0 : tri->NbTriangles();
  }
  return n;
}

int refine_torn(const TopoDS_Shape &body, const std::vector<TopoDS_Face> &faces,
                double lin, double ang, const std::string &name,
                double floor_mm)
{
  int best = body_tears(faces);
  if (!best)
    return 0;
  double best_lin = lin;
  const int base_tris = count_tris(faces);
  // Measured on the live housing: 0.1 mm -> 41 tears, 0.05 -> 56, 0.02 -> 28,
  // 0.01 -> 5, 0.005 -> 0. Non-monotonic, so the ladder is fixed and the BEST
  // rung wins — which may well be the original one.
  const double ladder[3] = {4.0, 10.0, 20.0};
  double factor = ladder[0];
  double tried = lin;
  for (const double f : ladder)
  {
    factor = f;
    // the explosion guard holds on the ladder too: a rung below the body's
    // extent floor is clamped, and a clamped repeat is skipped
    const double rung = (lin / factor > floor_mm) ? lin / factor : floor_mm;
    if (rung >= tried)
      break;
    tried = rung;
    BRepTools::Clean(body);         // else the mesher keeps the coarse one
    BRepMesh_IncrementalMesh(body, rung, Standard_False, ang,
                             Standard_True);
    if (count_tris(faces) > base_tris * REFINE_TRI_CAP)
      break;                        // denser than the body is worth
    const int got = body_tears(faces);
    if (got < best)
    {
      best = got;
      best_lin = rung;
    }
    if (!got)
      break;
  }
  if (best_lin != lin / factor || best)
  {
    // land on the winning rung (the loop may have ended on a worse one)
    BRepTools::Clean(body);
    BRepMesh_IncrementalMesh(body, best_lin, Standard_False, ang,
                             Standard_True);
  }
  std::printf("hc-cadconv: %s — torn tessellation re-meshed at %.4f mm: "
              "%d tear(s) left\n", name.empty() ? "?" : name.c_str(),
              best_lin, best);
  return best;
}
