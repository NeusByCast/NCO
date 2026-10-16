# NCO 0.1 engineering preview

Download **NCO-Setup.exe** to install the native x64 Windows application. It installs to Program Files by default, supports a custom destination, Start Menu/optional desktop shortcuts, Installed Apps, and a normal uninstaller. Settings live in `%LOCALAPPDATA%\NCO` and are preserved on update and uninstall.

Native C++20 brown Direct2D UI; WGC and Desktop Duplication capture; actual FFmpeg library encoders and containers; WASAPI mic/desktop audio; packet replay; global hotkeys; immediate settings persistence; bounded logging; Inno Setup installer.

This release is **not yet fully validated on real recording hardware**. CI only runs compilation, synthetic-media tests, and installer/UI-launch smoke tests. Read TESTING.md for unverified capture, audio, GPU, replay, DPI, performance checks, and known limits. No lag-free or benchmark claims are made.

Source and build scripts are in this repository. The corresponding-source asset contains source archives for the statically linked libraries. The installer does not bundle or invoke the FFmpeg command-line application.
