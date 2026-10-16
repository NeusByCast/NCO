# NCO 0.4 preview

- Region selection requires an explicit mouse drag; it ignores the release that opened the selector. Selecting a new area replaces the previous custom-region entry.
- Controls activate on mouse release. Dropdowns reveal from the edge next to their field, constrain their viewport, and disable clipped rows during animation. Hotkey warnings have separate space below their row.
- Window source titles refresh when browser tabs change. Source changes clear old preview pixels immediately and reject frames from previous configurations; WGC consumes the newest queued frame.
- A separate encoder worker uses two immutable GPU mailboxes and a bounded packet queue. Capture/audio no longer directly wait for encoder submission. Keyframe preroll preserves audio while asynchronous recording/resume starts.
- Cross-GPU NV12 readback uses three staging slots with nonblocking polling during capture. Original timestamps are retained; stop drains pending frames. Full queues drop input instead of accumulating latency.
- Higher motion-oriented automatic bitrate defaults; AMD peak-constrained VBR with burst headroom and Balanced quality for High/Very High/Ultra. Manual bitrate remains supported.
- Notifications render one premultiplied bitmap and animate its DWM position/alpha. No repeated font creation, window-region replacement, or paint-based redraw; placement stays fixed to the monitor chosen at notification start.
- Diagnostics include encoder queue depth and readback rejection in drop counts.

Automated Windows tests exercise region input messages, region replacement, popup release/clip handling, cached notification rendering at four requested DPI values, and a deliberately slow software encoder worker. These are not physical-GPU benchmarks or proof of smoothness on the user's laptop. Real-game FPS, GPU readback, live capture/audio synchronization, physical DPI layouts, and notification capture exclusion still require hardware testing.

Download NCO-Setup.exe. Updates preserve user settings.

Replay packets are kept in timestamp order when asynchronous video arrives after audio; initial audio is retained until its first video keyframe. Logs separately show GPU readback queue occupancy/rejections and CPU encoder queue depth.

Dropdown scrolling uses the number of visible rows, exposes a scroll indicator, and supports arrow-key selection plus Enter. Windows regression tests select offscreen options through actual wheel messages.

- Dashboard preview redraw is capped at 30 FPS and shares its paint cadence with animations; notification ticks no longer trigger unnecessary full-window redraws.
- Recording FPS choices are 15, 30 and 60 only. The custom field and Custom dropdown option are removed; unsupported saved FPS values migrate to 60.
- The attached diagnostic could not be opened in this session; current recording quality and GPU load still require a readable current diagnostic and real-PC retest.
