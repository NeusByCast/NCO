#pragma once
#include "core.hpp"
#include <atomic>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
namespace nco {
template <class T> using Com = Microsoft::WRL::ComPtr<T>;
inline void check(HRESULT hr, const char* what) {
    if (FAILED(hr))
        throw std::runtime_error(std::string(what) + " (HRESULT " + std::to_string(uint32_t(hr)) +
                                 ")");
}
std::string utf8(const std::wstring& s);
std::wstring wide(const std::string& s);
int64_t clockUs();
std::filesystem::path appDir();
std::filesystem::path videosDir();
std::filesystem::path uniquePath(const std::filesystem::path& dir, const std::string& prefix,
                                 const std::string& ext);
std::wstring folderDialog(HWND owner, const std::wstring& initial);
std::wstring shortcutText(Shortcut s);
std::optional<RECT> selectRegion(HWND owner, HMONITOR monitor);
bool captureExclusionSupported();
std::wstring hardwareInfo();
class Log {
    std::mutex mutex_;
    std::atomic<bool> verbose_ = false;

  public:
    void verbose(bool v) {
        verbose_ = v;
    }
    void write(const std::string& s, bool detail = false);
    static Log& get();
};
class Hotkeys {
    HWND hwnd_ = nullptr;

  public:
    explicit Hotkeys(HWND hwnd) : hwnd_(hwnd) {}
    std::vector<std::wstring> apply(const std::vector<Shortcut>& keys);
    ~Hotkeys();
};
} // namespace nco
