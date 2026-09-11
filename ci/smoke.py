"""Smoke the freshly built converter: it must SPEAK and it must WRITE.

    python ci/smoke.py <executable> <fixture.stp>

Two failures this catches that "it compiled" does not:

  * a binary whose `--protocol` handshake drifts from what a consumer expects —
    the addon compares that number on strict equality and refuses a mismatch, so
    a silent drift here reaches a user as «CAD converter protocol N, expects M»;
  * a binary that writes the glb and quietly drops a sidecar, or writes a sidecar
    missing an optional block. Optional keys are the dangerous shape: a consumer
    degrades when one is absent (by design), which means a REGRESSION looks
    exactly like an older build. So the presence of each block is asserted here,
    where the exe that just came off the compiler is the thing being questioned.

The fixture is an OCCT-generated synthetic part (15 faces, 3 clone groups, one
periodic seam, three solids) — deliberately not anyone's real model.
"""
import json
import math
import os
import subprocess
import sys

ANALYTIC = {"plane", "cylinder", "cone", "sphere", "torus"}
FACES, CLONES, SEAMS, BLOCKS = 15, 3, 1, [6, 6, 3]


def die(msg):
    print("SMOKE FAIL: %s" % msg)
    raise SystemExit(1)


def main():
    if len(sys.argv) != 3:
        die("usage: smoke.py <executable> <fixture.stp>")
    exe, step = sys.argv[1], sys.argv[2]
    if not os.path.isfile(exe):
        die("no executable at %s" % exe)

    out = subprocess.run([exe, "--protocol"], capture_output=True, text=True)
    if out.returncode != 0:
        die("--protocol exited %d: %s" % (out.returncode, out.stderr[-400:]))
    try:
        hs = json.loads(out.stdout.strip().splitlines()[-1])
    except (ValueError, IndexError):
        die("--protocol answered no json: %r" % out.stdout[-200:])
    for k in ("protocol", "impl", "occt"):
        if k not in hs:
            die("handshake has no %r: %s" % (k, hs))
    print("handshake ok: protocol %s (%s, OCCT %s)"
          % (hs["protocol"], hs["impl"], hs["occt"]))

    glb = "smoke_out.glb"
    run = subprocess.run([exe, step, glb, "0.1", "20", "1"],
                         capture_output=True, text=True)
    if run.returncode != 0:
        die("conversion exited %d: %s" % (run.returncode, run.stderr[-400:]))
    for f in (glb, glb + ".oracle.json", glb + ".uv.json", glb + ".parts.json"):
        if not (os.path.isfile(f) and os.path.getsize(f)):
            die("missing or empty: %s" % f)

    with open(glb + ".oracle.json", "r", encoding="utf-8") as fh:
        orc = json.load(fh)
    with open(glb + ".uv.json", "r", encoding="utf-8") as fh:
        uv = json.load(fh)
    with open(glb + ".parts.json", "r", encoding="utf-8") as fh:
        parts = json.load(fh)

    if len(orc.get("faces", [])) != FACES:
        die("faces %d, expected %d" % (len(orc.get("faces", [])), FACES))
    if len(orc.get("clones", [])) != CLONES:
        die("clone groups %d, expected %d" % (len(orc.get("clones", [])), CLONES))
    if len(orc.get("seams", [])) != SEAMS:
        die("seams %d, expected %d" % (len(orc.get("seams", [])), SEAMS))
    if [p["n"] for p in parts.get("parts", [])] != BLOCKS:
        die("blocks %s, expected %s" % ([p["n"] for p in parts["parts"]], BLOCKS))
    if len(uv.get("faces", [])) != FACES:
        die("uv records %d, expected %d" % (len(uv.get("faces", [])), FACES))

    # the analytic PLACEMENT block: optional by protocol, mandatory by build
    an = [f for f in orc["faces"] if f["s"] in ANALYTIC]
    if not an:
        die("fixture holds no analytic face — the block cannot be proven")
    for i, f in enumerate(orc["faces"]):
        if f["s"] not in ANALYTIC:
            continue
        g = f.get("g") or {}
        missing = [k for k in ("o", "d", "x") if k not in g]
        if missing:
            die("face %d (%s) has no %s in its placement" % (i, f["s"], missing))
        for k in ("d", "x"):
            n = math.sqrt(sum(c * c for c in g[k]))
            if abs(n - 1.0) > 1e-9:
                die("face %d: %s is not unit (|%s| = %.17g)" % (i, k, k, n))
        dot = sum(a * b for a, b in zip(g["d"], g["x"]))
        if abs(dot) > 1e-9:
            die("face %d: d and x are not orthogonal (dot = %.3g)" % (i, dot))
        if f["s"] == "cylinder" and abs(g.get("rmaj", -1) - f.get("r", -2)) > 1e-12:
            die("face %d: rmaj %r != r %r" % (i, g.get("rmaj"), f.get("r")))

    print("smoke ok: %d faces, %d clone groups, %d seam(s), blocks %s, "
          "%d analytic faces carry a full placement"
          % (FACES, CLONES, SEAMS, BLOCKS, len(an)))


if __name__ == "__main__":
    main()
