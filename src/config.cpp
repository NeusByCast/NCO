#include "config.hpp"
#include <fstream>
#include <map>
#include <sstream>
namespace nco {
Config Config::load() {
    Config c;
    c.directory = videosDir().wstring();
    c.replayDirectory = (videosDir() / L"Replays").wstring();
    std::ifstream f(appDir() / L"settings.ini");
    std::map<std::string, std::string> v;
    std::string line;
    while (std::getline(f, line)) {
        auto p = line.find('=');
        if (p != std::string::npos)
            v[line.substr(0, p)] = line.substr(p + 1);
    }
    auto num = [&](const char* k, int def, int lo, int hi) {
        try {
            auto it = v.find(k);
            return it == v.end() ? def : std::clamp(std::stoi(it->second), lo, hi);
        } catch (...) {
            return def;
        }
    };
    auto str = [&](const char* k, std::string d) {
        auto i = v.find(k);
        return i == v.end() ? d : i->second;
    };
    c.fps = num("fps", 60, 15, 60);
    if (c.fps != 15 && c.fps != 30 && c.fps != 60)
        c.fps = 60;
    c.captureBackend = num("captureBackend", 0, 0, 2);
    c.resolutionMode = num("resolution", 0, 0, 5);
    c.width = num("width", 1920, 64, 7680);
    c.height = num("height", 1080, 64, 4320);
    c.quality = num("quality", 2, 0, 4);
    c.bitrateKbps = num("bitrate", 0, 0, 150000);
    c.replaySeconds = num("replaySeconds", 30, 15, 60);
    if (c.replaySeconds != 15 && c.replaySeconds != 30 && c.replaySeconds != 60)
        c.replaySeconds = 30;
    c.encoder = str("encoder", "auto");
    c.format = str("format", "mp4");
    if (c.format != "mp4" && c.format != "mkv" && c.format != "mov" && c.format != "webm")
        c.format = "mp4";
    c.directory = wide(str("directory", utf8(c.directory)));
    c.replayDirectory = wide(str("replayDirectory", utf8(c.replayDirectory)));
    c.micDevice = wide(str("micDevice", ""));
    c.desktopDevice = wide(str("desktopDevice", ""));
#define BOOL(name, def) c.name = num(#name, def, 0, 1) != 0
    BOOL(remux, 1);
    BOOL(mic, 1);
    BOOL(desktop, 1);
    BOOL(separateTracks, 0);
    BOOL(replay, 0);
    BOOL(preview, 1);
    BOOL(animations, 1);
    BOOL(compact, 0);
    BOOL(verbose, 0);
#undef BOOL
    c.micVolume = num("micVolume", 100, 0, 100);
    c.desktopVolume = num("desktopVolume", 100, 0, 100);
    c.micGain = num("micGain", 100, 0, 300);
    c.audioOffsetMs = num("audioOffsetMs", 0, -500, 500);
    for (size_t i = 0; i < 5; ++i) {
        auto k = "key" + std::to_string(i), m = "mods" + std::to_string(i);
        c.keys[i].key = num(k.c_str(), int(c.keys[i].key), 0, 255);
        c.keys[i].mods = num(m.c_str(), 0, 0, 15);
    }
    c.preview = true;
    c.animations = true;
    c.compact = false;
    c.replayDirectory = c.directory;
    Log::get().verbose(c.verbose);
    return c;
}
void Config::save() const {
    auto temp = appDir() / L"settings.tmp", target = appDir() / L"settings.ini";
    std::ofstream f(temp, std::ios::trunc);
    if (!f)
        throw std::runtime_error("Cannot save settings");
#define WRITE(name) f << #name "=" << name << "\n"
    WRITE(fps);
    WRITE(captureBackend);
    f << "resolution=" << resolutionMode << "\n";
    WRITE(width);
    WRITE(height);
    WRITE(quality);
    f << "bitrate=" << bitrateKbps << "\n";
    WRITE(replaySeconds);
    WRITE(encoder);
    WRITE(format);
    f << "directory=" << utf8(directory) << "\nreplayDirectory=" << utf8(replayDirectory)
      << "\nmicDevice=" << utf8(micDevice) << "\ndesktopDevice=" << utf8(desktopDevice) << "\n";
    WRITE(remux);
    WRITE(mic);
    WRITE(desktop);
    WRITE(separateTracks);
    WRITE(replay);
    WRITE(preview);
    WRITE(animations);
    WRITE(compact);
    WRITE(verbose);
    WRITE(micVolume);
    WRITE(desktopVolume);
    WRITE(micGain);
    WRITE(audioOffsetMs);
#undef WRITE
    for (size_t i = 0; i < keys.size(); ++i)
        f << "key" << i << "=" << keys[i].key << "\nmods" << i << "=" << keys[i].mods << "\n";
    f.flush();
    if (!f)
        throw std::runtime_error("Settings write failed");
    f.close();
    if (!MoveFileExW(temp.c_str(), target.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot replace settings file");
}
} // namespace nco
