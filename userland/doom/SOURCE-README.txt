Doomgeneric/Freedoom corresponding source
========================================

The source archives here preserve the exact pinned upstream engine, Freedoom
data and DeuTex files. The sidecar manifests record per-file Git blob IDs and
SHA-256 values. port/sources.json describes the Doomgeneric archive. The port/
directory contains all owned native backend code and regression harnesses.

To overlay these files on a Coast checkout:
  - Copy port/ to userland/doom/.
  - Copy the .tar.xz archives and Freedoom/DeuTex .json sidecars to userland/vendor/.
  - Copy build_doom.py, build_freedoom.py and fetch_sources.py to tools/.
  - Copy include/uapi/audio.h and include/uapi/window.h to the same relative
    paths in that checkout.
  - Use the pinned static musl compiler at build/native-musl/bin/musl-gcc.
  - Run python3 tools/build_doom.py --engine-only for the executable, or run
    python3 tools/build_doom.py for engine and source-built Freedoom packaging.

The isolated engine build requires no SDL or X11 libraries. A compatible
static x86-64 musl compiler may be installed at the documented compiler path.
Freedoom's host build requirements and source provenance are described by
its original COMPILING.adoc, the build_freedoom.py docstring, and its source
manifest; host build tools are not installed by these scripts.

The original engine source's optional screenshots are excluded. All code,
headers, build files and license texts are retained. The archive is a locally
reconstructed deterministic source bundle, rather than a GitHub release asset.
No proprietary IWAD is provided. Freedoom data is BSD-3-Clause; Doomgeneric and
the native backend are GPL-2.0-or-later. Retain the upstream license and data
credits when redistributing a built image.
