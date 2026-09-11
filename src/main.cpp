// hc-cadconv — the CLI shell of the CAD converter: a native OCCT port of the
// python reference implementation.
//
// THE CONTRACT (the consumer holds the other end of it):
//     hc-cadconv <step> <glb> <lin_defl> <ang_defl_deg> <refine_torn>
//         (lin_defl in the STEP file's AUTHOR units — unit-abstract, PROTOCOL 2)
//         convert; exit 0 = the glb + its sidecars were written
//     hc-cadconv --protocol
//         print ONE json line and exit 0 — the compatibility handshake the
//         addon runs BEFORE it spawns a conversion, so a stale binary writing
//         an old sidecar schema is refused readably instead of half-importing.
//
// Kept a 1:1 transliteration of the python on purpose (same function names,
// same order): the reference implementation stays the proof, and a diff between
// the two has to be readable by eye.
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include <Standard_Failure.hxx>
#include <Standard_Version.hxx>

#include "convert.h"

// Bump TOGETHER with the consumer's PROTOCOL and the python reference's, but
// ONLY when an older producer would be MISREAD — changed argv, changed units,
// or a changed meaning for an existing field. The number is compared on strict
// equality, so a bump REFUSES every older build outright; an ADDITIVE sidecar
// key is announced in CAPS below instead, and a consumer degrades around it.
// 2 (2026-08-20, unit abstract): glb + sidecars in the STEP file's AUTHOR
// units (declared-unit normalisation unwound), deflection argv in author
// units, per-body extent floor on the deflection.
static const int PROTOCOL = 2;

// The channels this build emits. Present = authoritative; a consumer that sees
// NO caps key must treat it as UNKNOWN (an older converter) and go on probing
// the data, never as "this converter emits nothing".
static const char *CAPS = "\"placement\",\"eids\",\"seams\",\"uv\",\"parts\"";

static int handshake()
{
  // No "ocp" key on purpose: `backend.verify()` rejects an EXPLICIT false
  // (that is the source path's "this python has no bindings" answer). A native
  // binary has no bindings to report, and must not look like a broken python.
  std::printf("{\"protocol\":%d,\"impl\":\"hc-cadconv\",\"occt\":\"%s\","
              "\"caps\":[%s]}\n",
              PROTOCOL, OCC_VERSION_COMPLETE, CAPS);
  return 0;
}

static const char *USAGE =
    "hc-cadconv <step> <glb> [lin_defl] [ang_defl_deg] [refine_torn]\n"
    "hc-cadconv --protocol\n";

// argv doubles, parsed under the C locale (see main): a comma-decimal locale
// would silently truncate "0.05" to 0 and re-mesh the whole model coarse.
static double arg_double(const char *s, double fallback)
{
  char *end = nullptr;
  const double v = std::strtod(s, &end);
  if (end == s || (end && *end != '\0'))
    return fallback;
  return v;
}

// The python accepts 0/false/off; anything else is on.
static bool arg_bool(const char *s)
{
  std::string v(s);
  for (char &c : v)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return !(v == "0" || v == "false" || v == "off");
}

int main(int argc, char **argv)
{
  // FIRST statement, before anything formats or parses a number: OCCT, our json
  // writer and strtod all follow LC_NUMERIC, and a RU/DE locale turns every
  // decimal point into a comma — that corrupts the sidecars AND the clone
  // signatures. The converter is locale-free by decree.
  std::setlocale(LC_ALL, "C");

  if (argc >= 2 && std::strcmp(argv[1], "--protocol") == 0)
    return handshake();
  if (argc < 3)
  {
    std::fputs(USAGE, stderr);
    return 2;
  }

  ConvertOpts opts;
  if (argc > 3)
    opts.lin_defl = arg_double(argv[3], opts.lin_defl);
  if (argc > 4)
    opts.ang_defl_deg = arg_double(argv[4], opts.ang_defl_deg);
  if (argc > 5)
    opts.refine_torn = arg_bool(argv[5]);

  try
  {
    const int n = convert(argv[1], argv[2], opts);
    std::printf("hc-cadconv: %d free shape(s) -> %s\n", n, argv[2]);
  }
  catch (const Standard_Failure &e)
  {
    // OCCT's own failures carry the readable message; the addon shows the log
    // tail on a non-zero exit, so one line is what the artist sees.
    std::fprintf(stderr, "hc-cadconv: OCCT error: %s\n",
                 e.GetMessageString() ? e.GetMessageString() : "?");
    return 1;
  }
  catch (const std::exception &e)
  {
    std::fprintf(stderr, "hc-cadconv: %s\n", e.what());
    return 1;
  }
  return 0;
}
