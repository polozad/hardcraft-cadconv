// The torn-tessellation repair: re-mesh ONE torn body finer and keep
// the least torn result. Ported from the python reference.
//
// A tear is the mesher failing to stitch two faces along a shared CAD edge, and
// it is provoked by sloppy input (the live striker.stp housing carries 6.8 mm
// edge tolerances on a 20 mm body). ShapeFix + SameParameter does not help;
// density does, but NOT monotonically — which is why this walks a fixed ladder
// and keeps the BEST rung instead of tightening until it looks fixed.
#pragma once
#include <string>
#include <vector>

#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>

int body_tears(const std::vector<TopoDS_Face> &faces);

// Returns the tears left. Only bodies that actually tore pay the price.
// `floor_mm` clamps every ladder rung (the unit-abstract explosion guard —
// 2e-4 x the body's own extent); a clamped repeat rung is skipped.
int refine_torn(const TopoDS_Shape &body, const std::vector<TopoDS_Face> &faces,
                double lin, double ang, const std::string &name,
                double floor_mm = 0.0);
