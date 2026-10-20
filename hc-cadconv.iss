; hc-cadconv — the STEP converter package for Windows.
;
; PER-USER by design — we are asking people to run a binary, so ask for as
; little as possible in return: PrivilegesRequired=lowest
; means no UAC prompt, the files land in the profile, and the uninstall entry is
; the current user's. Never Program Files, no service, no autostart, no registry
; beyond Inno's own uninstall bookkeeping.
;
; The install path is FIXED and Blender-INDEPENDENT on purpose — it is the slot
; `backend._user_bin_dir()` probes. Blender's config dir is per-VERSION, so an
; installer aiming there would have to guess which Blenders exist (install under
; 5.2, artist opens 5.1, no converter).
;
; The whole onedir folder ships: exe + the OCCT libraries as SEPARATE files.
; OCCT is LGPL-2.1 and `tools/cadconv/build.py` builds it Shared deliberately —
; dynamic linking is what lets us keep our own source closed, and the libraries
; must stay replaceable. Their licence + the OCCT exception ship alongside.
;
; Build:  ISCC.exe tools\cadconv\hc-cadconv.iss
; (payload must exist: tools\cadconv\dist\hc-cadconv\)

; VERSION IS THE CONVERTER'S OWN, deliberately NOT the add-on's. The converter is
; a satellite with its own lifecycle — that is the whole reason the `--protocol`
; handshake exists. Tie the two numbers together and every alpha bump would force
; a 33 MB repackage, while a converter fix would need an add-on release. What
; decides compatibility is PROTOCOL (backend.PROTOCOL == step2glb.PROTOCOL),
; never a matching version string. The name still carries `hardcraft` everywhere
; BUT: a PROTOCOL bump in src/main.cpp MUST be followed by a converter release —
; 1.0.0 (protocol 1) stayed attached to every add-on release for ten days after
; the bump to 2 and every STEP tester hit "converter protocol 1, addon expects 2"
; (2026-09-02). the consumer's test suite now pins this #define to the code.
; (it is not a standalone product: it is the converter Hardcraft drives).
#define AppName      "Hardcraft CAD converter"
; 1.2.0 (2026-09-12): the placement block + the capability handshake. The
; protocol did NOT move (both are additive), so nothing forced a version bump —
; and that is exactly how TWO different binaries came to be called 1.1.0: the one
; attached to the add-on releases up to 2026-09-10 emits neither, the one built on
; 2026-09-11 emits both. At runtime `caps` tells them apart, which is what it is
; for; a file name cannot. So the rule is: an additive CHANNEL still bumps this
; version, even when it leaves PROTOCOL alone.
; 1.2.3 (2026-09-21): NOTHING functional — the asset rename alone. The rule above
; is about channels; this is its mirror image. 1.2.2 was published as
; `hardcraft-cadtools-1.2.2-win64.exe`, so re-cutting 1.2.2 under the new name
; would give ONE build TWO names — the same ambiguity as two builds under one
; name, and `caps` cannot help: they tell contents apart, not file names. A
; version is cheap; an ambiguous artifact is not.
; 1.2.4 (2026-09-27): a wrapped face whose trim over- or undershoots its period
; by more than 1e-9 gets its period in the uv sidecar (was 0 — striker face 68,
; slide face 72: the seam emitter had already moved to the seam-edge test, the
; period record beside it had not); the Windows `.sha256` is LF + lowercase, so
; `sha256sum -c` passes on unix. No PROTOCOL change, no new channel.
; 1.2.5 (2026-10-11): the face -> STEP entity lookup is indexed once instead of
; scanned per face (axg.stp: 45 s -> 11.5 s, 33.5 s of it was that scan; output
; byte-identical); a face that will not reach the mesh is NAMED in the log
; (#ident, surface kind, size, centre — the import note shows it); one stage-
; timing line. Text only: no PROTOCOL change, no new channel. 1.2.4 was built
; in CI but never released — superseded, not re-cut under its name.
#define AppVersion   "1.2.6"
#define Protocol     "2"
#define AppPublisher "polozad"
#define AppURL       "https://github.com/polozad/hardcraft-dist"
#define Payload      "dist\hc-cadconv"
#define OcctSrc      "_occt\occt-src"

[Setup]
; a STABLE AppId — an upgrade must replace the previous install, not stack next
; to it. Never regenerate this GUID.
AppId={{7A0D2F4F-E4D8-5E61-8717-8C70BF84EDD2}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher={#AppPublisher}
AppSupportURL={#AppURL}
VersionInfoVersion={#AppVersion}

; ── per-user, no elevation ────────────────────────────────────────────────────
PrivilegesRequired=lowest
PrivilegesRequiredOverridesAllowed=dialog
DefaultDirName={localappdata}\hardcraft\bin\hc-cadconv
UsePreviousAppDir=yes
DisableDirPage=yes
DisableProgramGroupPage=yes
CreateAppDir=yes
Uninstallable=yes
UninstallDisplayName={#AppName} {#AppVersion}
UninstallDisplayIcon={app}\hc-cadconv.exe

; ── output ────────────────────────────────────────────────────────────────────
OutputDir=dist
; ONE stem for every platform: `hc-cadconv-<version>-<triple>`, the name of
; the binary itself, of the CMake target, of the public repo and of the
; manifest the consumer reads. Until 1.2.2 this file alone said
; `hardcraft-cadtools-<version>-win64` — a product name for a package that
; holds ONE tool, in a spelling (`win64`) that matched no other asset. Two
; stems and two spellings of the triple on the same release page is what the
; artist saw; `tools/release.py` still accepts the old name so the assets
; already published stay resolvable.
OutputBaseFilename=hc-cadconv-{#AppVersion}-win-64
AppComments=Speaks converter protocol {#Protocol}; compatibility is decided by the protocol handshake, not by matching the add-on's version.
Compression=lzma2/max
SolidCompression=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible

; ── what the artist sees ──────────────────────────────────────────────────────
; the LGPL text IS shown: the package is mostly OCCT, and the licence is the
; reason the dlls sit next to the exe as separate replaceable files
LicenseFile={#OcctSrc}\LICENSE_LGPL_21.txt
InfoBeforeFile=readme-before.txt
WizardStyle=modern
SetupLogging=no

[Languages]
Name: "en"; MessagesFile: "compiler:Default.isl"

[Files]
; the WHOLE onedir folder — copying the bare exe out of it is the classic
; mistake (`backend._LOADER_STATUS` exists to explain exactly that)
Source: "{#Payload}\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs
; LGPL obligations travel with the binaries
Source: "{#OcctSrc}\LICENSE_LGPL_21.txt";      DestDir: "{app}\licenses"; DestName: "OCCT-LGPL-2.1.txt"; Flags: ignoreversion
Source: "{#OcctSrc}\OCCT_LGPL_EXCEPTION.txt";  DestDir: "{app}\licenses"; Flags: ignoreversion
Source: "{#OcctSrc}\..\rapidjson\license.txt"; DestDir: "{app}\licenses"; DestName: "RapidJSON-license.txt"; Flags: ignoreversion

[Icons]
; no Start-menu clutter for a tool nothing launches by hand — the add-on calls it

[Run]
; nothing runs at the end: this is a helper binary, not an app. The add-on's own
; `--protocol` handshake verifies it on first import.

[UninstallDelete]
; the folder is ours end-to-end; leave the parent (%LOCALAPPDATA%\hardcraft)
; alone — the add-on may keep its own data there
Type: filesandordirs; Name: "{app}\licenses"
