# Third-party software

NCO is distributed under GPL-3.0-or-later (see LICENSE). The static FFmpeg build enables GPL x264 and may include GPLv3 components, so the combined binary is distributed under GPLv3 or later. No nonfree FFmpeg features are enabled.

Dependencies are built by vcpkg at the exact baseline in vcpkg.json: FFmpeg, x264, libvpx, Opus, oneVPL, AMD AMF headers, and NVIDIA codec headers, plus their build dependencies. Original upstream license and copyright notices are installed under `licenses/`.

The release's `NCO-Corresponding-Source.zip` includes NCO source and downloaded dependency source archives; the pinned vcpkg baseline and `vcpkg.json`, `triplets/`, CMake files, and workflow provide the build instructions. Dependency source archives may include unused host build tools; they are not installed with NCO. Source for the exact vcpkg build scripts: https://github.com/microsoft/vcpkg/tree/c52f7bd748185ef0e0daf033d9f2ddfc8a3449ea.

Windows, Direct3D, WASAPI, and Windows Graphics Capture are operating-system components. Hardware encoders require compatible hardware and vendor display drivers; NCO does not install drivers.
