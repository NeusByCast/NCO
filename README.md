# NCO

A native C++20 Windows recorder with a warm brown interface, D3D11 capture/processing, hardware encoding, WASAPI audio and encoded instant replay. **Engineering preview: real hardware recording and performance qualification are still required.** See [TESTING.md](TESTING.md).

## Install

Download **NCO-Setup.exe** from [Releases](https://github.com/NeusByCast/NCO/releases). Windows 10 1903+ x64, preferably 2004+, with current GPU drivers. The installer registers a real desktop app and uninstaller. No separate FFmpeg program or runtime installer is needed.

## Use

Choose a display or window on the dashboard and start recording. Configure FPS, resolution, quality and encoder in Recording settings. F9 starts/stops, F10 pauses/resumes, F8 saves replay by default. Microphone and speaker buttons mute independently. Enable replay in Replay settings; it continues independently of recording. Configure all shortcuts in Hotkeys, including single keys. Valid settings save immediately to `%LOCALAPPDATA%\NCO\settings.ini`.

Encoding/capture settings are locked while recording or replay buffering. Stop recording and disable replay before changing them. Audio devices and mute/volume can change live. A source resize/disconnection may require reselecting the source. Errors appear in the status area and `%LOCALAPPDATA%\NCO\diagnostic.log`.

Advanced settings show detected GPU adapters and memory, live process CPU/RAM use, encoder timing and missed deadlines. Capture compatibility can use Auto, force Windows Graphics Capture, or force Desktop Duplication for a display/region. A saved encoder that is unavailable on the current machine falls back to Auto after startup detection.

Automatic remuxing records to a recoverable MKV and stream-copies to MP4/MOV/WebM after stopping; source is removed only after output verification. Recovery files remain if remux fails. WebM requires AV1 or VP9 and Opus; unavailable encoders are not offered. Auto selects compatible hardware, then software.

## Build

Use Visual Studio 2022 C++ desktop tools, Windows SDK 10.0.19041+, CMake 3.25+, Python with Pillow (icon generation only), and Inno Setup 6. Clone the pinned vcpkg revision from `vcpkg.json`, bootstrap it, then:

```powershell
python tools/icon.py
cmake -S . -B build -A x64 "-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake" "-DVCPKG_TARGET_TRIPLET=x64-windows-nco" "-DVCPKG_OVERLAY_TRIPLETS=triplets"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
build/Release/NCO.exe --self-test
```

The Actions workflow prepares licenses, corresponding source and distribution files, compiles `installer/nco.iss`, tests the installer, and publishes a clearly marked preview only after those checks succeed. Outputs are under `output/NCO-Setup.exe`. Static linkage includes GPL x264; NCO and its combined binary are GPLv3-or-later.

Portable core checks on Linux:

```sh
g++ -std=c++20 -O2 -I src tests/core_tests.cpp -o nco-core-tests
./nco-core-tests
```
