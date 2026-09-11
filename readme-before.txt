Hardcraft CAD Tools — the STEP converter for the Hardcraft Blender add-on.

WHAT THIS IS
  A command-line converter the add-on calls when you import a STEP file. It
  reads STEP and writes glTF plus sidecar data. Nothing launches it by hand and
  nothing appears in the Start menu.

  The add-on works without it (Plasticity bridge, OBJ). This package is only
  needed for STEP import.

WHERE IT GOES
  %LOCALAPPDATA%\hardcraft\bin\hc-cadconv

  Per user. No administrator rights, no UAC prompt, no service, no autostart,
  nothing written outside that folder. Uninstall it from Apps & features like
  anything else.

IT IS NOT SIGNED
  There is no code-signing certificate behind this project, so Windows
  SmartScreen may warn you once. Verify the download instead: every release
  publishes a SHA-256 next to the file at

    https://github.com/polozad/hardcraft-dist/releases

  The converter does not use the network. It reads the STEP file you point it
  at and writes into the output folder the add-on gives it.

OPEN CASCADE
  Most of this package is Open CASCADE Technology (LGPL-2.1), linked
  dynamically. The OCCT libraries install next to the executable as separate
  files and can be replaced with your own build. Their licence and the OCCT
  exception are installed under the "licenses" folder.
