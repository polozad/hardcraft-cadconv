"""Build hc-cadconv — build OCCT, then the converter. Stdlib only.

    python tools/cadconv/build.py             # OCCT (once, cached) + converter
    python tools/cadconv/build.py --occt      # only OCCT
    python tools/cadconv/build.py --clean     # drop the converter build, keep OCCT

ONE script for every platform: the same call
runs locally and in CI, so a green CI matrix means the artist's own command works.
The only platform-specific knowledge lives here, never in the C++.

WHY WE BUILD OCCT INSTEAD OF TAKING A PREBUILT (measured 2026-07-24)
The conda-forge `occt 7.9.3 novtk` binaries link TKService against FreeImage and
FreeType, so the loader demands them even though a STEP->glb converter never
decodes an image or rasterises a glyph. Satisfying that chain means shipping
freeimage + fontconfig + freetype + libpng + libtiff + libjpeg-turbo + libwebp +
openexr + imath + openjpeg + jxrlib + libraw + zlib — ~15 packages of image
codecs for nothing, and a supply-chain surface to match. A source build with
`USE_FREEIMAGE=OFF USE_FREETYPE=OFF USE_OPENGL=OFF` has NO third-party
dependency at all: the bundle is the TK dlls and nothing else.

The version stays pinned to what the OCP wheel carries — parity of the mesher is
what makes the python path a usable reference (that comparison IS the gate).
RapidJSON is the one exception: header-only, and OCCT's glTF writer needs it.
"""
import argparse
import io
import os
import platform
import shutil
import struct
import subprocess
import sys
import tarfile
import urllib.request

OCCT_TAG = "V7_9_3"                 # pinned: parity with the OCP wheel (7.9.3)
RAPIDJSON_TAG = "v1.1.0"
HERE = os.path.dirname(os.path.abspath(__file__))
WORK = os.path.join(HERE, "_occt")             # sources + install prefixes
BUILD_DIR = os.path.join(HERE, "build")        # the converter's own build tree

OCCT_URL = ("https://github.com/Open-Cascade-SAS/OCCT/archive/refs/tags/%s.tar.gz"
            % OCCT_TAG)
RAPIDJSON_URL = ("https://github.com/Tencent/rapidjson/archive/refs/tags/%s.tar.gz"
                 % RAPIDJSON_TAG)

# What OCCT must NOT drag in. Visualization stays ON — TKXCAF/TKRWMesh import
# TKService for XCAFDoc's texture/material types — but with no font and no image
# backend it compiles down to plain OCCT code with zero external imports.
OCCT_FLAGS = [
    "-DBUILD_LIBRARY_TYPE=Shared",             # LGPL: dynamic, never static
    "-DBUILD_MODULE_Draw=OFF",                 # the Tcl test harness
    "-DBUILD_DOC_Overview=OFF",
    "-DBUILD_SAMPLES_MFC=OFF", "-DBUILD_SAMPLES_QT=OFF",
    "-DUSE_FREETYPE=OFF", "-DUSE_FREEIMAGE=OFF",
    "-DUSE_OPENGL=OFF", "-DUSE_GLES2=OFF", "-DUSE_D3D=OFF",
    "-DUSE_VTK=OFF", "-DUSE_TBB=OFF", "-DUSE_DRACO=OFF", "-DUSE_FFMPEG=OFF",
    "-DUSE_XLIB=OFF",                          # headless linux
    "-DINSTALL_DIR_LAYOUT=Unix",               # lib/ bin/ include/ on ALL platforms
]


def subdir():
    """Platform tag for the install prefix (one tree per target)."""
    m = platform.machine().lower()
    arm = m in ("arm64", "aarch64")
    if sys.platform == "win32":
        return "win-64"
    if sys.platform == "darwin":
        return "osx-arm64" if arm else "osx-64"
    return "linux-aarch64" if arm else "linux-64"


def download_tar(url, dest, marker):
    """Unpack `url` (a github tag tarball) into `dest`; skip when present.

    Github tarballs hold ONE top-level directory; it is stripped so the caller
    gets a stable path instead of `OCCT-V7_9_3/`.
    """
    if os.path.isdir(dest) and os.path.exists(os.path.join(dest, marker)):
        return dest
    print("fetching %s" % url)
    with urllib.request.urlopen(url, timeout=900) as fh:
        blob = fh.read()
    tmp = dest + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    with tarfile.open(fileobj=io.BytesIO(blob), mode="r:gz") as tf:
        try:
            tf.extractall(tmp, filter="data")
        except TypeError:
            tf.extractall(tmp)                             # noqa: S202  (py<3.12)
    inner = [os.path.join(tmp, n) for n in os.listdir(tmp)]
    root = inner[0] if len(inner) == 1 and os.path.isdir(inner[0]) else tmp
    shutil.rmtree(dest, ignore_errors=True)
    os.replace(root, dest)
    shutil.rmtree(tmp, ignore_errors=True)
    return dest


def cmake_exe():
    """cmake from PATH, or the copy VS Build Tools ships (Windows has no other)."""
    got = shutil.which("cmake")
    if got:
        return got
    if sys.platform == "win32":
        for base in (os.environ.get("ProgramFiles(x86)", ""),
                     os.environ.get("ProgramFiles", "")):
            p = os.path.join(base, "Microsoft Visual Studio", "2022",
                             "BuildTools", "Common7", "IDE",
                             "CommonExtensions", "Microsoft", "CMake", "CMake",
                             "bin", "cmake.exe")
            if os.path.isfile(p):
                return p
    raise SystemExit("cmake not found — install it (Windows: the VS Build Tools "
                     "'C++ CMake tools' component)")


def _vcvars():
    """Path to vcvars64.bat, or None.

    ASK THE INSTALLER, don't guess the folder: the year/edition scan below only
    knows `2022\\{BuildTools,Community,Professional,Enterprise}` and broke on the
    first CI run (`windows-latest` is a moving image — its Visual Studio is not
    required to sit where this addon's dev box keeps it). `vswhere.exe` ships
    with every VS installer since 2017 at a FIXED path and answers for any
    version and edition.
    """
    tried = []
    vsw = os.path.join(os.environ.get("ProgramFiles(x86)",
                                      r"C:\Program Files (x86)"),
                       "Microsoft Visual Studio", "Installer", "vswhere.exe")
    tried.append(vsw)
    if os.path.isfile(vsw):
        try:
            out = subprocess.check_output(
                [vsw, "-latest", "-products", "*", "-requires",
                 "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                 "-property", "installationPath"],
                stderr=subprocess.DEVNULL).decode("utf-8", "replace")
        except (OSError, subprocess.CalledProcessError):
            out = ""
        for line in out.splitlines():
            root = line.strip()
            if not root:
                continue
            bat = os.path.join(root, "VC", "Auxiliary", "Build", "vcvars64.bat")
            tried.append(bat)
            if os.path.isfile(bat):
                return bat, tried
    for base in (os.environ.get("ProgramFiles(x86)", ""),
                 os.environ.get("ProgramFiles", "")):
        for year in ("2026", "2022", "2019"):
            for ed in ("BuildTools", "Community", "Professional", "Enterprise"):
                bat = os.path.join(base, "Microsoft Visual Studio", year, ed,
                                   "VC", "Auxiliary", "Build", "vcvars64.bat")
                tried.append(bat)
                if os.path.isfile(bat):
                    return bat, tried
    return None, tried


def vs_env():
    """MSVC's environment, harvested from vcvars64.bat (None off Windows).

    The compiler is not on PATH after a Build Tools install and CMake cannot find
    it from a plain shell either. Sourcing the batch file once and passing the
    result as `env` is the whole trick.
    """
    if sys.platform != "win32":
        return None
    bat, tried = _vcvars()
    if bat is None:
        raise SystemExit(
            "MSVC not found — install VS Build Tools with the 'Desktop "
            "development with C++' workload.\nLooked at:\n  %s"
            % "\n  ".join(tried))
    print("MSVC env from %s" % bat)
    out = subprocess.check_output('cmd /c ""%s" >nul && set"' % bat, shell=True)
    env = dict(os.environ)
    for line in out.decode("utf-8", "replace").splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env[k] = v
    return env


def ninja_gen():
    """Ninja when available — it is what makes a 24-thread OCCT build bearable.
    The VS Build Tools CMake component ships one next to cmake."""
    if shutil.which("ninja"):
        return ["-G", "Ninja"]
    cm = cmake_exe()
    side = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(cm))),
                        "Ninja", "ninja.exe")
    if os.path.isfile(side):
        return ["-G", "Ninja", "-DCMAKE_MAKE_PROGRAM=" + side]
    return []


def jobs():
    """`cmake --build` is SERIAL unless told otherwise. Ninja parallelises by
    itself, but it is not on every image — where the Makefile/MSBuild fallback
    kicks in, an OCCT build without this runs single-threaded for hours."""
    return ["--parallel", str(os.cpu_count() or 2)]


def build_occt():
    """Configure + build + install OCCT -> the prefix to link the converter to.
    Cached by the presence of the installed CMake config."""
    prefix = os.path.join(WORK, subdir())
    if os.path.isfile(os.path.join(prefix, "lib", "cmake", "opencascade",
                                   "OpenCASCADEConfig.cmake")):
        return prefix
    src = download_tar(OCCT_URL, os.path.join(WORK, "occt-src"), "CMakeLists.txt")
    rj = download_tar(RAPIDJSON_URL, os.path.join(WORK, "rapidjson"), "include")
    bld = os.path.join(WORK, "occt-build")
    cm, env = cmake_exe(), vs_env()
    subprocess.check_call(
        [cm, "-S", src, "-B", bld, "-DCMAKE_BUILD_TYPE=Release",
         "-DINSTALL_DIR=" + prefix,
         "-DUSE_RAPIDJSON=ON",                 # OCCT's glTF writer needs it
         "-D3RDPARTY_RAPIDJSON_DIR=" + rj,
         "-D3RDPARTY_RAPIDJSON_INCLUDE_DIR=" + os.path.join(rj, "include")]
        + OCCT_FLAGS + ninja_gen(), env=env)
    subprocess.check_call([cm, "--build", bld, "--config", "Release"] + jobs(),
                          env=env)
    subprocess.check_call([cm, "--install", bld, "--config", "Release"], env=env)
    print("OCCT installed -> %s" % prefix)
    return prefix


def build_converter(prefix):
    cm, env = cmake_exe(), vs_env()
    subprocess.check_call([cm, "-S", HERE, "-B", BUILD_DIR,
                           "-DCMAKE_BUILD_TYPE=Release",
                           "-DCMAKE_PREFIX_PATH=" + prefix] + ninja_gen(),
                          env=env)
    subprocess.check_call([cm, "--build", BUILD_DIR, "--config", "Release"]
                          + jobs(), env=env)
    exe = os.path.join(BUILD_DIR,
                       "hc-cadconv" + (".exe" if sys.platform == "win32" else ""))
    print("built -> %s" % exe)
    return exe


def pe_imports(path):
    """DLLs a Windows PE imports, read straight out of the file.

    Written by hand instead of shelling out to dumpbin: the bundler has to run
    on a CI image with no Visual Studio on PATH, and this is 40 lines.
    """
    with open(path, "rb") as fh:
        blob = fh.read()
    if blob[:2] != b"MZ":
        return []
    pe = struct.unpack_from("<I", blob, 0x3C)[0]
    if blob[pe:pe + 4] != b"PE\0\0":
        return []
    n_sec, opt_size = struct.unpack_from("<HH", blob, pe + 6)[0], \
        struct.unpack_from("<H", blob, pe + 20)[0]
    opt = pe + 24
    magic = struct.unpack_from("<H", blob, opt)[0]
    # the import directory is data directory #1; its offset differs between
    # PE32 (0x60) and PE32+ (0x70)
    dd = opt + (0x70 if magic == 0x20B else 0x60)
    imp_rva = struct.unpack_from("<I", blob, dd + 8)[0]
    if not imp_rva:
        return []
    sections = []
    sec = opt + opt_size
    for i in range(n_sec):
        off = sec + i * 40
        vaddr, raw_size, raw_ptr = struct.unpack_from("<III", blob, off + 12)
        sections.append((vaddr, raw_size, raw_ptr))

    def to_off(rva):
        for vaddr, raw_size, raw_ptr in sections:
            if vaddr <= rva < vaddr + max(raw_size, 1):
                return raw_ptr + (rva - vaddr)
        return None

    out, cur = [], to_off(imp_rva)
    while cur is not None:
        desc = blob[cur:cur + 20]
        if len(desc) < 20 or desc == b"\0" * 20:
            break
        name_off = to_off(struct.unpack_from("<I", desc, 12)[0])
        if name_off:
            end = blob.index(b"\0", name_off)
            out.append(blob[name_off:end].decode("ascii", "replace"))
        cur += 20
    return out


def _link_records(path):
    """What a mac/linux binary RECORDS as its dependencies, verbatim.

    Verbatim matters twice: the bundler wants the file names, and the mac
    relocation wants the exact recorded string to hand `install_name_tool
    -change`. `otool` prints a `path:` header line; `ldd` prints none.
    """
    tool = ["otool", "-L", path] if sys.platform == "darwin" else ["ldd", path]
    try:
        out = subprocess.check_output(tool, stderr=subprocess.DEVNULL)
    except (OSError, subprocess.CalledProcessError):
        return []
    recs = []
    for raw in out.decode("utf-8", "replace").splitlines():
        line = raw.strip()
        if not line or line.endswith(":"):                 # otool's header
            continue
        if sys.platform != "darwin" and "=>" in line:
            line = line.split("=>")[0]                     # keep the SONAME
        recs.append(line.split(" (")[0].strip())
    return [r for r in recs if r]


def deps_of(path):
    """Shared libraries `path` links against, BY NAME (platform-native scan).

    Names, never resolved paths. The exe links with rpath `$ORIGIN` /
    `@loader_path`, so inside the build tree NOTHING resolves: `ldd` answers
    `libTKernel.so.7.9 => not found` and `otool` answers `@rpath/libTKernel...`.
    Keeping only the absolute lines therefore bundled the exe and NOTHING else
    on linux and mac. The caller looks each name up in the OCCT prefix and
    skips what is not there, so an unresolved name is as good as a resolved one.
    """
    if sys.platform == "win32":
        return pe_imports(path)
    return [os.path.basename(r) for r in _link_records(path)]


def _quiet(argv):
    """Run a fixup tool, ignoring the ones that are already no-ops (adding an
    rpath a binary already carries is an error, and a harmless one)."""
    try:
        subprocess.call(argv, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError as e:
        raise SystemExit("%s not found (%s)" % (argv[0], e))


def mac_relocate(out):
    """Point the copied dylibs at EACH OTHER instead of at the build machine.

    OCCT's install records absolute install_names (`<prefix>/lib/libTKernel...`),
    so a straight copy keeps referring to the tree it was built in: the bundle
    runs on the box that produced it and dies everywhere else. Rewriting every
    bundled record to `@rpath/<name>` is the mac tax the spec budgeted for.
    The re-sign is not optional — install_name_tool invalidates the signature
    and arm64 macOS refuses to load an unsigned-but-modified library.
    """
    names = sorted(os.listdir(out))
    for f in names:
        p = os.path.join(out, f)
        if f.endswith(".dylib"):
            _quiet(["install_name_tool", "-id", "@rpath/" + f, p])
        for rec in _link_records(p):
            base = os.path.basename(rec)
            if base in names and rec != "@rpath/" + base:
                _quiet(["install_name_tool", "-change", rec, "@rpath/" + base, p])
        _quiet(["install_name_tool", "-add_rpath", "@loader_path", p])
    for f in names:
        _quiet(["codesign", "-f", "-s", "-", os.path.join(out, f)])


def bundle(prefix, exe):
    """Lay the exe + the libraries it actually needs into dist/hc-cadconv/.

    The drop shape `cadio/backend.py` already accepts: an unpacked folder in
    `<blender config>/hardcraft/bin/`. Transitively scanned, never a hardcoded
    dll list — a link-set change must not silently ship a broken folder.
    """
    lib_dir = os.path.join(prefix, "bin" if sys.platform == "win32" else "lib")
    out = os.path.join(HERE, "dist", "hc-cadconv")
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    shutil.copy2(exe, out)
    seen, queue = set(), [exe]
    while queue:
        for name in deps_of(queue.pop()):
            if name.lower() in seen:
                continue
            seen.add(name.lower())
            src = os.path.join(lib_dir, name)
            if not os.path.isfile(src):
                continue              # a system library — never bundled
            shutil.copy2(src, out)
            queue.append(src)
    if sys.platform == "darwin":
        mac_relocate(out)
    total = sum(os.path.getsize(os.path.join(out, f)) for f in os.listdir(out))
    print("bundled %d file(s), %.1f MB -> %s"
          % (len(os.listdir(out)), total / 1e6, out))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--occt", action="store_true", help="only build OCCT")
    ap.add_argument("--bundle", action="store_true",
                    help="also lay out dist/hc-cadconv/ (exe + its libraries)")
    ap.add_argument("--clean", action="store_true",
                    help="drop the converter build tree (keeps OCCT)")
    args = ap.parse_args()
    if args.clean:
        shutil.rmtree(BUILD_DIR, ignore_errors=True)
        print("removed %s" % BUILD_DIR)
    prefix = build_occt()
    if args.occt:
        return
    exe = build_converter(prefix)
    if args.bundle:
        bundle(prefix, exe)


if __name__ == "__main__":
    sys.exit(main())
