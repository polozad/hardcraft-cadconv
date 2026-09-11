// ONE OBJECT PER CAD BODY: the XCAF walk down to leaf parts, the split
// inside a part by solid, and the `<glb>.parts.json` sidecar that carries the
// block boundaries to the import.
//
// Port of the `_xcaf_parts` / `_solid_owners` / `_block_*` block of
// `cadio/step2glb.py`. The face traversal is NOT reordered by any of this: a
// face is merely LABELLED with its owning body, and the blocks fall out as the
// runs of that label — which is what keeps the blocks aligned with the writer's
// primitives by construction.
#pragma once
#include <array>
#include <map>
#include <string>
#include <vector>

#include <TopTools_IndexedMapOfShape.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <Poly_Triangulation.hxx>
#include <XCAFDoc_ShapeTool.hxx>
#include <XSControl_TransferReader.hxx>

// One leaf PART of the assembly tree: the located shape plus the name the tree
// gave it.
struct PartIn
{
  TopoDS_Shape shape;
  std::string name;
};

// One BODY inside a part (or the loose sheet faces that belong to none).
struct SolidOwner
{
  TopTools_IndexedMapOfShape faces;
  std::string name;
  TopoDS_Shape shape;
  bool closed = true;               // a SOLID; false = a loose shell (sheet body)
};

// One emitted block == one object on import.
struct PartOut
{
  std::string name;
  int n = 0;                        // faces (== primitives) in the block
  int ne = 0;                       // oracle edge records of the block
  int torn = 0;                     // unpaired triangulation edges (closed only)
};

// Quantised triangle-edge tally of the block being built (see tally_edges).
using EdgeTally = std::map<std::pair<std::array<long long, 3>,
                                     std::array<long long, 3>>, int>;

std::string label_name(const TDF_Label &label);

// Walk the XCAF tree down to leaf parts, accumulating placements down the branch.
void xcaf_parts(const Handle(XCAFDoc_ShapeTool) & shape_tool,
                const TDF_Label &label, const TopLoc_Location &loc,
                const std::string &name_hint, std::vector<PartIn> &out);

// [(face-map, name) per body] of `shape`, or empty when it holds <= 1 solid.
std::vector<SolidOwner> solid_owners(const TopoDS_Shape &shape,
                                     const Handle(XSControl_TransferReader) & tr,
                                     const std::string &name_hint);

int owner_of(const std::vector<SolidOwner> &owners, const TopoDS_Face &face);
bool has_solid(const TopoDS_Shape &shape);
bool block_closed(const std::vector<SolidOwner> &owners, int owner,
                  const TopoDS_Shape &shape);
std::string block_name(const std::string &part_name, int owner,
                       const std::vector<SolidOwner> &owners);

// A compound of `faces` — the shape the edge oracle is read off for one block
// (a body's own edges, not the whole part's).
TopoDS_Shape block_shape(const std::vector<TopoDS_Face> &faces);

// Count this face's triangle edges into the block's tally, keyed by QUANTISED
// node position. An edge seen ONCE is a TEAR: the neighbouring face meshed the
// shared CAD edge differently and the two triangulations do not meet.
void tally_edges(const Handle(Poly_Triangulation) & tri,
                 const TopLoc_Location &loc, EdgeTally &ed);

int tally_tears(const EdgeTally &ed);
