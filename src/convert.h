// STEP -> glb conversion, the port of `step2glb.convert()`.
#pragma once
#include <string>

struct ConvertOpts
{
  double lin_defl = 0.1;       // chordal deflection in the STEP file's AUTHOR
                               // units (unit-abstract, PROTOCOL 2); floored at
                               // 2e-4 x each body's own extent inside convert()
  double ang_defl_deg = 20.0;  // angular deflection, degrees
  bool refine_torn = true;     // re-mesh a torn CLOSED body
};

// Writes `glb` (+ its sidecars, from 8b on) and returns the number of XCAF free
// shapes. Throws std::runtime_error / Standard_Failure on failure — main() turns
// either into a one-line stderr message and a non-zero exit.
int convert(const std::string &step_path, const std::string &glb_path,
            const ConvertOpts &opts);
