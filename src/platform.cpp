#include "platform.hpp"
#include <cstdio>
#include <dxgi1_2.h>
#include <fstream>
#include <iomanip>
#include <shlobj.h>
#include <shobjidl.h>
#include <sstream>
namespace nco {
std::wstring hardwareInfo() {
    Com<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
        return L"Adapter information unavailable";
    std::wstring result;
    for (UINT i = 0;; ++i) {
        Com<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(i, &adapter) == DXGI_ERROR_NOT_FOUND)
            break;
        if (!adapter)
            break;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc)))
            continue;
        std::wstring name = desc.Description;
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
            name += L" (software)";
        else if (desc.DedicatedVideoMemory)
            name += L" · " + std::to_wstring(desc.DedicatedVideoMemory / 1048576) + L" MiB VRAM";
        if (!result.empty())
            result += L" / ";
        result += name;
        Log::get().write("Detected adapter: " + utf8(name));
    }
    return result.empty() ? L"No display adapter detected" : result;
}
bool captureExclusionSupported() {
    struct Version {
        ULONG size, major, minor, build, platform;
        WCHAR service[128];
    };
    using Fn = LONG(WINAPI*)(Version*);
    auto f = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    Version v{};
    v.size = sizeof(v);
    return f && f(&v) == 0 && v.major >= 10 && v.build >= 19041;
}
std::string utf8(const std::wstring& s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring wide(const std::string& s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring r(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n);
    return r;
}
int64_t clockUs() {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return t.QuadPart / f.QuadPart * 1000000 + (t.QuadPart % f.QuadPart) * 1000000 / f.QuadPart;
}
std::filesystem::path known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    check(SHGetKnownFolderPath(id, 0, nullptr, &p), "Find user folder");
    std::filesystem::path out(p);
    CoTaskMemFree(p);
    return out;
}
std::filesystem::path appDir() {
    auto p = known(FOLDERID_LocalAppData) / L"NCO";
    std::filesystem::create_directories(p);
    return p;
}
std::filesystem::path videosDir() {
    return known(FOLDERID_Videos) / L"NCO";
}
std::filesystem::path uniquePath(const std::filesystem::path& dir, const std::string& prefix,
                                 const std::string& ext) {
    std::filesystem::create_directories(dir);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[96];
    swprintf_s(b, L"%04u-%02u-%02u_%02u-%02u-%02u-%03u", t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond, t.wMilliseconds);
    static std::atomic<uint64_t> serial = 0;
    std::wstring suffix = L"-" + std::to_wstring(++serial);
    auto p = dir / (wide(prefix) + b + suffix + L"." + wide(ext));
    for (int n = 1; std::filesystem::exists(p); ++n)
        p = dir / (wide(prefix) + b + L"-" + std::to_wstring(n) + L"." + wide(ext));
    return p;
}
std::wstring folderDialog(HWND owner, const std::wstring& initial) {
    Com<IFileOpenDialog> d;
    check(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)),
          "Folder dialog");
    d->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    Com<IShellItem> item;
    if (SUCCEEDED(SHCreateItemFromParsingName(initial.c_str(), nullptr, IID_PPV_ARGS(&item))))
        d->SetFolder(item.Get());
    if (FAILED(d->Show(owner)))
        return {};
    Com<IShellItem> r;
    check(d->GetResult(&r), "Selected folder");
    PWSTR p = nullptr;
    check(r->GetDisplayName(SIGDN_FILESYSPATH, &p), "Folder name");
    std::wstring s(p);
    CoTaskMemFree(p);
    return s;
}
std::wstring shortcutText(Shortcut s) {
    if (!s.key)
        return L"Not assigned";
    std::wstring r;
    if (s.mods & MOD_CONTROL)
        r += L"Ctrl + ";
    if (s.mods & MOD_ALT)
        r += L"Alt + ";
    if (s.mods & MOD_SHIFT)
        r += L"Shift + ";
    if (s.mods & MOD_WIN)
        r += L"Win + ";
    wchar_t b[80]{};
    UINT sc = MapVirtualKeyW(s.key, MAPVK_VK_TO_VSC);
    if (s.key == VK_LEFT || s.key == VK_RIGHT || s.key == VK_UP || s.key == VK_DOWN ||
        s.key == VK_DELETE || s.key == VK_INSERT || s.key == VK_HOME || s.key == VK_END ||
        s.key == VK_PRIOR || s.key == VK_NEXT)
        sc |= 0x100;
    GetKeyNameTextW(LONG(sc << 16), b, 80);
    return r + (b[0] ? b : L"Key " + std::to_wstring(s.key));
}
Log& Log::get() {
    static Log log;
    return log;
}
void Log::write(const std::string& s, bool detail) {
    if (detail && !verbose_)
        return;
    std::lock_guard lock(mutex_);
    try {
        auto path = appDir() / L"diagnostic.log";
        if (std::filesystem::exists(path) && std::filesystem::file_size(path) > 1024 * 1024) {
            std::ifstream old(path, std::ios::binary);
            old.seekg(-512 * 1024, std::ios::end);
            std::string tail((std::istreambuf_iterator<char>(old)), {});
            old.close();
            auto newline = tail.find('\n');
            if (newline != std::string::npos) tail.erase(0, newline + 1);
            std::ofstream truncate(path, std::ios::trunc);
            truncate << "NCO log rotated; recent history retained\n" << tail;
        }
        std::ofstream f(path, std::ios::app);
        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        char timestamp[40]{};
        snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                 utc.wYear, utc.wMonth, utc.wDay, utc.wHour, utc.wMinute, utc.wSecond, utc.wMilliseconds);
        f << timestamp << " t_us=" << clockUs() << " tid=" << GetCurrentThreadId() << " " << s << "\n";
    } catch (...) {
    }
}
std::string Log::snapshot() {
    std::lock_guard lock(mutex_);
    std::ifstream input(appDir() / L"diagnostic.log", std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}
std::vector<std::wstring> Hotkeys::apply(const std::vector<Shortcut>& keys) {
    std::vector<std::wstring> errors(keys.size());
    for (int i = 0; i < 5; ++i)
        UnregisterHotKey(hwnd_, 100 + i);
    for (size_t i = 0; i < keys.size(); ++i) {
        if (duplicate(keys, i, keys[i])) {
            errors[i] = L"Duplicate shortcut";
            continue;
        }
        if (keys[i].key &&
            !RegisterHotKey(hwnd_, 100 + int(i), keys[i].mods | MOD_NOREPEAT, keys[i].key))
            errors[i] = L"Reserved by Windows or another application";
    }
    return errors;
}
Hotkeys::~Hotkeys() {
    for (int i = 0; i < 5; ++i)
        UnregisterHotKey(hwnd_, 100 + i);
}
namespace {
struct RegionState {
    bool dragging = false, done = false, accepted = false;
    POINT start{}, end{};
};
LRESULT CALLBACK regionProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    auto s = reinterpret_cast<RegionState*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        s = static_cast<RegionState*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s)
        return DefWindowProcW(h, m, w, l);
    if (m == WM_LBUTTONDOWN) {
        s->start = {short(LOWORD(l)), short(HIWORD(l))};
        s->end = s->start;
        s->dragging = true;
        SetCapture(h);
        return 0;
    }
    if (m == WM_MOUSEMOVE && s->dragging) {
        s->end = {short(LOWORD(l)), short(HIWORD(l))};
        InvalidateRect(h, nullptr, TRUE);
        return 0;
    }
    if (m == WM_LBUTTONUP) {
        // Ignore the click that opened this modal selector; a drag must start here.
        if (!s->dragging) return 0;
        s->dragging = false;
        RECT bounds{};
        GetClientRect(h, &bounds);
        s->end = {short(LOWORD(l)), short(HIWORD(l))};
        s->accepted = abs(s->end.x - s->start.x) >= 64 && abs(s->end.y - s->start.y) >= 64;
        s->start.x = std::clamp(s->start.x, 0L, bounds.right);
        s->start.y = std::clamp(s->start.y, 0L, bounds.bottom);
        s->end.x = std::clamp(s->end.x, 0L, bounds.right);
        s->end.y = std::clamp(s->end.y, 0L, bounds.bottom);
        s->accepted = abs(s->end.x - s->start.x) >= 64 && abs(s->end.y - s->start.y) >= 64;
        s->done = s->accepted;
        ReleaseCapture();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    if (m == WM_CAPTURECHANGED) {
        s->dragging = false;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    if (m == WM_CLOSE) {
        s->done = true;
        s->accepted = false;
        return 0;
    }
    if (m == WM_KEYDOWN && w == VK_ESCAPE) {
        s->done = true;
        return 0;
    }
    if (m == WM_PAINT) {
        PAINTSTRUCT p;
        HDC dc = BeginPaint(h, &p);
        RECT r;
        GetClientRect(h, &r);
        HBRUSH b = CreateSolidBrush(RGB(35, 25, 18));
        FillRect(dc, &r, b);
        DeleteObject(b);
        SetTextColor(dc, RGB(245, 224, 202));
        SetBkMode(dc, TRANSPARENT);
        r.top += 24;
        DrawTextW(dc, L"Drag to select a capture region · Esc to cancel · minimum 64 × 64", -1, &r,
                  DT_CENTER | DT_TOP | DT_SINGLELINE);
        if (s->dragging) {
            HPEN pen = CreatePen(PS_SOLID, 3, RGB(240, 181, 120));
            auto old = SelectObject(dc, pen);
            auto brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
            Rectangle(dc, s->start.x, s->start.y, s->end.x, s->end.y);
            SelectObject(dc, brush);
            SelectObject(dc, old);
            DeleteObject(pen);
        }
        EndPaint(h, &p);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace
std::optional<RECT> selectRegion(HWND owner, HMONITOR monitor) {
    MONITORINFO mi{sizeof(mi)};
    check(GetMonitorInfoW(monitor, &mi) ? S_OK : E_FAIL, "Region display");
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = regionProc;
    wc.lpszClassName = L"NCORegion";
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    RegisterClassW(&wc);
    RegionState state;
    HWND h = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, L"NCORegion", L"NCO capture region",
        WS_POPUP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top, owner, nullptr, wc.hInstance, &state);
    if (!h)
        throw std::runtime_error("Cannot show region selector");
    SetLayeredWindowAttributes(h, 0, 160, LWA_ALPHA);
    SetWindowDisplayAffinity(h, WDA_EXCLUDEFROMCAPTURE);
    EnableWindow(owner, FALSE);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(h);
    MSG msg;
    while (!state.done && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DestroyWindow(h);
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (!state.accepted)
        return {};
    return RECT{std::min(state.start.x, state.end.x), std::min(state.start.y, state.end.y),
                std::max(state.start.x, state.end.x), std::max(state.start.y, state.end.y)};
}
void regionInputSelfTest() {
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = regionProc;
    wc.lpszClassName = L"NCORegionInputTest";
    RegisterClassW(&wc);
    RegionState state;
    HWND window = CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"", WS_POPUP,
                                  0, 0, 640, 480, nullptr, nullptr, wc.hInstance, &state);
    if (!window) throw std::runtime_error("Cannot create region input test");
    try {
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(260, 220));
        if (state.done || state.accepted)
            throw std::runtime_error("Region accepted a release without a drag");
        SendMessageW(window, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(100, 80));
        SendMessageW(window, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(420, 320));
        SendMessageW(window, WM_LBUTTONUP, 0, MAKELPARAM(420, 320));
        if (!state.done || !state.accepted || state.start.x != 100 || state.start.y != 80 ||
            state.end.x != 420 || state.end.y != 320)
            throw std::runtime_error("Region drag did not preserve the selected area");
    } catch (...) {
        DestroyWindow(window);
        throw;
    }
    DestroyWindow(window);
}
} // namespace nco
