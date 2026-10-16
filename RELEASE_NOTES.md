# NCO 0.3 preview

- Auto prefers the high-performance GPU for encoding; game/window WGC capture requests that GPU too. Hybrid full-display capture may still require cross-adapter transfer.
- Replace NV12 render-target arrays with separately pooled textures and allocation preflight. GPU NV12 scaling/readback reduces CPU conversion and transfer size for hardware-upload paths.
- Correct persistent DXGI cursor visibility and use capture-timestamped positions.
- Recover Desktop Duplication after display transitions; fit changing capture sizes to fixed recording dimensions.
- Replay no longer locks Recording settings; changing encoding/source settings restarts the replay buffer.
- Up to 1280-wide preview; 30 FPS preview while encoding to prioritize recording, 60 FPS idle. Paint during native window moves/resizing.
- Hide cloaked/minimized/tool windows and known input/search/start-menu system windows.
- Remove replay-active footer; rename Advanced to Settings.
- Manual background remux keeps original recordings and validates output; refuses overwriting existing files.
- Full diagnostic text export with OS/GPU/driver/configuration, actual dimensions/FPS/bitrate, prepare/submit/capture timings, audio discontinuities, frame drops and bounded UTC/thread-stamped logs.

The supplied 0.2 log confirms AMD hardware upload on RX 5500M, failed texture initialization, and encode spikes above the 60 FPS budget. Improvements are implemented but game performance, driver compatibility, cursor behavior, real audio synchronization and display-transition recovery remain unverified on physical GPUs. No measured FPS improvement is claimed.

See TESTING.md and the matching Actions run for executed automated checks.
