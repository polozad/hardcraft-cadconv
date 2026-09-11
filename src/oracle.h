// The CAD-truth sidecars: `<glb>.oracle.json` (surface/curve types, seams, clone
// signatures) and `<glb>.uv.json` (the parametric development of each face).
//
// Port of the `_face_*` / `_edge_oracle` block of `cadio/step2glb.py`. The whole
// point of emitting them HERE is that they are read off the SAME face traversal
// that orders the glb primitives — `faces[i] == primitive i` is constructive,
// never a text correlation.
#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <Poly_Triangulation.hxx>

class Json;

// Oracle coordinates ship in GLB UNITS — the file's AUTHOR units since
// PROTOCOL 2 (raw OCC mm x 1/unit_mm; set per file by convert()) — the same
// constant the glb writer applies, so oracle positions == primitive positions.
extern double ORACLE_SCALE;

// One face's parametric UV, or "no UV nodes" (the consumer validates per-face
// vertex counts and drops the whole channel on any mismatch).
struct UvRecord
{
  bool present = false;
  int n = 0;
  std::vector<double> uv;           // flat u,v pairs, rounded to 1e-6
  double pu = 0.0, pv = 0.0;        // parametric period per closed axis (0 = open)
};

// The analytic PLACEMENT of a face, as NUMBERS. An OPTIONAL key inside the
// current protocol: it is additive, so main.cpp's PROTOCOL does NOT move — the
// handshake is compared on strict equality, and a bump would make every consumer
// refuse every older producer. A consumer of an optional key degrades when it is
// absent instead, which is the whole point. These same values were
// already computed for `surf_sig` below and collapsed into a hash; downstream
// that cost a re-measurement of every one of them (the tube's axis by SVD
// through two rim circle-fits, a ring phase invented by a Z->Y->X cascade, the
// ARC branch re-fitting circles the oracle already knows).
//
// Lengths are scaled by ORACLE_SCALE like every other oracle magnitude;
// directions are UNIT and unscaled. An absent flag means the surface has no
// such fact (a bspline has only a kind; an extrusion has only `d` = its RULING)
// — the consumer then keeps its current measurement, never refuses.
struct FaceGeom
{
  bool has_o = false, has_d = false, has_x = false;
  double o[3] = {0, 0, 0};          // placement origin (sphere: its centre)
  double d[3] = {0, 0, 0};          // axis / plane normal / extrusion ruling
  double x[3] = {0, 0, 0};          // placement X — CAD's OWN theta = 0
  bool has_rmaj = false, has_rmin = false, has_semi = false;
  double rmaj = 0.0;                // cylinder/sphere R, cone RefRadius, torus Major
  double rmin = 0.0;                // torus Minor
  double semi = 0.0;                // cone semi-angle, radians
  bool any() const { return has_o || has_d || has_x || has_rmaj; }
};

struct FaceRecord
{
  std::string kind;                 // "plane" | "cylinder" | ... | "?"
  bool has_r = false;
  double r = 0.0;                   // radius in glb units, analytic surfaces only
  FaceGeom g;                       // the optional placement block (see FaceGeom)
  int eid = 0;                      // STEP #ident of the ADVANCED_FACE (0 = unmapped).
                                    // THE face<->primitive correlation: shell-order
                                    // text correlation broke on re-exports (entity
                                    // renumbering permutes it) — this is exact.
};

struct EdgeRecord
{
  double a[3] = {0, 0, 0};
  double b[3] = {0, 0, 0};
  std::string kind;                 // "line" | "circle" | "ellipse" | "bspline" | "?"
  bool has_circle = false;
  double r = 0.0, c[3] = {0, 0, 0}, ax[3] = {0, 0, 0};
};

struct SeamRecord
{
  int face = 0;                     // index INTO the emitted primitives
  std::vector<double> pts;          // flat x,y,z of 17 samples
};

// Clone intake: faces whose surface is bit-equal defining data. Keyed by the
// signature STRING itself (the python hashed it to keep the dict small; only
// the GROUPING is observable, and a full-string key cannot collide at all).
using CloneSigs = std::unordered_map<std::string, std::vector<int>>;

UvRecord face_uv_record(const TopoDS_Face &face,
                        const Handle(Poly_Triangulation) & tri);

// Also feeds `sigs` with this face's clone signature (index = its primitive).
FaceRecord face_oracle(const TopoDS_Face &face, CloneSigs &sigs,
                       std::vector<std::string> &sig_order, int idx);

std::vector<EdgeRecord> edge_oracle(const TopoDS_Shape &shape);

// 3D trace of every SEAM edge of a periodic face — the ONLY carrier of where a
// wrap opens (the mesher welds the seam shut in the triangulation).
std::vector<SeamRecord> face_seams(const TopoDS_Face &face, int idx);
