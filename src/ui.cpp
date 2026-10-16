#include "ui.hpp"
#include <cmath>
#include <commctrl.h>
#include <commdlg.h>
#include "diagnostics.hpp"
#include <dwmapi.h>
#include <fstream>
#include <iomanip>
#include <shellapi.h>
#include <sstream>
#include <windowsx.h>
namespace nco {
static const auto bg = D2D1::ColorF(0x19130f), panel = D2D1::ColorF(0x251d17),
                  surface = D2D1::ColorF(0x30251d), accent = D2D1::ColorF(0xc68c59),
                  ink = D2D1::ColorF(0xeee4da), muted = D2D1::ColorF(0xa69585),
                  danger = D2D1::ColorF(0xe09a86);
static bool inside(D2D1_RECT_F r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}
static float ease(float t) {
    t = std::clamp(t, 0.f, 1.f);
    return 1 - (1 - t) * (1 - t) * (1 - t);
}
static D2D1_COLOR_F blend(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1};
}
static void screenshot(HWND window, int page) {
    RECT r;
    GetClientRect(window, &r);
    POINT p{0, 0};
    ClientToScreen(window, &p);
    int width = r.right, height = r.bottom;
    if (width < 1 || height < 1)
        return;
    HDC screen = GetDC(nullptr), dc = CreateCompatibleDC(screen);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* data = nullptr;
    HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &data, nullptr, 0);
    auto old = SelectObject(dc, bitmap);
    if (data && BitBlt(dc, 0, 0, width, height, screen, p.x, p.y, SRCCOPY | CAPTUREBLT)) {
        std::filesystem::create_directories(L"nco-self-test");
        std::ofstream f(std::filesystem::path(L"nco-self-test") /
                            (L"ui-page-" + std::to_wstring(page) + L".bmp"),
                        std::ios::binary);
        BITMAPFILEHEADER header{};
        header.bfType = 0x4d42;
        header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
        header.bfSize = header.bfOffBits + width * height * 4;
        f.write(reinterpret_cast<const char*>(&header), sizeof(header));
        f.write(reinterpret_cast<const char*>(&info.bmiHeader), sizeof(info.bmiHeader));
        f.write(static_cast<const char*>(data), size_t(width) * height * 4);
    }
    SelectObject(dc, old);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}
int UI::run(int show) {
    cfg_ = Config::load();
    Log::get().write("NCO 0.4.0 starting");
    hardware_ = hardwareInfo();
    WNDCLASSEXW c{sizeof(c)};
    c.lpfnWndProc = proc;
    c.hInstance = instance_;
    c.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    c.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    c.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(1));
    c.hIconSm = c.hIcon;
    c.lpszClassName = L"NCOWindow";
    RegisterClassExW(&c);
    c.lpszClassName = L"NCOToast";
    c.lpfnWndProc = toastProc;
    RegisterClassExW(&c);
    hwnd_ =
        // NCO paints its own title bar and controls. Keep the resizable frame
        // and system menu for native sizing/snap behavior, but omit WS_CAPTION
        // so Windows does not paint a second set of caption buttons over ours.
        CreateWindowExW(0, L"NCOWindow", L"NCO",
                        WS_POPUP | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX |
                            WS_SYSMENU | WS_CLIPCHILDREN,
                        CW_USEDEFAULT, CW_USEDEFAULT, 1120, 760, nullptr, nullptr, instance_, this);
    if (!hwnd_)
        throw std::runtime_error("Cannot create NCO window");
    dpi_ = float(GetDpiForWindow(hwnd_));
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    int width = std::min(int(1120 * dpi_ / 96), int(mi.rcWork.right - mi.rcWork.left)),
        height = std::min(int(760 * dpi_ / 96), int(mi.rcWork.bottom - mi.rcWork.top));
    SetWindowPos(hwnd_, nullptr, mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - width) / 2,
                 mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - height) / 2, width, height,
                 SWP_NOZORDER);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd_, 20, &dark, sizeof(dark));
    COLORREF borderColor = RGB(25, 19, 15);
    DwmSetWindowAttribute(hwnd_, 34, &borderColor, sizeof(borderColor));
    DwmSetWindowAttribute(hwnd_, 35, &borderColor, sizeof(borderColor));
    DWORD corner = 2;
    DwmSetWindowAttribute(hwnd_, 33, &corner, sizeof(corner));
    hotkeys_ = std::make_unique<Hotkeys>(hwnd_);
    keyErrors_ = hotkeys_->apply(cfg_.keys);
    engine_ = std::make_unique<Engine>([this](std::string m, bool e) {
        auto data = new std::pair<std::string, bool>(std::move(m), e);
        if (!PostMessageW(hwnd_, WM_APP + 1, 0, reinterpret_cast<LPARAM>(data)))
            delete data;
    });
    refresh();
    save();
    toast_ =
        CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                        L"NCOToast", L"", WS_POPUP, 0, 0, 300, 50, nullptr, nullptr, instance_, this);
    exclusion_ = captureExclusionSupported() &&
                 SetWindowDisplayAffinity(toast_, WDA_EXCLUDEFROMCAPTURE) != FALSE;
    if (!exclusion_)
        Log::get().write("Capture exclusion unavailable; external notifications disabled");
    probe_ = std::jthread([this] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto result = new std::vector<EncoderChoice>(probeEncoders());
        if (!PostMessageW(hwnd_, WM_APP + 2, 0, reinterpret_cast<LPARAM>(result)))
            delete result;
        CoUninitialize();
    });
    resources();
    draw();
    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);
    SetTimer(hwnd_, 1, 1000, nullptr);
    if (smoke_)
        SetTimer(hwnd_, 3, 1200, nullptr);
    updatePreviewTimer();
    MSG msg{};
    bool running = true;
    while (running) {
        HANDLE timer = previewTimer_.get();
        DWORD ready = MsgWaitForMultipleObjectsEx(timer ? 1 : 0, timer ? &timer : nullptr,
                                                  INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (timer && ready == WAIT_OBJECT_0 && page_ == 0 && !IsIconic(hwnd_))
            invalidate();
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (probe_.joinable())
        probe_.join();
    return smoke_ && renderFailed_ ? 1 : int(msg.wParam);
}
void UI::updatePreviewTimer() {
    // Recording pauses the live preview, so the dashboard needs only its
    // one-second status refresh. Avoid repainting the whole Direct2D page at
    // 30 FPS while there is no preview bitmap to display.
    const bool active = page_ == 0 && IsWindowVisible(hwnd_) && !IsIconic(hwnd_) &&
                        !closing_ && !stats_.recording;
    if (active != previewTimerActive_) {
        previewTimerActive_ = active;
        if (engine_)
            engine_->visible(active);
        if (active)
            previewTimer_.arm(33333, 33);
        else
            previewTimer_.cancel();
    }
}
void UI::resources() {
    if (!factory_)
        check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()),
              "Direct2D factory");
    if (!write_)
        check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                  reinterpret_cast<IUnknown**>(write_.GetAddressOf())),
              "DirectWrite");
    if (!target_) {
        RECT rc;
        GetClientRect(hwnd_, &rc);
        check(factory_->CreateHwndRenderTarget(
                  D2D1::RenderTargetProperties(
                      D2D1_RENDER_TARGET_TYPE_DEFAULT,
                      D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN), dpi_, dpi_),
                  D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(rc.right, rc.bottom)),
                  &target_),
              "UI render target");
        check(target_->CreateSolidColorBrush(ink, &brush_), "UI brush");
        bitmap_.Reset();
        bitmapSerial_ = 0;
    }
}
void UI::text(std::wstring s, D2D1_RECT_F r, int size, D2D1_COLOR_F color, bool bold, bool center) {
    int id = size + (bold ? 1000 : 0) + (center ? 2000 : 0);
    if (!fonts_.count(id)) {
        Com<IDWriteTextFormat> f;
        check(write_->CreateTextFormat(
                  L"Segoe UI", nullptr,
                  bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                  DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, float(size), L"en-US", &f),
              "UI font");
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        if (center)
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        Com<IDWriteInlineObject> ellipsis;
        check(write_->CreateEllipsisTrimmingSign(f.Get(), &ellipsis), "Text trimming");
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        f->SetTrimming(&trim, ellipsis.Get());
        fonts_[id] = f;
    }
    brush_->SetColor(color);
    target_->DrawTextW(s.c_str(), UINT32(s.size()), fonts_[id].Get(), r, brush_.Get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void UI::round(D2D1_RECT_F r, D2D1_COLOR_F color, float radius) {
    brush_->SetColor(color);
    target_->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.Get());
}
void UI::icon(const std::wstring& name, D2D1_RECT_F r, D2D1_COLOR_F color) {
    brush_->SetColor(color);
    float x = (r.left + r.right) / 2, y = (r.top + r.bottom) / 2;
    auto line = [&](float a, float b, float c, float d) {
        target_->DrawLine({x + a, y + b}, {x + c, y + d}, brush_.Get(), 1.6f);
    };
    if (name == L"record") {
        target_->FillEllipse(D2D1::Ellipse({x, y}, 5, 5), brush_.Get());
    } else if (name == L"stop")
        round({x - 5, y - 5, x + 5, y + 5}, color, 2);
    else if (name == L"pause") {
        round({x - 5, y - 6, x - 2, y + 6}, color, 1);
        round({x + 2, y - 6, x + 5, y + 6}, color, 1);
    } else if (name == L"play") {
        line(-3, -6, 5, 0);
        line(5, 0, -3, 6);
        line(-3, 6, -3, -6);
    } else if (name == L"mic") {
        round({x - 3, y - 8, x + 3, y + 2}, color, 3);
        line(-6, -1, -6, 2);
        line(-6, 2, -3, 5);
        line(-3, 5, 3, 5);
        line(3, 5, 6, 2);
        line(6, 2, 6, -1);
        line(0, 5, 0, 9);
        line(-3, 9, 3, 9);
    } else if (name == L"speaker") {
        line(-7, -3, -3, -3);
        line(-3, -3, 2, -7);
        line(2, -7, 2, 7);
        line(2, 7, -3, 3);
        line(-3, 3, -7, 3);
        line(-7, 3, -7, -3);
        target_->DrawEllipse(D2D1::Ellipse({x + 2, y}, 7, 7), brush_.Get(), 1.4f);
    } else if (name == L"close") {
        line(-4, -4, 4, 4);
        line(-4, 4, 4, -4);
    } else if (name == L"min")
        line(-5, 0, 5, 0);
    else if (name == L"max")
        target_->DrawRectangle({x - 4, y - 4, x + 4, y + 4}, brush_.Get(), 1.2f);
    else if (name == L"chevron") {
        line(-4, -2, 0, 2);
        line(0, 2, 4, -2);
    } else if (name == L"replay") {
        target_->DrawEllipse(D2D1::Ellipse({x, y}, 7, 7), brush_.Get(), 1.5f);
        line(-7, -7, -7, -1);
        line(-7, -1, -1, -1);
        line(0, -4, 0, 0);
        line(0, 0, 3, 2);
    } else if (name == L"screen") {
        target_->DrawRoundedRectangle(D2D1::RoundedRect({x - 8, y - 6, x + 8, y + 4}, 2, 2),
                                      brush_.Get(), 1.5f);
        line(0, 4, 0, 8);
        line(-4, 8, 4, 8);
    } else if (name == L"settings") {
        target_->DrawEllipse(D2D1::Ellipse({x, y}, 6, 6), brush_.Get(), 1.6f);
        target_->FillEllipse(D2D1::Ellipse({x, y}, 2, 2), brush_.Get());
        for (int i = 0; i < 4; ++i) {
            float a = float(i) * 1.5707963f;
            line(cosf(a) * 6, sinf(a) * 6, cosf(a) * 9, sinf(a) * 9);
        }
    }
}
D2D1_COLOR_F UI::hoverColor(const std::wstring& id, D2D1_COLOR_F base) {
    const bool hover = id == hover_;
    auto [entry, created] = tweens_.try_emplace(L"hover/" + id, Tween{0, 0, 0});
    auto& tween = entry->second;
    if (tween.to != float(hover)) {
        float previous = ease(float(clockUs() - tween.start) / 150000);
        tween.from += (tween.to - tween.from) * previous;
        tween.to = float(hover);
        tween.start = clockUs();
        if (cfg_.animations)
            SetTimer(hwnd_, 2, 33, nullptr);
    }
    const float fraction = cfg_.animations ? ease(float(clockUs() - tween.start) / 150000) : 1;
    const float t = tween.from + (tween.to - tween.from) * fraction;
    return blend(base, ink, .08f * t);
}
void UI::button(std::wstring id, std::wstring label, D2D1_RECT_F r, std::function<void()> click,
                bool primary, bool enabled, std::wstring glyph) {
    D2D1_COLOR_F color = hoverColor(id, primary ? accent : surface);
    if (id == pressed_)
        color = blend(color, bg, .2f);
    if (!enabled)
        color = blend(panel, color, .4f);
    round(r, color, 9);
    auto fg = enabled ? (primary ? bg : ink) : muted;
    if (!glyph.empty())
        icon(glyph, {r.left + 7, r.top, r.left + 33, r.bottom}, fg);
    if (!label.empty())
        text(label, {r.left + (glyph.empty() ? 14.f : 40.f), r.top, r.right - 8, r.bottom}, 13, fg,
             true);
    controls_.push_back({r, std::move(id), std::move(click), enabled});
}
void UI::field(std::wstring id, std::wstring label, std::wstring value, float y,
               std::function<void(D2D1_RECT_F)> click, bool enabled) {
    float left = 206, right = w_ - 28, mid = left + (right - left) * .49f;
    round({left, y, right, y + 58}, hoverColor(id, panel), 10);
    text(label, {left + 16, y, mid - 12, y + 58}, 14, enabled ? ink : muted);
    D2D1_RECT_F r{mid, y + 10, right - 12, y + 48};
    round(r, surface, 9);
    controls_.push_back({{left, y, right, y + 58}, id, [r, click] { click(r); }, enabled});
    text(value, {r.left + 14, r.top, r.right - 34, r.bottom}, 13, enabled ? ink : muted, true);
    icon(L"chevron", {r.right - 30, r.top, r.right - 8, r.bottom}, muted);
}
void UI::toggle(std::wstring id, std::wstring label, float y, bool value,
                std::function<void()> click, bool enabled, std::wstring hint) {
    float left = 206, right = w_ - 28;
    round({left, y, right, y + 58}, hoverColor(id, panel));
    text(label, {left + 16, y + 4, right - 84, y + (hint.empty() ? 54 : 30)}, 14,
         enabled ? ink : muted);
    if (!hint.empty())
        text(hint, {left + 16, y + 30, right - 84, y + 52}, 11, muted);
    D2D1_RECT_F r{right - 62, y + 18, right - 20, y + 40};
    auto [it, inserted] = tweens_.try_emplace(id, Tween{float(value), float(value), 0});
    auto& t = it->second;
    if (t.to != float(value)) {
        float p = ease(float(clockUs() - t.start) / 150000);
        t.from = t.from + (t.to - t.from) * p;
        t.to = float(value);
        t.start = clockUs();
        if (cfg_.animations)
            SetTimer(hwnd_, 2, 33, nullptr);
    }
    float p = cfg_.animations ? ease(float(clockUs() - t.start) / 150000) : 1;
    float v = t.from + (t.to - t.from) * p;
    round(r, blend(surface, accent, v), 11);
    brush_->SetColor(blend(muted, bg, v));
    target_->FillEllipse(D2D1::Ellipse({r.left + 11 + 20 * v, y + 29}, 7, 7), brush_.Get());
    controls_.push_back({{left, y, right, y + 58}, std::move(id), std::move(click), enabled});
}
void UI::dropdown(D2D1_RECT_F rect, std::vector<std::wstring> items, int selected,
                  std::function<void(int)> choose) {
    finishEdit(true);
    popup_ = {true, rect, std::move(items), selected, 0, clockUs(), std::move(choose)};
    popup_.owner = pressed_;
    popup_.scroll = std::clamp(selected - 3, 0, std::max(0, int(popup_.items.size()) - popup_.visible));
    SetTimer(hwnd_, 2, 33, nullptr);
    invalidate();
}
void UI::edit(std::wstring value, D2D1_RECT_F r, std::function<void(std::wstring)> accept,
              bool numeric) {
    finishEdit(true);
    popup_.open = false;
    editCommit_ = std::move(accept);
    edit_ = CreateWindowExW(
        0, L"EDIT", value.c_str(),
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (numeric ? ES_NUMBER : 0),
        int((r.left + 8) * dpi_ / 96), int((r.top + 6) * dpi_ / 96),
        int((r.right - r.left - 16) * dpi_ / 96), int((r.bottom - r.top - 10) * dpi_ / 96), hwnd_,
        reinterpret_cast<HMENU>(500), instance_, nullptr);
    editFont_ = CreateFontW(-int(14 * dpi_ / 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(editFont_), TRUE);
    SetWindowSubclass(edit_, editProc, 1, reinterpret_cast<DWORD_PTR>(this));
    SendMessageW(edit_, EM_SETLIMITTEXT, 1024, 0);
    SetFocus(edit_);
    SendMessageW(edit_, EM_SETSEL, 0, -1);
}
void UI::finishEdit(bool accept) {
    if (!edit_)
        return;
    HWND e = edit_;
    edit_ = nullptr;
    wchar_t b[1025]{};
    GetWindowTextW(e, b, 1025);
    auto fn = std::move(editCommit_);
    DestroyWindow(e);
    if (editFont_) {
        DeleteObject(editFont_);
        editFont_ = nullptr;
    }
    if (accept && fn) {
        try {
            fn(b);
        } catch (const std::exception& ex) {
            notify(ex.what(), true);
        }
    }
    SetFocus(hwnd_);
    invalidate();
}
void UI::number(std::wstring, int value, int lo, int hi, D2D1_RECT_F r,
                std::function<void(int)> accept) {
    edit(
        std::to_wstring(value), r,
        [lo, hi, accept](std::wstring s) {
            size_t end = 0;
            int n = std::stoi(s, &end);
            if (end != s.size() || n < lo || n > hi)
                throw std::runtime_error("Enter a value from " + std::to_string(lo) + " to " +
                                         std::to_string(hi));
            accept(n);
        },
        lo >= 0);
}
void UI::save(bool update) {
    try {
        cfg_.save();
        Log::get().verbose(cfg_.verbose);
        if (update && engine_ && !sources_.empty()) {
            auto effective = cfg_;
            // Probe vendor sessions before starting an automatic replay pipeline.
            if (probing_)
                effective.replay = false;
            engine_->configure(
                effective, sources_[size_t(std::clamp(sourceIndex_, 0, int(sources_.size()) - 1))]);
        }
    } catch (const std::exception& e) {
        notify(e.what(), true);
    }
    invalidate();
}
void UI::refresh() {
    auto old = !sources_.empty() ? sources_[size_t(sourceIndex_)] : Source{};
    sources_ = sources(hwnd_);
    sourceIndex_ = 0;
    if (old.custom && std::any_of(sources_.begin(), sources_.end(), [&](const Source& source) {
            return source.monitor == old.monitor;
        }))
        sourceIndex_ = replaceRegion(sources_, old, old.region);
    for (size_t i = 0; i < sources_.size(); ++i)
        if (sources_[i].monitor == old.monitor && sources_[i].window == old.window &&
            sources_[i].custom == old.custom)
            sourceIndex_ = int(i);
    mics_ = audioDevices(true);
    speakers_ = audioDevices(false);
}
void UI::command(int a) {
    if (!engine_)
        return;
    if (a == 0 && !engine_->stats().recording && probing_) {
        message_ = L"Detecting compatible encoders; please wait…";
        invalidate();
        return;
    }
    switch (a) {
    case 0:
        engine_->recording();
        break;
    case 1:
        engine_->pause();
        break;
    case 2:
        engine_->replay();
        break;
    case 3:
        cfg_.mic = !cfg_.mic;
        save();
        break;
    case 4:
        cfg_.desktop = !cfg_.desktop;
        save();
        break;
    }
    invalidate();
}
void UI::dashboard() {
    float left = 206, right = w_ - 28, width = right - left;
    float y = 140 - float(scroll_);
    bool locked = stats_.recording;
    if (!sources_.empty() && sources_[size_t(sourceIndex_)].window) {
        wchar_t title[512]{};
        auto& selected = sources_[size_t(sourceIndex_)];
        if (IsWindow(selected.window) && GetWindowTextW(selected.window, title, 512))
            selected.name = std::wstring(L"Window · ") + title;
    }
    std::wstring source =
        sources_.empty() ? L"No capture source" : sources_[size_t(sourceIndex_)].name;
    text(L"CAPTURE SOURCE", {left, y, right, y + 22}, 11, muted, true);
    D2D1_RECT_F sr{left, y + 30, right - 198, y + 72};
    button(
        L"source", source, sr,
        [this, sr] {
            std::vector<std::wstring> names;
            for (auto& s : sources_) {
                if (s.window) {
                    wchar_t title[512]{};
                    if (GetWindowTextW(s.window, title, 512))
                        s.name = std::wstring(L"Window · ") + title;
                }
                names.push_back(s.name);
            }
            dropdown(sr, names, sourceIndex_, [this](int i) {
                sourceIndex_ = i;
                save();
            });
        },
        false, !locked && !sources_.empty(), L"screen");
    button(
        L"region", L"Region", {right - 186, y + 30, right - 96, y + 72},
        [this] {
            if (sources_.empty())
                return;
            auto s = sources_[size_t(sourceIndex_)];
            if (!s.monitor)
                throw std::runtime_error("Select a display before choosing a region");
            auto r = selectRegion(hwnd_, s.monitor);
            if (r) {
                sourceIndex_ = replaceRegion(sources_, s, *r);
                save();
            }
        },
        false, !locked && !sources_.empty() && sources_[size_t(sourceIndex_)].monitor);
    button(
        L"refresh", L"Refresh", {right - 84, y + 30, right, y + 72},
        [this] {
            refresh();
            save();
        },
        false, !locked);
    y += 88;
    const bool tight = h_ < 620;
    const float maximumPreview = std::max(50.f, h_ - (tight ? 416.f : 426.f));
    float ph =
        std::min(cfg_.compact ? 150.f : std::clamp(width * 9 / 16, 180.f, 320.f), maximumPreview);
    D2D1_RECT_F pr{left, y, right, y + ph};
    round(pr, D2D1::ColorF(0x100d0b), 12);
    // Preview readback performs an extra GPU scale + staging copy. Stop that
    // work during a recording so the game and encoder get the GPU budget.
    if (cfg_.preview && bitmap_ && !stats_.recording) {
        auto s = bitmap_->GetSize();
        float scale = std::min((width - 12) / s.width, (ph - 12) / s.height), dw = s.width * scale,
              dh = s.height * scale;
        target_->DrawBitmap(bitmap_.Get(),
                            {left + (width - dw) / 2, y + (ph - dh) / 2, left + (width + dw) / 2,
                             y + (ph + dh) / 2},
                            1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        text(stats_.recording ? L"Preview paused while recording to reduce GPU load"
                              : cfg_.preview ? L"Waiting for capture…" : L"Preview paused",
             {left + 24, y + ph / 2 - 20, right - 24, y + ph / 2 + 20}, 16, muted);
    }
    if (ph >= 100) {
        round({left + 12, y + 12, left + 100, y + 36}, panel, 7);
        text(stats_.recording ? (stats_.paused ? L"PAUSED" : L"RECORDING") : L"LIVE PREVIEW",
             {left + 22, y + 12, left + 98, y + 36}, 9, stats_.recording ? accent : muted, true);
    }
    y += ph + 16;
    round({left, y, right, y + 76}, panel);
    int64_t sec = stats_.durationUs / 1000000;
    wchar_t duration[32];
    swprintf_s(duration, L"%02lld:%02lld:%02lld", sec / 3600, (sec / 60) % 60, sec % 60);
    text(duration, {left + 16, y + 10, left + 160, y + 42}, 24, ink, true);
    text(stats_.recording ? (stats_.paused ? L"Recording paused" : L"Recording in progress")
                          : L"",
         {left + 16, y + 42, left + 190, y + 66}, 11, muted);
    button(
        L"record",
        stats_.finishing ? L"Finalizing…"
        : stats_.recording ? L"Stop recording"
        : probing_       ? L"Detecting encoders…"
                         : L"Start recording",
        {right - 380, y + 18, right - 216, y + 58}, [this] { command(0); }, true,
        stats_.ready && !stats_.finishing && (!probing_ || stats_.recording), stats_.recording ? L"stop" : L"record");
    button(
        L"pause", L"", {right - 204, y + 18, right - 160, y + 58}, [this] { command(1); }, false,
        stats_.recording && !stats_.finishing, stats_.paused ? L"play" : L"pause");
    button(
        L"replay", L"Save replay", {right - 148, y + 18, right - 12, y + 58},
        [this] { command(2); }, false, stats_.replay, L"replay");
    y += tight ? 80 : 90;
    button(
        L"mic", cfg_.mic ? L"Mic on" : L"Mic muted", {left, y, left + 126, y + 38},
        [this] { command(3); }, cfg_.mic, true, L"mic");
    button(
        L"desktop", cfg_.desktop ? L"Sound on" : L"Sound muted",
        {left + 138, y, left + 278, y + 38}, [this] { command(4); }, cfg_.desktop, true,
        L"speaker");
    maxScroll_ = std::max(0, int(y + scroll_ + 92 - h_));
    if (scroll_ > maxScroll_) {
        scroll_ = maxScroll_;
        invalidate();
    }
}
void UI::settings() {
    float y = 140 - float(scroll_);
    bool locked = stats_.recording;
    auto pick = [&](std::wstring id, std::wstring label, std::wstring value,
                    std::vector<std::wstring> items, int selected, std::function<void(int)> choose,
                    bool enabled = true) {
        field(
            id, label, value, y,
            [this, items, selected, choose](D2D1_RECT_F r) {
                dropdown(r, items, selected, choose);
            },
            enabled);
        y += 68;
    };
    auto num = [&](std::wstring id, std::wstring label, int value, int lo, int hi,
                   std::function<void(int)> fn, bool enabled = true) {
        field(
            id, label, std::to_wstring(value), y,
            [this, value, lo, hi, fn](D2D1_RECT_F r) {
                number(L"", value, lo, hi, r, [this, fn](int n) {
                    fn(n);
                    save();
                });
            },
            enabled);
        y += 68;
    };
    auto tog = [&](std::wstring id, std::wstring label, bool value, std::function<void()> fn,
                   bool enabled = true, std::wstring hint = L"") {
        toggle(
            id, label, y, value,
            [this, fn] {
                fn();
                save();
            },
            enabled, hint);
        y += 68;
    };
    auto directory = [&](bool replay) {
        field(
            replay ? L"replaydir" : L"recorddir", replay ? L"Replay folder" : L"Recording folder",
            replay ? cfg_.replayDirectory : cfg_.directory, y,
            [this, replay](D2D1_RECT_F) {
                auto s = folderDialog(hwnd_, replay ? cfg_.replayDirectory : cfg_.directory);
                if (!s.empty()) {
                    (replay ? cfg_.replayDirectory : cfg_.directory) = s;
                    save();
                }
            },
            !stats_.recording);
        y += 68;
    };
    if (page_ == 1) {
        pick(
            L"fps", L"Frame rate", std::to_wstring(cfg_.fps) + L" FPS",
            {L"15 FPS", L"30 FPS", L"60 FPS"},
            cfg_.fps == 15 ? 0 : cfg_.fps == 30 ? 1 : 2,
            [this](int i) {
                cfg_.fps = i == 0 ? 15 : i == 1 ? 30 : 60;
                save();
            },
            !locked);
        const std::vector<std::wstring> res = {L"Native", L"720p", L"1080p",
                                               L"1440p",  L"4K",   L"Custom"};
        pick(
            L"resolution", L"Resolution", res[cfg_.resolutionMode], res, cfg_.resolutionMode,
            [this](int i) {
                cfg_.resolutionMode = i;
                save();
            },
            !locked);
        if (cfg_.resolutionMode == 5) {
            num(
                L"width", L"Custom width", cfg_.width, 64, 7680,
                [this](int n) { cfg_.width = n & ~1; }, !locked);
            num(
                L"height", L"Custom height", cfg_.height, 64, 4320,
                [this](int n) { cfg_.height = n & ~1; }, !locked);
        }
        std::vector<std::wstring> q = {L"Low", L"Medium", L"High", L"Very High", L"Ultra"};
        pick(
            L"quality", L"Quality preset", q[cfg_.quality], q, cfg_.quality,
            [this](int i) {
                cfg_.quality = i;
                save();
            },
            !locked);
        num(
            L"bitrate", L"Bitrate · kbps", cfg_.bitrateKbps, 0, 150000,
            [this](int n) { cfg_.bitrateKbps = n; }, !locked);
        const std::vector<std::wstring> rateModes = {
            L"VBR · peak constrained", L"CQP · constant quality", L"CBR · constant bitrate"};
        pick(
            L"ratecontrol", L"Rate control", rateModes[size_t(cfg_.rateControl)], rateModes,
            cfg_.rateControl,
            [this](int i) {
                cfg_.rateControl = i;
                save();
            },
            !locked);
        std::vector<std::wstring> labels = {L"Auto · best compatible encoder"};
        std::vector<std::string> names = {"auto"};
        int selected = 0;
        for (auto& e : encoders_)
            if (containerCompatible(cfg_.format, e.name)) {
                if (e.name == cfg_.encoder)
                    selected = int(labels.size());
                labels.push_back(e.label);
                names.push_back(e.name);
            }
        pick(
            L"encoder", L"Encoder", probing_ ? L"Detecting hardware…" : labels[size_t(selected)],
            labels, selected,
            [this, names](int i) {
                cfg_.encoder = names[size_t(i)];
                save();
            },
            !locked && !probing_);
        std::vector<std::wstring> formats = {L"MP4", L"MKV", L"MOV", L"WebM · AV1 / VP9"};
        std::vector<std::string> ids = {"mp4", "mkv", "mov", "webm"};
        int fi = int(std::find(ids.begin(), ids.end(), cfg_.format) - ids.begin());
        pick(
            L"format", L"File container", formats[size_t(fi)], formats, fi,
            [this, ids](int i) {
                cfg_.format = ids[size_t(i)];
                if (!containerCompatible(cfg_.format, cfg_.encoder))
                    cfg_.encoder = "auto";
                save();
            },
            !locked);
        directory(false);
        tog(
            L"remux", L"Automatically remux recordings", cfg_.remux,
            [this] { cfg_.remux = !cfg_.remux; }, !locked,
            L"MKV recordings are already in the safe target; other formats remux after recording.");
        tog(
            L"replayenabled", L"Enable replay buffer", cfg_.replay,
            [this] { cfg_.replay = !cfg_.replay; }, true);
        std::vector<std::wstring> ds = {L"15 seconds", L"30 seconds", L"60 seconds"};
        pick(L"duration", L"Replay duration", std::to_wstring(cfg_.replaySeconds) + L" seconds", ds,
             cfg_.replaySeconds == 15   ? 0
             : cfg_.replaySeconds == 30 ? 1
                                        : 2,
             [this](int i) {
                 cfg_.replaySeconds = i == 0 ? 15 : i == 1 ? 30 : 60;
                 save();
             });
    } else if (page_ == 2) {
        auto device = [&](bool mic) {
            auto& all = mic ? mics_ : speakers_;
            std::vector<std::wstring> labels;
            int selected = 0;
            for (size_t i = 0; i < all.size(); ++i) {
                labels.push_back(all[i].name);
                if (all[i].id == (mic ? cfg_.micDevice : cfg_.desktopDevice))
                    selected = int(i);
            }
            pick(mic ? L"micdevice" : L"sounddevice",
                 mic ? L"Microphone device" : L"Desktop audio device", labels[size_t(selected)],
                 labels, selected, [this, mic, all](int i) {
                     (mic ? cfg_.micDevice : cfg_.desktopDevice) = all[size_t(i)].id;
                     save();
                 });
        };
        device(true);
        device(false);
        field(L"refreshdevices", L"Detect connected devices", L"Refresh audio devices", y,
              [this](D2D1_RECT_F) {
                  mics_ = audioDevices(true);
                  speakers_ = audioDevices(false);
                  engine_->refreshAudio();
                  invalidate();
              });
        y += 68;
        tog(L"mic", L"Microphone", cfg_.mic, [this] { cfg_.mic = !cfg_.mic; });
        num(L"micvolume", L"Microphone volume · %", cfg_.micVolume, 0, 100,
            [this](int n) { cfg_.micVolume = n; });
        num(L"micgain", L"Microphone gain · %", cfg_.micGain, 0, 300,
            [this](int n) { cfg_.micGain = n; });
        tog(L"desktop", L"Desktop audio", cfg_.desktop, [this] { cfg_.desktop = !cfg_.desktop; });
        num(L"desktopvolume", L"Desktop volume · %", cfg_.desktopVolume, 0, 100,
            [this](int n) { cfg_.desktopVolume = n; });
        tog(
            L"tracks", L"Separate microphone and desktop tracks", cfg_.separateTracks,
            [this] { cfg_.separateTracks = !cfg_.separateTracks; }, !locked);
        num(
            L"offset", L"Audio synchronization offset · ms", cfg_.audioOffsetMs, -500, 500,
            [this](int n) { cfg_.audioOffsetMs = n; }, !locked);
    } else if (page_ == 4) {
        const wchar_t* names[] = {L"Start / stop recording", L"Pause / resume", L"Save replay",
                                  L"Microphone mute", L"Desktop audio mute"};
        for (int i = 0; i < 5; ++i) {
            float row = y;
            round({206, y, w_ - 28, y + 58}, panel);
            text(names[i], {222, y, 206 + (w_ - 234) * .45f, y + 58}, 14, ink);
            button(L"key" + std::to_wstring(i),
                   hotkeyIndex_ == i ? L"Press a key…" : shortcutText(cfg_.keys[size_t(i)]),
                   {206 + (w_ - 234) * .47f, y + 10, w_ - 120, y + 48}, [this, i] {
                       hotkeyIndex_ = i;
                       hotkeys_->apply({});
                       SetFocus(hwnd_);
                       invalidate();
                   });
            button(L"clear" + std::to_wstring(i), L"Clear", {w_ - 108, row + 14, w_ - 42, row + 44},
                   [this, i] {
                       cfg_.keys[size_t(i)] = {0, 0};
                       keyErrors_ = hotkeys_->apply(cfg_.keys);
                       save(false);
                   });
            if (size_t(i) < keyErrors_.size() && !keyErrors_[size_t(i)].empty()) {
                text(keyErrors_[size_t(i)], {222, y + 62, w_ - 30, y + 88}, 11, danger);
                y += 32;
            }
            y += 68;
        }
    } else if (page_ == 6) {
        round({206, y, w_ - 28, y + 180}, panel);
        text(L"Hardware and encoding", {222, y + 12, w_ - 42, y + 42}, 16, ink, true);
        text(L"Adapters: " + hardware_, {222, y + 46, w_ - 42, y + 70}, 12, muted);
        std::wstring encoderName = L"Not recording";
        for (const auto& e : encoders_)
            if (e.name == stats_.encoder)
                encoderName = e.label;
        text(L"Encoder: " + encoderName,
             {222, y + 74, w_ - 42, y + 98}, 13, muted);
        text(L"Recorded frames: " + std::to_wstring(stats_.frames) + L"   Skipped frames: " +
                 std::to_wstring(stats_.dropped),
             {222, y + 102, w_ - 42, y + 126}, 13, muted);
        text(L"FPS: " + std::to_wstring(int(stats_.actualFps)) + L"   Output: " +
                 std::to_wstring(stats_.output.w) + L" × " + std::to_wstring(stats_.output.h) +
                 L"   Encoding: " + std::to_wstring(stats_.encodeMs).substr(0, 5) + L" ms",
             {222, y + 130, w_ - 42, y + 154}, 13, muted);
        y += 194;
        text(L"Process CPU: " + std::to_wstring(stats_.cpuPercent).substr(0, 5) +
                 L"%   Working set: " + std::to_wstring(int(stats_.workingSetMiB)) + L" MiB",
             {206, y, w_ - 28, y + 32}, 12, muted);
        y += 44;
        tog(
            L"logging", L"Verbose diagnostics", cfg_.verbose,
            [this] { cfg_.verbose = !cfg_.verbose; }, true);
        field(L"openlog", L"Diagnostic file", L"Open diagnostic log", y, [this](D2D1_RECT_F) {
            ShellExecuteW(hwnd_, L"open", (appDir() / L"diagnostic.log").c_str(), nullptr, nullptr,
                          SW_SHOWNORMAL);
        });
        y += 68;
        field(L"exportlog", L"Export diagnostics", L"Choose export folder", y, [this](D2D1_RECT_F) {
            auto folder = folderDialog(hwnd_, cfg_.directory);
            if (!folder.empty()) {
                exportDiagnostics(std::filesystem::path(folder) / L"NCO-diagnostic.txt",
                                  cfg_, engine_->stats(), dpi_, encoders_);
                toast(L"Diagnostics exported");
            }
        });
        y += 68;
        field(L"manualRemux", L"Remux a recording", L"Choose file… (original retained)", y,
              [this](D2D1_RECT_F) {
            wchar_t input[32768]{}, output[32768]{};
            OPENFILENAMEW dialog{};
            dialog.lStructSize = sizeof(dialog);
            dialog.hwndOwner = hwnd_;
            dialog.lpstrFilter = L"Recordings\0*.mkv;*.mp4;*.mov;*.webm\0All files\0*.*\0";
            dialog.lpstrFile = input;
            dialog.nMaxFile = DWORD(std::size(input));
            dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
            dialog.lpstrTitle = L"Choose recording to remux";
            if (!GetOpenFileNameW(&dialog)) return;
            std::filesystem::path source(input);
            auto suggested = source.parent_path() / (source.stem().wstring() + L"-remux.mp4");
            wcsncpy_s(output, suggested.c_str(), _TRUNCATE);
            dialog.lpstrFile = output;
            dialog.nMaxFile = DWORD(std::size(output));
            dialog.lpstrFilter = L"MP4\0*.mp4\0Matroska\0*.mkv\0MOV\0*.mov\0WebM\0*.webm\0";
            dialog.lpstrDefExt = L"mp4";
            dialog.lpstrTitle = L"Save remuxed recording (no re-encoding)";
            dialog.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_OVERWRITEPROMPT;
            if (!GetSaveFileNameW(&dialog)) return;
            const wchar_t* extensions[] = {L".mp4", L".mkv", L".mov", L".webm"};
            std::filesystem::path target(output);
            target.replace_extension(extensions[std::clamp(dialog.nFilterIndex, DWORD(1), DWORD(4)) - 1]);
            if (std::filesystem::exists(target))
                throw std::runtime_error("Choose a new filename; existing recordings are preserved");
            engine_->remux(source, target);
            message_ = L"Remux queued";
        });
        y += 68;
        const std::vector<std::wstring> apis = {L"Auto (recommended)", L"Windows Graphics Capture",
                                                L"Desktop Duplication"};
        pick(
            L"captureApi", L"Capture compatibility", apis[size_t(cfg_.captureBackend)], apis,
            cfg_.captureBackend,
            [this](int index) {
                cfg_.captureBackend = index;
                save();
            },
            !locked);

    }
    // Clamp after layout: avoids blank scroll regions after changing a section.
    maxScroll_ = std::max(0, int(y + scroll_ + 90 - h_));
    if (scroll_ > maxScroll_) {
        scroll_ = maxScroll_;
        invalidate();
    }
}
void UI::popup() {
    if (!popup_.open)
        return;
    auto& a = popup_;
    if (a.closing && clockUs() - a.closeStart >= 150000) {
        a.open = a.closing = false;
        return;
    }
    const float below = h_ - 44 - a.anchor.bottom;
    const float above = a.anchor.top - 124;
    const float fullHeight = std::min(7, int(a.items.size())) * 36.f + 12;
    const bool upward = below < fullHeight && above > below;
    const float available = std::max(48.f, upward ? above : below);
    int visible = std::max(1, std::min({7, int(a.items.size()), int((available - 12) / 36)}));
    a.visible = visible;
    if (!a.positioned) {
        a.scroll = std::clamp(a.selected - visible / 2, 0, std::max(0, int(a.items.size()) - visible));
        a.positioned = true;
    }
    a.scroll = std::clamp(a.scroll, 0, std::max(0, int(a.items.size()) - visible));
    float height = visible * 36.f + 12;
    float width = std::min(w_ - 24, std::max(180.f, a.anchor.right - a.anchor.left));
    float x = std::clamp(a.anchor.left, 12.f, w_ - width - 12);
    float y = upward ? a.anchor.top - height - 6 : a.anchor.bottom + 6;
    float progress = cfg_.animations ? ease(float(clockUs() - a.start) / 150000) : 1;
    if (a.closing)
        progress = 1 - ease(float(clockUs() - a.closeStart) / 150000);
    D2D1_RECT_F box{x, upward ? y + height * (1 - progress) : y,
                      x + width, upward ? y + height : y + height * progress};
    round(box, surface, 10);
    target_->PushAxisAlignedClip(box, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (int j = 0; j < visible; ++j) {
        int i = j + a.scroll;
        if (i >= int(a.items.size()))
            break;
        D2D1_RECT_F r{x + 6, y + 6 + j * 36, x + width - 6, y + 42 + j * 36};
        button(
            L"option" + std::to_wstring(i), a.items[size_t(i)], r,
            [this, i] {
                auto choose = popup_.choose;
                dismissPopup();
                choose(i);
                invalidate();
            },
            i == a.selected, !a.closing);
        controls_.back().rect.top = std::max(r.top, box.top);
        controls_.back().rect.bottom = std::min(r.bottom, box.bottom);
        if (controls_.back().rect.bottom <= controls_.back().rect.top)
            controls_.back().enabled = false;
    }
    if (int(a.items.size()) > visible) {
        float track = height - 16;
        float thumb = std::max(18.f, track * visible / int(a.items.size()));
        float top = y + 8 + (track - thumb) * a.scroll / (int(a.items.size()) - visible);
        round({x + width - 5, top, x + width - 2, top + thumb}, muted, 1.5f);
    }
    target_->PopAxisAlignedClip();
}
void UI::dismissPopup() {
    if (!popup_.open)
        return;
    if (!cfg_.animations)
        popup_.open = false;
    else {
        popup_.closing = true;
        popup_.closeStart = clockUs();
        SetTimer(hwnd_, 2, 33, nullptr);
    }
    invalidate();
}
void UI::draw() {
    resources();
    RECT client;
    GetClientRect(hwnd_, &client);
    w_ = client.right * 96 / dpi_;
    h_ = client.bottom * 96 / dpi_;
    stats_ = engine_ ? engine_->stats() : EngineStats{};
    updatePreviewTimer();
    if (engine_ && cfg_.preview) {
        auto p = engine_->preview(bitmapSerial_);
        if (p.serial && p.serial != bitmapSerial_) {
            if (p.pixels.empty()) bitmap_.Reset();
            else if (!bitmap_ || int(bitmap_->GetPixelSize().width) != p.size.w ||
                int(bitmap_->GetPixelSize().height) != p.size.h) {
                bitmap_.Reset();
                check(target_->CreateBitmap(D2D1::SizeU(p.size.w, p.size.h), p.pixels.data(),
                                            UINT32(p.size.w * 4),
                                            D2D1::BitmapProperties(D2D1::PixelFormat(
                                                DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE)),
                                            &bitmap_), "Preview bitmap");
            } else
                check(bitmap_->CopyFromMemory(nullptr, p.pixels.data(), UINT32(p.size.w * 4)),
                      "Update preview bitmap");
            bitmapSerial_ = p.serial;
        }
    }
    target_->BeginDraw();
    target_->Clear(bg);
    controls_.clear();
    round({0, 0, 178, h_}, D2D1::ColorF(0x211913), 0);
    text(L"NCO", {12, 20, 166, 64}, 28, ink, true, true);
    const wchar_t* titles[] = {L"Dashboard", L"Recording",  L"Audio",   L"Replay",
                               L"Hotkeys",   L"Appearance", L"Settings"};
    const int pages[] = {0, 1, 2, 4, 6};
    for (int n = 0; n < 5; ++n) {
        const int i = pages[n];
        float y = 100 + n * 48.f;
        if (page_ == i)
            round({12, y, 166, y + 40}, surface, 8);
        button(
            L"nav" + std::to_wstring(i), titles[i], {12, y, 166, y + 40},
            [this, i] {
                finishEdit(true);
                page_ = i;
                scroll_ = 0;
                popup_.open = false;
                navigationStart_ = clockUs();
                if (cfg_.animations)
                    SetTimer(hwnd_, 2, 33, nullptr);
                invalidate();
            },
            page_ == i, true,
            i == 0   ? L"screen"
            : i == 3 ? L"replay"
                     : L"settings");
    }
    button(
        L"min", L"", {w_ - 138, 8, w_ - 96, 42}, [this] { ShowWindow(hwnd_, SW_MINIMIZE); }, false,
        true, L"min");
    button(
        L"max", L"", {w_ - 94, 8, w_ - 52, 42},
        [this] { ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE); }, false, true,
        L"max");
    button(
        L"close", L"", {w_ - 50, 8, w_ - 8, 42}, [this] { SendMessageW(hwnd_, WM_CLOSE, 0, 0); },
        false, true, L"close");
    text(titles[page_], {206, 62, w_ - 28, 100}, 27, ink, true);
    float fade = cfg_.animations ? ease(float(clockUs() - navigationStart_) / 150000) : 1;
    target_->PushLayer(D2D1::LayerParameters({180, 136, w_, h_ - 38}, nullptr,
                                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                             D2D1::IdentityMatrix(), fade),
                       nullptr);
    if (page_ == 0)
        dashboard();
    else
        settings();
    target_->PopLayer();
    if (maxScroll_ > 0) {
        const float track = h_ - 190;
        const float thumb = std::max(24.f, track * track / (track + maxScroll_));
        const float top = 140 + (track - thumb) * scroll_ / maxScroll_;
        round({w_ - 15, top, w_ - 11, top + thumb}, scrollDragging_ ? accent : muted, 2);
    }
    round({180, h_ - 36, w_, h_}, bg, 0);
    text(message_.empty() ? (stats_.recording ? L"● Recording"
                             : L"")
                          : message_,
         {206, h_ - 34, w_ - 26, h_ - 3}, 11, message_.empty() ? muted : danger);
    popup();
    if (focus_ >= 0 && size_t(focus_) < controls_.size()) {
        brush_->SetColor(accent);
        target_->DrawRoundedRectangle(D2D1::RoundedRect(controls_[size_t(focus_)].rect, 9, 9),
                                      brush_.Get(), 1.2f);
    }
    HRESULT hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        target_.Reset();
        brush_.Reset();
        bitmap_.Reset();
    } else
        check(hr, "Render interface");
}
void UI::invalidate() {
    if (hwnd_ && !IsIconic(hwnd_))
        InvalidateRect(hwnd_, nullptr, FALSE);
}
void UI::scrollAt(float y) {
    const float track = h_ - 190;
    const float thumb = std::max(24.f, track * track / (track + maxScroll_));
    scroll_ = std::clamp(int((y - 140 - thumb / 2) / std::max(1.f, track - thumb) * maxScroll_), 0,
                         maxScroll_);
    invalidate();
}
void UI::click(float x, float y) {
    if (edit_)
        finishEdit(true);
    for (auto i = controls_.rbegin(); i != controls_.rend(); ++i) {
        if (inside(i->rect, x, y) && i->enabled) {
            bool chrome = i->id.rfind(L"nav", 0) == 0 || i->id == L"min" || i->id == L"max" ||
                          i->id == L"close";
            if (!popup_.open && !chrome && (y < 136 || y > h_ - 38))
                continue;
            if (popup_.open && !popup_.closing && popup_.owner == i->id) {
                dismissPopup();
                return;
            }
            pressed_ = i->id;
            if (popup_.open && i->id.rfind(L"option", 0) != 0)
                dismissPopup();
            auto fn = i->click;
            fn();
            invalidate();
            return;
        }
    }
    dismissPopup();
    invalidate();
}
void UI::key(WPARAM key) {
    if (hotkeyIndex_ >= 0) {
        if (key == VK_ESCAPE) {
            hotkeyIndex_ = -1;
            keyErrors_ = hotkeys_->apply(cfg_.keys);
            invalidate();
            return;
        }
        if (key == VK_CONTROL || key == VK_SHIFT || key == VK_MENU || key == VK_LWIN ||
            key == VK_RWIN)
            return;
        Shortcut s{unsigned(key), 0};
        if (GetKeyState(VK_CONTROL) & 0x8000)
            s.mods |= MOD_CONTROL;
        if (GetKeyState(VK_SHIFT) & 0x8000)
            s.mods |= MOD_SHIFT;
        if (GetKeyState(VK_MENU) & 0x8000)
            s.mods |= MOD_ALT;
        if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000)
            s.mods |= MOD_WIN;
        cfg_.keys[size_t(hotkeyIndex_)] = s;
        hotkeyIndex_ = -1;
        keyErrors_ = hotkeys_->apply(cfg_.keys);
        save(false);
        return;
    }
    if (popup_.open && !popup_.closing && (key == VK_UP || key == VK_DOWN || key == VK_RETURN)) {
        if (key == VK_RETURN) {
            auto choose = popup_.choose;
            int selected = popup_.selected;
            dismissPopup();
            choose(selected);
        } else {
            popup_.selected = std::clamp(popup_.selected + (key == VK_DOWN ? 1 : -1),
                                         0, int(popup_.items.size()) - 1);
            popup_.scroll = std::clamp(popup_.selected - popup_.visible / 2, 0,
                                        std::max(0, int(popup_.items.size()) - popup_.visible));
        }
        invalidate();
        return;
    }
    if (key == VK_ESCAPE) {
        dismissPopup();
        finishEdit(false);
        invalidate();
    } else if (key == VK_TAB) {
        focus_ = (focus_ + 1) % std::max(1, int(controls_.size()));
        invalidate();
    } else if ((key == VK_RETURN || key == VK_SPACE) && focus_ >= 0 &&
               size_t(focus_) < controls_.size()) {
        auto c = controls_[size_t(focus_)];
        if (c.enabled)
            c.click();
        invalidate();
    }
}
void UI::notify(std::string msg, bool error) {
    if (error) {
        message_ = wide(msg);
        Log::get().write(msg);
        if (msg.find("D3D11 device") != std::string::npos)
            message_ = L"Capture unavailable in this graphics session. Try a local desktop with "
                       L"updated GPU drivers.";
        else if (msg.rfind("Microphone unavailable:", 0) == 0)
            message_ = L"Microphone unavailable. Reconnect it or select another device in Audio.";
        else if (msg.rfind("Desktop audio unavailable:", 0) == 0)
            message_ = L"Desktop audio unavailable. Refresh or select an output device in Audio.";
        if (msg.rfind("Preview disabled:", 0) == 0) {
            cfg_.preview = false;
            save(false);
        }
        if (msg.rfind("Pipeline stopped:", 0) == 0) {
            cfg_.replay = false;
            save(false);
        }
    } else {
        message_.clear();
        toast(wide(msg));
    }
    invalidate();
}
bool UI::animationActiveAt(int64_t now) const {
    const bool tweenActive = std::any_of(tweens_.begin(), tweens_.end(), [now](const auto& item) {
        return now - item.second.start < 180000;
    });
    return tweenActive || popup_.closing || (popup_.open && now - popup_.start <= 180000) ||
           now - navigationStart_ <= 180000 || now - hoverStart_ <= 180000;
}
bool UI::animationFrameNeedsPaint(int64_t now) const {
    return animationActiveAt(now) && !previewTimerActive_ && hwnd_ && !IsIconic(hwnd_) &&
           IsWindowVisible(hwnd_);
}
void UI::toast(std::wstring msg) {
    if (!exclusion_) {
        message_ = std::move(msg);
        invalidate();
        return;
    }
    SetWindowTextW(toast_, msg.c_str());
    toastDpi_ = dpi_;
    toastSurface_.prepare(msg, toastDpi_);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &monitor);
    toastPosition_ = {monitor.rcWork.right - int(292 * toastDpi_ / 96),
                      monitor.rcWork.top + int(12 * toastDpi_ / 96)};
    toastStart_ = clockUs();
    toastVisible_ = true;
    SetTimer(hwnd_, 2, 33, nullptr);
}
LRESULT CALLBACK UI::toastProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_ERASEBKGND) return 1;
    if (m == WM_PAINT) {
        PAINTSTRUCT paint;
        BeginPaint(h, &paint);
        EndPaint(h, &paint);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
void UI::shutdown() {
    if (closing_)
        return;
    closing_ = true;
    EnableWindow(hwnd_, FALSE);
    message_ = L"Finalizing recording…";
    invalidate();
    std::thread([this] {
        engine_->stop();
        PostMessageW(hwnd_, WM_APP + 3, 0, 0);
    }).detach();
}
LRESULT CALLBACK UI::editProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR data) {
    auto self = reinterpret_cast<UI*>(data);
    if (m == WM_SYSCHAR)
        return 0;
    if (m == WM_KEYDOWN && (w == VK_RETURN || w == VK_ESCAPE || w == VK_TAB)) {
        self->finishEdit(w != VK_ESCAPE);
        return 0;
    }
    if (m == WM_NCDESTROY)
        RemoveWindowSubclass(h, editProc, id);
    return DefSubclassProc(h, m, w, l);
}
LRESULT CALLBACK UI::proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    UI* self = reinterpret_cast<UI*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = static_cast<UI*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = h;
    }
    if (self) {
        try {
            return self->message(h, m, w, l);
        } catch (const std::exception& e) {
            Log::get().write(e.what());
            self->message_ = wide(e.what());
            return 0;
        }
    }
    return DefWindowProcW(h, m, w, l);
}
LRESULT UI::message(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_NCCALCSIZE:
        if (w)
            return 0;
        break;
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(l), GET_Y_LPARAM(l)};
        ScreenToClient(h, &p);
        RECT r;
        GetClientRect(h, &r);
        int border = IsZoomed(h) ? 0 : int(7 * dpi_ / 96);
        if (p.y < border)
            return p.x < border ? HTTOPLEFT : p.x >= r.right - border ? HTTOPRIGHT : HTTOP;
        if (p.y >= r.bottom - border)
            return p.x < border ? HTBOTTOMLEFT : p.x >= r.right - border ? HTBOTTOMRIGHT : HTBOTTOM;
        if (p.x < border)
            return HTLEFT;
        if (p.x >= r.right - border)
            return HTRIGHT;
        if (p.y >= int(8 * dpi_ / 96) && p.y < int(42 * dpi_ / 96) &&
            p.x >= int((w_ - 94) * dpi_ / 96) && p.x < int((w_ - 52) * dpi_ / 96))
            return HTMAXBUTTON;
        if (p.y < int(52 * dpi_ / 96) && p.x < int((w_ - 148) * dpi_ / 96))
            return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCLBUTTONDOWN:
        if (w == HTMAXBUTTON)
            return 0;
        break;
    case WM_NCLBUTTONUP:
        if (w == HTMAXBUTTON) {
            ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
            return 0;
        }
        break;
    case WM_GETMINMAXINFO: {
        auto info = reinterpret_cast<MINMAXINFO*>(l);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
        info->ptMinTrackSize = {std::min(LONG(920 * dpi_ / 96), mi.rcWork.right - mi.rcWork.left),
                                std::min(LONG(540 * dpi_ / 96), mi.rcWork.bottom - mi.rcWork.top)};
        info->ptMaxPosition = {mi.rcWork.left - mi.rcMonitor.left,
                               mi.rcWork.top - mi.rcMonitor.top};
        info->ptMaxSize = {mi.rcWork.right - mi.rcWork.left, mi.rcWork.bottom - mi.rcWork.top};
        return 0;
    }
    case WM_ENTERSIZEMOVE:
        // Moving the window does not change its contents. Avoid forcing a full
        // synchronous Direct2D redraw during every step of the system move loop.
        return 0;
    case WM_EXITSIZEMOVE:
        invalidate();
        return 0;
    case WM_SIZE:
        updatePreviewTimer();
        if (engine_)
            engine_->visible(w != SIZE_MINIMIZED && page_ == 0);
        if (target_ && w != SIZE_MINIMIZED)
            target_->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
        if (w == SIZE_MINIMIZED) {
            popup_.open = popup_.closing = false;
            KillTimer(h, 1);
            // A toast must finish its lifetime even when the main window minimizes.
            if (!toastVisible_)
                KillTimer(h, 2);
        } else {
            SetTimer(h, 1, 1000, nullptr);
            invalidate();
        }
        return 0;
    case WM_DPICHANGED: {
        finishEdit(true);
        dpi_ = float(HIWORD(w));
        if (target_)
            target_->SetDpi(dpi_, dpi_);
        RECT* r = reinterpret_cast<RECT*>(l);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT p;
        BeginPaint(h, &p);
        try {
            draw();
        } catch (...) {
            EndPaint(h, &p);
            renderFailed_ = true;
            throw;
        }
        EndPaint(h, &p);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSELEAVE:
        hover_.clear();
        hoverStart_ = clockUs();
        SetTimer(h, 2, 33, nullptr);
        invalidate();
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, h, 0};
        TrackMouseEvent(&track);
        float x = GET_X_LPARAM(l) * 96 / dpi_, y = GET_Y_LPARAM(l) * 96 / dpi_;
        if (scrollDragging_) {
            scrollAt(y);
            return 0;
        }
        std::wstring id;
        for (auto i = controls_.rbegin(); i != controls_.rend(); ++i)
            if (i->enabled && inside(i->rect, x, y)) {
                id = i->id;
                break;
            }
        if (id != hover_) {
            hover_ = id;
            hoverStart_ = clockUs();
            if (cfg_.animations)
                SetTimer(h, 2, 33, nullptr);
            invalidate();
        }
        return 0;
    }
    case WM_CAPTURECHANGED:
        scrollDragging_ = false;
        break;
    case WM_LBUTTONUP: {
        const bool scrolling = scrollDragging_;
        scrollDragging_ = false;
        const auto pressed = pressed_;
        if (GetCapture() == h) ReleaseCapture();
        const float x = GET_X_LPARAM(l) * 96 / dpi_, y = GET_Y_LPARAM(l) * 96 / dpi_;
        if (!scrolling && !pressed.empty()) {
            auto hit = std::find_if(controls_.rbegin(), controls_.rend(), [&](const auto& control) {
                return control.enabled && inside(control.rect, x, y);
            });
            if (hit != controls_.rend() && hit->id == pressed) click(x, y);
        }
        pressed_.clear();
        invalidate();
        return 0;
    }
    case WM_LBUTTONDOWN:
        SetFocus(h);
        if (maxScroll_ > 0 && GET_X_LPARAM(l) * 96 / dpi_ > w_ - 22 &&
            GET_Y_LPARAM(l) * 96 / dpi_ >= 136 && GET_Y_LPARAM(l) * 96 / dpi_ < h_ - 38) {
            scrollDragging_ = true;
            SetCapture(h);
            scrollAt(GET_Y_LPARAM(l) * 96 / dpi_);
            return 0;
        }
        pressed_.clear();
        for (auto it = controls_.rbegin(); it != controls_.rend(); ++it)
            if (it->enabled && inside(it->rect, GET_X_LPARAM(l) * 96 / dpi_, GET_Y_LPARAM(l) * 96 / dpi_)) {
                pressed_ = it->id;
                break;
            }
        if (pressed_.empty()) dismissPopup();
        else SetCapture(h);
        invalidate();
        return 0;
    case WM_MOUSEWHEEL:
        if (popup_.open) {
            popup_.scroll = std::clamp(popup_.scroll - GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA, 0,
                                       std::max(0, int(popup_.items.size()) - popup_.visible));
        } else
            scroll_ =
                std::clamp(scroll_ - GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA * 56, 0, maxScroll_);
        invalidate();
        return 0;
    case WM_KEYDOWN:
        key(w);
        return 0;
    case WM_SYSKEYDOWN:
        if (hotkeyIndex_ >= 0) {
            key(w);
            return 0;
        }
        if (w == VK_F4)
            break;
        return 0;
    case WM_SYSCHAR:
        return 0;
    case WM_HOTKEY:
        command(int(w) - 100);
        return 0;
    case WM_COMMAND:
        if (LOWORD(w) == 500 && HIWORD(w) == EN_KILLFOCUS && edit_)
            finishEdit(true);
        return 0;
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(w);
        SetTextColor(dc, RGB(238, 228, 218));
        SetBkColor(dc, RGB(48, 37, 29));
        static HBRUSH b = CreateSolidBrush(RGB(48, 37, 29));
        return reinterpret_cast<LRESULT>(b);
    }
    case WM_TIMER:
        if (w == 3) {
            screenshot(h, smokeStep_);
            if (++smokeStep_ >= 6) {
                KillTimer(h, 3);
                smokeControls();
                shutdown();
            } else {
                const int smokePages[] = {0, 1, 2, 4, 6, 0};
                page_ = smokePages[smokeStep_];
                scroll_ = 0;
                if (smokeStep_ == 5)
                    SetWindowPos(h, nullptr, 0, 0, int(920 * dpi_ / 96), int(540 * dpi_ / 96),
                                 SWP_NOMOVE | SWP_NOZORDER);
                invalidate();
            }
            return 0;
        }
        if (w == 1) {
            if (!IsIconic(h) && (page_ == 6 || page_ == 0))
                invalidate();
        } else if (w == 2) {
            int64_t now = clockUs();
            if (toastVisible_ && toast_) {
                float t = float(now - toastStart_) / 1000;
                float f = t < 150 ? ease(t / 150) : t > 850 ? 1 - ease((t - 850) / 150) : 1;
                if (t >= 1000) {
                    ShowWindow(toast_, SW_HIDE);
                    toastVisible_ = false;
                } else {
                    const int x = toastPosition_.x + int(12 * (1 - f) * toastDpi_ / 96);
                    const int y = toastPosition_.y;
                    toastSurface_.frame(toast_, x, y, BYTE(255 * f));
                    if (!IsWindowVisible(toast_))
                        ShowWindow(toast_, SW_SHOWNOACTIVATE);
                }
            }
            const bool activeAnimation = animationActiveAt(now);
            // The dashboard preview timer owns its paint cadence. Do not make a toast
            // or other animation trigger a second full-window draw on the same UI thread.
            if (animationFrameNeedsPaint(now))
                invalidate();
            if (!toastVisible_ && !activeAnimation)
                KillTimer(h, 2);
        }
        return 0;
    case WM_APP + 1: {
        std::unique_ptr<std::pair<std::string, bool>> p(
            reinterpret_cast<std::pair<std::string, bool>*>(l));
        notify(p->first, p->second);
        return 0;
    }
    case WM_APP + 2: {
        std::unique_ptr<std::vector<EncoderChoice>> p(
            reinterpret_cast<std::vector<EncoderChoice>*>(l));
        encoders_ = std::move(*p);
        probing_ = false;
        if (cfg_.encoder != "auto" &&
            (!containerCompatible(cfg_.format, cfg_.encoder) ||
             std::none_of(encoders_.begin(), encoders_.end(),
                          [this](const auto& encoder) { return encoder.name == cfg_.encoder; }))) {
            Log::get().write("Saved encoder is unavailable; selecting Auto");
            cfg_.encoder = "auto";
        }
        save();
        return 0;
    }
    case WM_APP + 3:
        DestroyWindow(h);
        return 0;
    case WM_QUERYENDSESSION:
        shutdown();
        return FALSE;
    case WM_ENDSESSION:
        if (w && engine_)
            engine_->stop();
        return 0;
    case WM_CLOSE:
        finishEdit(true);
        shutdown();
        return 0;
    case WM_DESTROY:
        hotkeys_.reset();
        if (toast_)
            DestroyWindow(toast_);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}
} // namespace nco
