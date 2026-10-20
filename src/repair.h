// MESH REPAIR: faces the mesher left with HOLES (2026-10-20) — the port of
// step2glb.py's block of the same name.
//
// A valid CAD face can still come out of BRepMesh with whole pieces missing
// (axg.stp's knurl strips chain their facets through 0.2-2.4 um necks; the
// discretised outline crosses itself there and the mesher drops what it cannot
// resolve). Such a face is re-triangulated from its OWN boundary nodes — the
// edge polygons it shares with its neighbours — so the result is watertight by
// construction. The CAD topology is untouched; a face the mesher closed is
// never touched.
#pragma once
#include <functional>

#include <TopTools_IndexedMapOfShape.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

// Repair every face of `shape` the mesher left with holes (`done` = faces
// already handled — an assembly instance shares its prototype's
// triangulation). Prints one line per repair; returns how many.
int repair_holed_faces(const TopoDS_Shape &shape,
                       const std::function<int(const TopoDS_Face &)> &eid_of,
                       TopTools_IndexedMapOfShape &done);
