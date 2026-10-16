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
class FrameTimer {
    HANDLE handle_ = nullptr;
  public:
    FrameTimer() {
        handle_ = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
        if (!handle_)
            handle_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    ~FrameTimer() { if (handle_) CloseHandle(handle_); }
    FrameTimer(const FrameTimer&) = delete;
    FrameTimer& operator=(const FrameTimer&) = delete;
    HANDLE get() const { return handle_; }
    void arm(int64_t us, LONG period = 0) {
        LARGE_INTEGER due{};
        due.QuadPart = -std::max<int64_t>(1, us) * 10;
        if (handle_)
            SetWaitableTimer(handle_, &due, period, nullptr, nullptr, FALSE);
    }
    void cancel() { if (handle_) CancelWaitableTimer(handle_); }
};
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
