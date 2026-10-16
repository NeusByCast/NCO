#pragma once
#include "platform.hpp"
namespace nco {
struct Config {
    int fps = 60, resolutionMode = 0, width = 1920, height = 1080, quality = 2, bitrateKbps = 0,
        replaySeconds = 30, captureBackend = 0;
    std::string encoder = "auto", format = "mp4";
    std::wstring directory, replayDirectory, micDevice, desktopDevice;
    bool remux = true, mic = true, desktop = true, separateTracks = false, replay = false,
         preview = true, animations = true, compact = false, verbose = false;
    int micVolume = 100, desktopVolume = 100, micGain = 100, audioOffsetMs = 0;
    std::vector<Shortcut> keys = {{VK_F9, 0}, {VK_F10, 0}, {VK_F8, 0}, {0, 0}, {0, 0}};
    static Config load();
    void save() const;
};
} // namespace nco
