# NCO 0.2 preview

Download NCO-Setup.exe. Updating preserves saved recording/audio/hotkey settings.

- Readable native installer pages with brown branding artwork.
- Five cleaner pages; Replay settings live in Recording, with replay clips saved beside recordings using Replay_ names. Appearance controls, slogans, breadcrumb, preview toggle and instructional clutter removed.
- Whole-row hover highlights; repeated dropdown click closes it; Alt-key system sound suppressed; dark initial window background/border.
- Preview targets 60 FPS via asynchronous readback and an independent high-resolution UI timer. Rendering stops when minimized.
- Display capture defaults to Desktop Duplication to avoid the yellow WGC indicator. Window capture remains subject to Windows capture-border permissions.
- One-second, top-right recording/replay notifications.
- AMD adapter-aware encoder selection, hardware-only Media Foundation alternatives, and detailed initialization/fallback logging.
- Audio packet continuity across small timestamp jitter; low-level noise suppression and DC removal.

Hardware encoding, actual 60 FPS preview/recording, endpoint buzz, notification exclusion, DPI and long-run performance still require real-PC retesting. The supplied clip showed approximately 35 recorded FPS and very quiet audio noise; no claim of a measured performance fix on that machine is made. See TESTING.md for limits and automated checks.

Source and build scripts, including statically linked dependency source, are included in the corresponding-source release asset.
