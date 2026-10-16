# NCO

A native C++20 Windows recorder with a warm brown interface, D3D11 capture and processing, hardware encoding, WASAPI audio, and encoded instant replay. See [TESTING.md](TESTING.md) for current validation coverage and known limits.

## Install

Download **NCO-Setup.exe** from [Releases](https://github.com/NeusByCast/NCO/releases). Windows 10 1903+ x64, preferably 2004+, with current GPU drivers. The installer registers a real desktop app and uninstaller. No separate FFmpeg program or runtime installer is needed.

## Use

Choose a display or window on the dashboard and start recording. Configure FPS, resolution, quality and encoder in Recording settings. F9 starts/stops, F10 pauses/resumes, F8 saves replay by default. Microphone and speaker buttons mute independently. Enable replay in Recording settings; it continues independently of recording. Configure all shortcuts in Hotkeys, including single keys. Valid settings save immediately to `%LOCALAPPDATA%\NCO\settings.ini`.

Encoding/capture settings are locked only during normal recording. Replay-only settings changes restart the encoded buffer with the new configuration; clips already queued for export are unaffected. Audio devices and mute/volume can change live. Window resizing preserves the recording dimensions and aspect ratio; display-mode transitions retry Desktop Duplication on the same device. Device removal/disconnection may require reselecting the source. Errors appear in the status area and `%LOCALAPPDATA%\NCO\diagnostic.log`.

Settings shows detected GPU adapters and memory, live process CPU/RAM use, encoder timing and missed deadlines. Capture compatibility can use Auto, force Windows Graphics Capture, or force Desktop Duplication for a display/region. A saved encoder that is unavailable on the current machine falls back to Auto after startup detection.

Automatic remuxing records to a recoverable MKV and stream-copies to MP4/MOV/WebM after stopping; source is removed only after output verification. Recovery files remain if remux fails. WebM requires AV1 or VP9 and Opus; unavailable encoders are not offered. Auto selects compatible hardware, then software.

Replay clips use the recording folder with a `Replay_` filename prefix. The five pages are Dashboard, Recording, Audio, Hotkeys and Settings. Preview is up to 960 pixels wide at 30 FPS, reduced to 10 FPS when replay encoding approaches its frame budget. Unchanged preview frames do not trigger redraws. Preview capture/rendering pauses during recording and while minimized. Notifications last one second at the top-right.

Auto capture prefers Desktop Duplication for displays/regions, avoiding the Windows capture outline. Window capture uses WGC; border suppression depends on Windows permission/support. Preview targets 30 FPS using asynchronous thumbnail readback and a shared UI paint timer. Hardware/driver load can reduce actual FPS.

AMD probing tries each AMD adapter explicitly; Auto also tries hardware-only Windows Media Foundation before CPU encoding. Failed probes and recording initialization errors are logged, including FFmpeg error messages. A GPU using uploaded frames is still hardware encoding.

## Build

Use Visual Studio 2022 C++ desktop tools, Windows SDK 10.0.19041+, CMake 3.25+, Python with Pillow (icon generation only), and Inno Setup 6. Clone the pinned vcpkg revision from `vcpkg.json`, bootstrap it, then:

```powershell
python tools/icon.py
python tools/prepare_vcpkg.py <vcpkg> build/vcpkg-overlay
cmake -S . -B build -A x64 "-DCMAKE_TOOLCHAIN_FILE=<vcpkg>/scripts/buildsystems/vcpkg.cmake" "-DVCPKG_TARGET_TRIPLET=x64-windows-nco" "-DVCPKG_OVERLAY_TRIPLETS=triplets" "-DVCPKG_OVERLAY_PORTS=build/vcpkg-overlay"
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
build/Release/NCO.exe --self-test
```

The Actions workflow prepares licenses, corresponding source and distribution files, compiles `installer/nco.iss`, tests the installer, and uploads the Windows setup as a GitHub Actions artifact. Outputs are under `output/NCO-Setup.exe`. Static linkage includes GPL x264; NCO and its combined binary are GPLv3-or-later.

Portable core checks on Linux:

```sh
g++ -std=c++20 -O2 -I src tests/core_tests.cpp -o nco-core-tests
./nco-core-tests
```

Auto prefers the high-performance GPU vendor and AMD adapter, with software encoding as a last resort. WGC window capture requests the high-performance GPU. Full-display Desktop Duplication must use the adapter driving that display; on hybrid laptops, cross-GPU hardware encoding uses GPU NV12 scaling/color conversion followed by a smaller NV12 readback/upload. This is not a universal zero-copy promise. Hardware texture frames use separate NV12 textures instead of arrays.

Settings includes **Remux a recording**, an asynchronous stream-copy export that preserves the source and rejects existing destination files. Codec/container mismatches are rejected by the muxer. **Export diagnostics** creates NCO-diagnostic.txt with Windows build, adapter/driver details, current configuration, available encoders, output dimensions, actual FPS/bitrate, per-stage timing, frame-drop/resource counters and the bounded log. The log always samples active pipelines once per second and includes UTC timestamps and thread IDs. Verbose logging remains optional; no captured media is logged.

0.4 uses a separate bounded video worker (two GPU mailboxes) and three nonblocking NV12 staging slots for hybrid hardware uploads. Source changes discard stale preview generations, and browser/window labels refresh. Region selection replaces one custom source and requires an actual drag. Dropdowns reveal from their owning field with clipped hit targets disabled. Notifications reuse a premultiplied bitmap rather than repainting GDI text/regions every frame. Automatic quality presets now allow more bitrate for moving scenes; game performance remains hardware-dependent.
