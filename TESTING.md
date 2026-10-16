# Validation status

This is an engineering preview, not a fully hardware-qualified release. A successful CI workflow verifies compilation, synthetic media, concurrent file exports, and installer smoke tests only. Do not infer gaming performance or real audio synchronization from those checks.

## Executed locally

- Linux GCC 13 C++20 core tests, also under AddressSanitizer/UBSan with LeakSanitizer disabled because the container cannot inspect process threads: integer frame deadlines at 15/30/60/96/144/240 FPS (no early wakeup or duplicate timestamp); aspect-preserving letterbox geometry; resolution constraints and even dimensions; bitrate ordering; WebM codec compatibility; duplicate shortcut detection; replay GOP trimming and hard byte limit. Passed.

## Automated Windows checks (results are in Actions)

- MSVC x64 Release compile, static CRT and dependency libraries.
- Core tests via CTest.
- Windows shortcut integration: real RegisterHotKey single keys/combinations, duplicate detection and controlled ownership conflict; real key delivery to a minimized test window when desktop input injection is available. The report distinguishes passed checks from unavailable input tests.
- PE import inspection rejects external Visual C++ and FFmpeg runtimes.
- `NCO.exe --self-test`: synthetic video and 440 Hz stereo audio; MP4, MKV, MOV, WebM muxing; A/V packet timestamp comparisons; pause timeline; 15/30/60 FPS and all quality levels; native/custom scaling; accelerated 15/30/60-second replay snapshots; concurrent recording/replay exports; MKV-to-MP4 remux with codec and packet-count comparisons. These do not exercise real-time capture or endpoint audio.
- `NCO.exe --ui-smoke-test`: create/render all seven actual native pages and a 920x540 dashboard, capture screenshots, then operate real controls through Win32 mouse/key messages. Checks dropdown selections, numeric edits, independent mute/preview/remux toggles, settings persistence, audio/replay configuration, interactive hotkey capture/clear/duplicate warnings, appearance/advanced toggles and native minimize/maximize/restore. Results go to `nco-self-test/ui-results.txt`; original settings are restored afterward. This does not establish real capture/audio operation, every control, animation quality or DPI correctness.
- Inno Setup silent installation, installed self-test, installed UI launch, shortcuts, registry entry, update/settings persistence, uninstall.

## Requires suitable Windows machines — UNVERIFIED

- WGC monitor/window capture and custom region correctness; Desktop Duplication fallback; monitor rotation; HDR tone mapping; minimized/closed capture sources; protected content.
- Actual NVENC, AMF, Quick Sync H.264/HEVC/AV1 capabilities, GPU texture conversion, multi-GPU behavior and resolution/FPS limits. Opening an encoder at 640x360 is a capability probe, not a full recording test.
- Microphone/desktop WASAPI capture, live mute/gain/device switching, device disconnection recovery, crackling/clipping, long-duration A/V drift and independent audio-track playback.
- 15/30/60/custom FPS; 720p through 4K/custom output; all quality presets; 60+ minute captures; storage exhaustion and slow-disk behavior.
- Full 15/30/60-second replay duration, simultaneous recording, rapid repeat saves, real replay A/V sync. Buffer uses a 256 MiB cap and may retain less time at high bitrates; clips begin on a keyframe.
- Single-key/modifier global shortcuts in games, conflicts, restart persistence, minimized behavior.
- Every interactive control; hover/dropdown/toggle/navigation transitions; spacing, clipping and resizing at real Windows 100/125/150/200% display scales. UI launch smoke tests do not establish visual quality.
- Notification exclusion from WGC and Desktop Duplication recordings and actual 0.5-second behavior.
- Clean-machine installation with no development runtimes, non-default path, interactive install/uninstall, taskbar icon, update during a recording, Windows snapping.
- CPU/RAM/GPU load and recording frame pacing on real games. No performance benchmark has been measured. Missed capture deadlines and encode duration are runtime diagnostics, not FPS guarantees.

## Known implementation limits

- Windows 10 1903+ x64 is required; Windows 10 2004+ is recommended for capture-excluded notifications. Toasts are suppressed when exclusion cannot be requested. A brown in-window status message is used instead.
- SDR BGRA capture with BT.709 NV12/YUV420 output. HDR tone mapping is not implemented.
- Hardware uses D3D11 textures when NVENC/AMF accepts them; Intel and hardware fallback paths use CPU readback/upload. GPU processing support varies by driver. Software VP9 at high resolutions is expensive.
- Active recording/replay locks capture and encoding settings. Disabling replay allows changes. Device changes restart the audio endpoints; missing devices emit a visible error and the affected track becomes silence until reselected/refreshed.
- Window resizing during capture currently stops the pipeline with a recoverable error. A window source must remain available; minimized applications may stop producing frames.
- One encoding worker consumes the OS capture pool; audio endpoints and disk output have independent workers. Preview reads back only a GPU-scaled thumbnail at 8 FPS and stops when minimized.
- Recording pause affects the recording writer; replay continues. Audio at pause/resume boundaries can differ by an encoder frame. Synchronization uses QPC endpoint timestamps at 48 kHz; long-run behavior is unverified.
- Replay exports/remux/verification have a separate background-priority worker. Recording writes have a 64 MiB queue; exports have a 256 MiB encoded-packet budget. A saturated storage device can still miss recording deadlines. Recording queue exhaustion stops the recording with a visible error; a full replay queue rejects only the replay save. Source/recovery files are retained on failure.
- Brown controls are custom Direct2D. Full accessibility/UI Automation is not implemented. Native maximize hit testing is present, but Windows 11 Snap Layout hover behavior still requires testing. No claim of pixel-perfect UI is made.
- Installer and executable are unsigned. No updater service, system tray, or external FFmpeg application is included.
