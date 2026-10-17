# NCO 0.4

- Notifications explicitly reassert topmost placement without activating the window. Their first frame is fully readable; rapid repeated actions replace the same visible popup without restarting a fade from near-zero opacity. Recording/replay encoding and capture exclusion are unchanged.

- Replay requests keyframes at the encoder's normal two-second interval, with retries when a boundary frame cannot be submitted, instead of forcing an IDR every second. Replay clips remain keyframe-aligned and can include up to one GOP of extra history. Buffer insertion appends ordered packets directly; GOP eviction removes a counted prefix without repeatedly scanning it. Resolution, frame rate, bitrate, quantizer limits and capture routing are unchanged. Hardware stutter still requires confirmation on the affected PC.

- Moving/resizing the app pauses preview acquisition and UI animations, retains the last image, and resumes preview on release. The native move loop no longer drives preview repaint timers. Record requests show one immediate Starting popup; output-file readiness refreshes the dashboard without a second popup. Notifications read current engine status and present their first frame immediately.

- Opening the capture-source selector discovers current windows, preserving the selected display/window/region without periodic scanning or audio-device enumeration. Minimized windows remain listed and ask to be restored before selection. Browser windows show their active tab title; tabs within one browser window are not separate window-capture sources. Recording and preview rendering are unchanged.

- Region selection requires an explicit mouse drag; it ignores the release that opened the selector. Selecting a new area replaces the previous custom-region entry.
- Controls activate on mouse release. Dropdowns reveal from the edge next to their field, constrain their viewport, and disable clipped rows during animation. Hotkey warnings have separate space below their row.
- Window source titles refresh when browser tabs change. Source changes clear old preview pixels immediately and reject frames from previous configurations; WGC consumes the newest queued frame.
- A separate encoder worker uses two immutable GPU mailboxes and a bounded packet queue. Capture/audio no longer directly wait for encoder submission. Keyframe preroll preserves audio while asynchronous recording/resume starts.
- Cross-GPU NV12 readback uses three staging slots with nonblocking polling during capture. Original timestamps are retained; stop drains pending frames. Full queues drop input instead of accumulating latency.
- Recording settings now default to 35,000 kbps and allow typed bitrate entry; zero remains available for automatic bitrate on older or manually edited configurations.
- Added VBR peak constrained, CQP and CBR controls. VBR uses the selected bitrate as target and 1.5x as peak; CBR caps at target, and CQP uses quality-preset QP values.
- AMF Low/Medium/High/Very High/Ultra map to speed/balanced/balanced/balanced/balanced. All recording presets disable pre-analysis and lookahead to prevent the bounded hardware surface queue from stalling before its first output packet.
- Notifications render one premultiplied bitmap and animate its DWM position/alpha. No repeated font creation, window-region replacement, or paint-based redraw; placement stays fixed to the monitor chosen at notification start.
- Diagnostics include encoder queue depth and readback rejection in drop counts.
- Recording pauses live-preview scaling/readback to leave more GPU bandwidth for the game and encoder; the dashboard explains when preview is paused.
- The custom title bar no longer requests a native caption, preventing duplicate Windows minimize/maximize/close glyphs while retaining native resizing.
- Automatic remux now logs whether it is skipped because MKV is already selected, queued, started, completed, or failed. Successful remux completion is reported; the settings hint explains the MKV behavior.
- High, Very High and Ultra use higher motion recording bitrates; AMD AMF uses its Balanced workload for Medium through Ultra; VBR quantizer limits and CQP values set image quality independently. NVIDIA Very High/Ultra select NVENC P5/HQ. Diagnostics record the effective preset, rate-control mode, peak rate and 4:2:0 pixel format.

Automated Windows tests exercise region input messages, region replacement, popup release/clip handling, cached notification rendering at four requested DPI values, and a deliberately slow software encoder worker. These are not physical-GPU benchmarks or proof of smoothness on the user's laptop. Real-game FPS, GPU readback, live capture/audio synchronization, physical DPI layouts, and notification capture exclusion still require hardware testing.

Download NCO-Setup.exe. Updates preserve user settings.

Replay packets are kept in timestamp order when asynchronous video arrives after audio; initial audio is retained until its first video keyframe. Logs separately show GPU readback queue occupancy/rejections and CPU encoder queue depth.

Dropdown scrolling uses the number of visible rows, exposes a scroll indicator, and supports arrow-key selection plus Enter. Windows regression tests select offscreen options through actual wheel messages.

- Dashboard preview targets 60 FPS and wakes rendering on completed frames, consuming the newest ready thumbnail instead of accumulating a FIFO delay. Animations use a separate on-demand high-resolution clock at the monitor refresh rate (60–240 Hz); static pages do not repaint for notification animation ticks.
- Recording FPS choices are 15, 30 and 60 only. The custom field and Custom dropdown option are removed; unsupported saved FPS values migrate to 60.
- Recording files open only after the encoder produces a video keyframe; the dashboard shows Starting until then. GPU conversion/readback work is submitted explicitly.
- Capture follows the selected frame rate. Muted audio inputs stop their capture workers. Preview reuses processor views and does not overwrite pending readback slots.
- UI redraws reuse text layouts and avoid a full-page compositing layer outside navigation fades. Successful recording validation checks the initial keyframe without rescanning the entire video.

- AMF recording disables preencode and permits one-frame processing overlap; it retains bounded queues, no B frames and no lookahead. Ultra VBR caps quantization at QP 22 for H.264/HEVC; CQP remains QP 19. AV1 uses its corresponding quantizer index scale. CBR retains its bitrate constraint.
- Recording conversion disables driver automatic video processing and reuses bounded input/output processor views. Static conversion state is configured once per pipeline.
- Diagnostics distinguish submitted frames from encoded output and report available packet QP statistics. Recording tests verify saved resolution and BT.709 limited-range metadata.

- Auto display/region capture uses Desktop Duplication on the display adapter first, keeping conversion off the gaming GPU on hybrid systems. Hardware encoding tries the compatible capture adapter first; other hardware adapters retain the bounded asynchronous NV12 transfer fallback. Explicit WGC selection and window capture retain their preferred-GPU path.

- Desktop Duplication retains its frame between capture ticks, releases just before the next acquisition, and cleans up outstanding ownership on errors and destruction. A deterministic Windows regression exercises actual Capture calls, including empty ticks and access loss.
- AMF can encode textures on an integrated AMD capture GPU even when another AMD GPU ranks faster. This removes the forced GPU-to-RAM-to-GPU route on compatible hybrid systems. Encoder startup still validates support and retains the prior fallbacks. Quality, quantization limits, bitrate, output dimensions and frame-rate settings are unchanged.

- Auto capture evaluates encoder-local WGC at startup when display-adapter encoding falls back to hardware frame upload. The retry keeps the successful codec and all image settings, verifies unchanged source dimensions, and requires direct GPU-texture encoding. Unsupported retries restore the compatible display capture path; explicit capture backends are respected. No runtime performance-monitor process or dependency is added.

- Preview and UI animations stop while minimized or recording. Restoring the dashboard requests a fresh preview even for a static source; render-target recreation can restore the last displayed image. Window resizing coalesces render-target changes into paint events. Recording, encoder quality, audio and file finalization are unchanged by these GUI changes.

### Native-size colour sampling

- Added a GPU chroma filter for native-size NV12 conversion. The supplied frame comparison showed the video processor taking each colour sample from just the top-left RGB pixel, which aliases fine coloured text and can turn green strokes purple.
- The filter averages vertically and uses a horizontal low-pass filter aligned to the existing LEFT chroma location. It writes only the NV12 colour plane; luma, hardware encoding, bitrate, frame timing and capture routing retain their existing paths. No CPU frame readback or intermediate frame copies are added.
- Writable NV12 planes are checked on the active device. Unsupported drivers retain video-processor conversion and log the reason. Resized output continues using the existing video processor.

### Ultra H.264 text detail

- Tightened AMD H.264 Ultra VBR's maximum quantizer from 22 to 8 to reduce loss of thin strokes and anti-aliased text, including at startup and after scene changes. Other quality levels, CQP/CBR, and HEVC/AV1 retain their settings.
- The hardware Balanced preset, colour-sampling correction, capture routing and frame pacing remain in use. Ultra can produce substantially larger files or retain less replay history at the existing memory limit; the bitrate setting remains the VBR target. This is lossy 4:2:0 encoding and does not guarantee pixel-identical desktop text.

### Recording quality settings

- AMD H.264 Ultra CQP now requests QP 8, matching Ultra VBR's existing maximum QP of 8, instead of weakening to QP 19 when switching modes. This can increase CQP file sizes and reduce replay history at the existing memory cap. Other presets, codecs, VBR and CBR retain their encoding settings.
- AMF processing-preset, rate-control and fixed-quantizer option errors now fail initialization so a rejected request cannot silently continue with codec defaults.
- Recording settings explain automatic bitrate (0), VBR targets and CBR's bitrate constraint. The bitrate field is disabled in CQP, where it is ignored; its saved value is retained for the other modes.
- This corrects confirmed settings inconsistencies. The remaining thin-text complaint still needs runtime confirmation; no capture, colour-filter, frame-queue or GPU-routing change is included.

### Replay enabled during recording finalization

- Enabling replay after an encoder drain has started now waits for the drain to complete, releases the finished worker, and starts a new replay pipeline. Previously the replay flag could close the recording early and retain an encoder that no longer accepted frames.
- Added a focused stop-decision check for replay changes during a drain. Physical GPU capture, folder-picker permissions and every fallback encoder's rate-control behavior are not certified by the existing synthetic recording checks.

### Recording option verification

- Corrected VP9 CQP/CBR mode selection and Media Foundation's missing explicit rate control. Fixed the Windows encoder's quantizer argument to use the API's required UINT64 type; rejected modes/quantizers now fail initialization, and available mode readback must match the request.
- Added VBR quantizer limits for software H.264, NVENC and H.264/HEVC QSV so a manual bitrate no longer bypasses the preset's detail limit. NVENC forced keyframes are IDRs for decodable recording/replay boundaries. Existing processing presets, capture routing and bounded queues remain in use.
- Expanded verification to 180 H.264/VP9 encode/decode cases covering FPS, presets, rate modes and automatic/manual bitrates; checks use runtime encoder modes and actual packet quantizers. Every resolution preset, including 4K, is decoded from each software encoder. Available hardware encoders are attempted and unavailable devices are reported separately.
- Recording UI checks cover all frame rates, presets, resolution choices, formats, available encoders, replay toggle/durations, numeric persistence, CQP bitrate disabling and recording locks. Native folder selection/cancellation and Unicode folder writes are exercised. Reports are included as a separate Windows build artifact.
