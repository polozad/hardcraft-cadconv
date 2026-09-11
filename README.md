# hc-cadconv — STEP → glTF with a CAD-truth sidecar

A small command-line converter that reads a STEP file and writes

* **`<out>.glb`** — one glTF primitive per CAD face, un-welded, with the surface's
  true normals, and
* three JSON sidecars that carry what a triangulation loses:
  * **`<out>.glb.oracle.json`** — per face: the surface *kind* (plane / cylinder /
    cone / sphere / torus / bspline / revolution / extrusion / offset), its radius
    and its analytic **placement** (origin, axis, the surface's own θ=0 direction,
    cone semi-angle, torus radii); per edge: the curve kind and, for a circle, its
    centre / radius / axis; the parametric **seams** of periodic faces; and groups
    of faces whose defining surface data is bit-identical.
  * **`<out>.glb.uv.json`** — each face's parametric development: a (u, v) pair per
    mesh node, plus the period of each closed axis. The domain arrives with the
    periodic seam already open and holes as honest closed loops in a rectangle.
  * **`<out>.glb.parts.json`** — per solid: name, face and edge counts, how many
    tessellation tears were found and repaired, what was lost.

It is built for **retopology**: the mesh is a means of pointing at CAD facts, not
the product. A downstream tool can ask "is this face a cylinder, and where is its
axis?" instead of fitting a circle to triangles and hoping.

It is the ingest path of [Hardcraft](https://polozad.github.io/hardcraft-docs/), a
Blender retopology add-on, and it is useful on its own to anyone who wants CAD
structure next to a mesh. MIT — see `LICENSE`.

## Install

Windows: download `hardcraft-cadtools-<version>-win64.exe` from
[Releases](../../releases) and run it. It installs **per user**, no administrator
rights, nothing written outside your profile.

macOS / Linux: download the archive for your platform from the same page and put
the folder anywhere; the executable is `hc-cadconv` inside it.

## Use

```
hc-cadconv <in.step> <out.glb> [lin_defl] [ang_defl] [refine_torn]
hc-cadconv --protocol
```

* `lin_defl` — linear deflection **in the STEP file's own author units** (default
  0.1). `ang_defl` — angular deflection in degrees (default 20).
* `refine_torn` — `1` re-meshes a body whose tessellation tore along a shared CAD
  edge, keeping the least-torn result of a fixed ladder. Sloppy exports need it;
  clean ones do not notice.
* `--protocol` prints one JSON line — `{"protocol": N, "impl": ..., "occt": ...}`.
  **This is the compatibility contract:** a consumer checks that number and refuses
  a converter it does not understand, rather than misreading a changed sidecar. The
  sidecar schema may gain optional keys without moving the protocol; a consumer of
  an optional key must degrade when it is absent.

## Build from source

Stdlib Python only. OCCT is built from source (pinned) and cached:

```
python build.py            # OCCT once, then the converter
python build.py --bundle   # + collect the runtime next to the executable
```

Requires CMake and a C++17 compiler; Ninja if you would like it to finish this
week. Bundling also needs the platform's own relocation tool — `patchelf` on
Linux, the Xcode command line tools (`install_name_tool`, `codesign`) on macOS —
so that the folder finds its libraries next to itself instead of where it was
built. Windows needs neither. OCCT is configured with `USE_FREEIMAGE=OFF USE_FREETYPE=OFF USE_OPENGL=OFF`,
so the bundle has **no third-party runtime dependency** — the alternative was
shipping some fifteen image codecs a STEP converter never calls.

## Reproducibility and virus scanners

Every published binary is built by the GitHub Actions workflow in this repository,
from the commit the release points at, and its SHA-256 is printed in the run log
and shipped beside the file. If a scanner flags a download, please compare the hash
first and then open an issue with the vendor's name — an unsigned freshly-built
executable with no download history is a well-known false-positive shape, and a
public build log is what makes a false-positive report actionable.

## Contributing

The canonical tree lives in the Hardcraft repository; this one is a snapshot mirror
that is replaced wholesale on each release. Issues and discussion are welcome here;
patches are carried over by hand and come back in the next snapshot, so a merged
change will not appear as your commit — say so in the issue and you will be
credited in the release notes.
