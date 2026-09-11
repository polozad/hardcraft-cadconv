## hc-cadconv — hardcraft's CAD converter as a native OCCT executable.
##
## The C++ port of the python reference implementation: same argv, same
## sidecars, same `--protocol` handshake, no python
## and no VTK. Built against the conda-forge `occt 7.9.3 novtk` binaries — the
## SAME OCCT the OCP wheel carries, so the mesher output stays comparable to the
## python reference path (that comparison IS the gate).
##
## Configure with the OCCT tree on the prefix path (tools/cadconv/build.py does
## this for you):
##   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
##         -DCMAKE_PREFIX_PATH=<occt>/Library   (win) | <occt>  (linux/mac)
cmake_minimum_required(VERSION 3.21)
project(hc_cadconv CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(OpenCASCADE REQUIRED CONFIG)
message(STATUS "OpenCASCADE ${OpenCASCADE_VERSION} at ${OpenCASCADE_INSTALL_PREFIX}")

add_executable(hc-cadconv
  src/main.cpp
  src/convert.cpp
  src/oracle.cpp
  src/parts.cpp
  src/refine.cpp
)
target_include_directories(hc-cadconv PRIVATE ${OpenCASCADE_INCLUDE_DIR})

## MSVC's default is permissive: it binds a temporary to a NON-const reference,
## which clang and gcc reject outright — `src/refine.cpp` shipped with exactly
## that and only the macOS runner found it. Compile strict on the dev box so the
## next one dies here instead of costing a CI round trip.
if(MSVC)
  target_compile_options(hc-cadconv PRIVATE /permissive- /W3)
endif()

## EXPLICIT toolkit list, not ${OpenCASCADE_LIBRARIES}: every linked toolkit is
## a dll that must ship next to the exe (and that the loader opens at start).
## Linking all 50 would triple the download for nothing.
##   TKDESTEP  STEP reader        TKDEGLTF/TKRWMesh  glTF writer
##   TKBinXCAF the "BinXCAF" document format the reader transfers into
##   TKService RWGltf's image/texture types (no visualization is used)
target_link_libraries(hc-cadconv PRIVATE
  TKernel TKMath TKG2d TKG3d TKGeomBase TKGeomAlgo
  TKBRep TKTopAlgo TKPrim TKShHealing TKMesh
  TKXSBase TKDESTEP TKRWMesh TKDEGLTF TKService
  TKCDF TKLCAF TKCAF TKXCAF TKBinXCAF
)

## The exe finds its own libs next to itself on every platform: win = dll in the
## same folder (no rpath concept), linux/mac = a relative rpath. Keeping this in
## CMake is what lets `build.py` stay a dumb copier.
if(UNIX AND NOT APPLE)
  set_target_properties(hc-cadconv PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "$ORIGIN")
elseif(APPLE)
  set_target_properties(hc-cadconv PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE INSTALL_RPATH "@loader_path")
endif()
