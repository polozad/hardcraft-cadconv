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

#include <TopTools_IndexedMapOfShape.hxx>
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

// `faces[].w` — the face's wires as GLOBAL edges[] indices, in the wire's own
// LOOP ORDER. «Is this loop a cut-out or a scrap of soup» is guessed three ways
// downstream today; OCC simply knows (`BRepTools::OuterWire`). A periodic
// face's SEAM edge appears TWICE in its loop (once per pcurve — that is the
// loop as CAD has it); an edge with no `edges[]` record (degenerate apex, mute
// curve) is OMITTED and the loop stays a loop, one hop shorter.
struct FaceWires
{
  bool present = false;
  std::vector<int> outer;
  std::vector<std::vector<int>> inner;
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
  // The CAD-TOPOLOGY provision (2026-09-18). Every one of these is a fact OCC
  // already holds; an absent flag means «unknown», never a wrong answer.
  bool has_orient = false;
  bool orient_rev = false;          // face.Orientation() == TopAbs_REVERSED
  bool has_bounds = false;
  double b[4] = {0, 0, 0, 0};       // [u0,u1,v0,v1], RAW OCC parameters — the
                                    // SAME space UvRecord reads for pu/pv
  FaceWires w;
};

// `edges[].p` — one edge's (u,v) trace on each adjacent face, in that face's RAW
// OCC parameters. A LIST of traces per face: once normally, TWICE on a periodic
// face's SEAM edge, where OCC holds two pcurves (one at each end of the period)
// and picks between them by the edge's ORIENTATION. Each trace is a FLAT u,v run.
struct EdgePcurve
{
  int face = 0;                     // GLOBAL oracle face index (the record's own `f`)
  std::vector<std::vector<double>> traces;
};

struct EdgeRecord
{
  double a[3] = {0, 0, 0};
  double b[3] = {0, 0, 0};
  std::string kind;                 // "line" | "circle" | "ellipse" | "bspline" | "?"
  bool has_circle = false;
  double r = 0.0, c[3] = {0, 0, 0}, ax[3] = {0, 0, 0};
  // An ELLIPSE was typed and its numbers thrown away until 2026-09-18: `r` is
  // then the MAJOR radius, `r2` the minor, `xd` the major-axis direction.
  bool has_ellipse = false;
  double r2 = 0.0, xd[3] = {0, 0, 0};
  std::vector<int> f;               // adjacent GLOBAL face indices, ascending,
                                    // deduped (a seam edge names its face ONCE)
  bool has_v = false;
  int v[2] = {0, 0};                // [i0, i1] into verts[] (closed edge: equal)
  std::vector<EdgePcurve> p;        // keyed by the very faces `f` just named
};

// One CAD 0-cell. An unreadable vertex emits `null` and keeps the INDEXING
// honest — `edges[].v` indexes this list.
struct VertRecord
{
  bool ok = false;
  double p[3] = {0, 0, 0};
};

// The block's edge oracle plus the map that turns a wire's edge into its
// position in `recs` (only the edges that PRODUCED a record, in record order,
// so `recmap.FindIndex(edge) - 1` IS the record's index).
struct EdgeOracle
{
  std::vector<EdgeRecord> recs;
  TopTools_IndexedMapOfShape recmap;
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

// `faces`/`f0` are the block's face list and its global face offset, `vmap`/`v0`
// the block's 0-cell map and its global offset — without them `f`, `p` and `v`
// are simply absent and every consumer degrades to its own measurement (the
// gradient law: a CAD fact adds precision, it never takes a route away). The
// block is the unit of INDEXING, so both offsets are the caller's.
EdgeOracle edge_oracle(const TopoDS_Shape &shape,
                       const std::vector<TopoDS_Face> *faces = nullptr,
                       int f0 = 0, int v0 = 0,
                       const TopTools_IndexedMapOfShape *vmap = nullptr);

// The block's CAD 0-cells (positions in oracle scale) and their TopExp map.
// Today every consumer re-derives them from edge endpoints plus a «>=3 faces at
// one quantised position» rule — a global-scale threshold with its own bug class.
void block_verts(const TopoDS_Shape &shape, std::vector<VertRecord> &pts,
                 TopTools_IndexedMapOfShape &vmap);

// The face's wires as GLOBAL edges[] indices; `.present == false` when nothing
// could be read (an absent key, never a wrong wire).
FaceWires face_wires(const TopoDS_Face &face,
                     const TopTools_IndexedMapOfShape &recmap, int e0);

// 3D trace of every SEAM edge of a periodic face — the ONLY carrier of where a
// wrap opens (the mesher welds the seam shut in the triangulation).
std::vector<SeamRecord> face_seams(const TopoDS_Face &face, int idx);
