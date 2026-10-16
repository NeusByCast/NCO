#pragma once
#include "engine.hpp"
#include "toast.hpp"
#include <d2d1.h>
#include <dwrite.h>
#include <map>
#include <tuple>
namespace nco {
class UI {
    struct Control {
        D2D1_RECT_F rect;
        std::wstring id;
        std::function<void()> click;
        bool enabled = true;
    };
    struct Popup {
        bool open = false;
        D2D1_RECT_F anchor{};
        std::vector<std::wstring> items;
        int selected = 0, scroll = 0;
        int64_t start = 0;
        std::function<void(int)> choose;
        bool closing = false;
        int64_t closeStart = 0;
        std::wstring owner;
        int visible = 7;
        bool positioned = false;
    };
    HWND hwnd_ = nullptr, toast_ = nullptr, edit_ = nullptr;
    HINSTANCE instance_;
    Config cfg_;
    FrameTimer previewTimer_;
    ToastSurface toastSurface_;
    POINT toastPosition_{};
    float toastDpi_ = 96;
    bool previewTimerActive_ = false, frameTimerArmed_ = false, messageTimerActive_ = false;
    bool inSizeMove_ = false, animationsAllowed_ = false;
    int64_t frameIntervalUs_ = 16667, lastUiFrame_ = 0;
    Preview displayedPreview_;
    std::unique_ptr<Engine> engine_;
    std::unique_ptr<Hotkeys> hotkeys_;
    Com<ID2D1Factory> factory_;
    Com<ID2D1HwndRenderTarget> target_;
    Com<IDWriteFactory> write_;
    Com<ID2D1SolidColorBrush> brush_;
    Com<ID2D1Bitmap> bitmap_;
    uint64_t bitmapSerial_ = 0;
    std::map<int, Com<IDWriteTextFormat>> fonts_;
    std::map<std::tuple<std::wstring, int, float, float>, Com<IDWriteTextLayout>> textLayouts_;
    std::vector<Control> controls_;
    Popup popup_;
    struct Tween {
        float from = 0, to = 0;
        int64_t start = 0;
    };
    std::map<std::wstring, Tween> tweens_;
    std::wstring pressed_;
    int64_t navigationStart_ = 0;
    HFONT editFont_ = nullptr;
    std::vector<Source> sources_;
    std::vector<AudioDevice> mics_, speakers_;
    std::vector<EncoderChoice> encoders_;
    std::jthread probe_;
    std::vector<std::wstring> keyErrors_;
    int sourceIndex_ = 0, page_ = 0, scroll_ = 0, maxScroll_ = 0, hotkeyIndex_ = -1, focus_ = -1,
        smokeStep_ = 0;
    bool scrollDragging_ = false, trackingMouse_ = false;
    float w_ = 1120, h_ = 760, dpi_ = 96;
    bool closing_ = false, smoke_ = false, probing_ = true, renderFailed_ = false;
    std::wstring hover_, message_;
    std::wstring hardware_;
    int64_t hoverStart_ = 0, toastStart_ = 0;
    bool toastVisible_ = false, exclusion_ = false;
    std::function<void(std::wstring)> editCommit_;
    EngineStats stats_;
    void draw();
    void updatePreviewTimer();
    void updateDisplayTiming();
    void requestAnimation();
    void advanceUiFrame();
    bool animationsEnabled() const;
    bool animationActiveAt(int64_t now) const;
    bool animationFrameNeedsPaint(int64_t now) const;
    void resources();
    void text(const std::wstring& s, D2D1_RECT_F rect, int size = 14,
              D2D1_COLOR_F color = D2D1::ColorF(0xece2d8), bool bold = false, bool center = false);
    void round(D2D1_RECT_F rect, D2D1_COLOR_F color, float radius = 10);
    void icon(const std::wstring& name, D2D1_RECT_F rect, D2D1_COLOR_F color);
    D2D1_COLOR_F hoverColor(const std::wstring& id, D2D1_COLOR_F base);
    void button(std::wstring id, std::wstring label, D2D1_RECT_F rect, std::function<void()> click,
                bool primary = false, bool enabled = true, std::wstring glyph = L"");
    void toggle(std::wstring id, std::wstring label, float y, bool value,
                std::function<void()> click, bool enabled = true, std::wstring hint = L"");
    void field(std::wstring id, std::wstring label, std::wstring value, float y,
               std::function<void(D2D1_RECT_F)> click, bool enabled = true);
    void dropdown(D2D1_RECT_F rect, std::vector<std::wstring> items, int selected,
                  std::function<void(int)> choose);
    void number(std::wstring title, int value, int lo, int hi, D2D1_RECT_F rect,
                std::function<void(int)> accept);
    void edit(std::wstring value, D2D1_RECT_F rect, std::function<void(std::wstring)> accept,
              bool numeric = false);
    void finishEdit(bool accept);
    void dashboard();
    void settings();
    void popup();
    void dismissPopup();
    void save(bool update = true);
    void refresh();
    void notify(std::string msg, bool error);
    void toast(std::wstring msg);
    void key(WPARAM key);
    void click(float x, float y);
    void scrollAt(float y);
    void invalidate();
    void command(int action);
    void shutdown();
    void smokeControls();
    static LRESULT CALLBACK proc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK toastProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK editProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    LRESULT message(HWND, UINT, WPARAM, LPARAM);

  public:
    explicit UI(HINSTANCE instance, bool smoke = false) : instance_(instance), smoke_(smoke) {}
    int run(int show);
};
} // namespace nco
