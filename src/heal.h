// STEP WRITER SPLITS — the port of step2glb.py's block of the same name.
//
// The HOOPS writer (Plasticity's STEP export) cuts a face on a periodic surface
// along the surface's period origin when its trim crosses it, splits the rim
// edges at the cut's ends, and APPENDS the cut-off piece at the end of the
// shell. Plasticity and the bridge have ONE face there. With `heal_splits` the
// B-rep is restored before anything reads it, so every channel (glb, uv,
// oracle, seams, topology) sees the face the artist has. A pair (A, B) merges
// only when all four checks hold — never anything else:
//   R1  A and B are in one shell and share an edge each uses once (no seam)
//   R2  their surfaces are bit-equal (resolved entity text, planes excluded)
//   R3  the shared edge lies on the surface's period origin (u or v iso)
//   R4  the later piece sits in the shell's trailing block — the run of faces
//       at the shell's end that are R1+R2 members or carry a B-spline surface
//       (where the writer appends what it cut off)
#pragma once
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <XCAFDoc_ShapeTool.hxx>

// {(eid_a, eid_b): (pos_a, pos_b)} — a = the earlier piece in shell order.
using HealPairs = std::map<std::pair<int, int>, std::pair<int, int>>;

// The TEXT half of the checks, per shell: R1, R2 and R4. R3 is judged on the
// shape (the pcurves).
HealPairs writer_split_pairs(const std::string &step_path);

// The faces / bodies the heal rebuilt are new shapes the transfer map has never
// seen; their STEP identity rides here (reset per conversion).
struct HealAlias
{
  TopTools_IndexedMapOfShape faces;
  std::vector<int> face_eids;
  TopTools_IndexedMapOfShape solids;
  std::vector<std::string> solid_names;
  void clear()
  {
    faces.Clear();
    face_eids.clear();
    solids.Clear();
    solid_names.clear();
  }
};
extern HealAlias HEAL_ALIAS;

struct HealRecord
{
  int kept = 0;                     // the earlier piece's eid — the merged face's
  std::vector<int> gone;            // the pieces folded into it
};

struct HealResult
{
  std::vector<HealRecord> healed;
  std::vector<std::vector<int>> kept;   // components left as the file has them
  int removed = 0;                      // faces folded away
};

// R3 on the shape + the heal, per simple-shape label of the XCAF document.
// `face_eid` / `solid_name` are the converter's own lookups (transfer map).
HealResult heal_writer_splits(
    const Handle(XCAFDoc_ShapeTool) & shape_tool, const HealPairs &pairs,
    const std::function<int(const TopoDS_Face &)> &face_eid,
    const std::function<std::string(const TopoDS_Shape &)> &solid_name);
