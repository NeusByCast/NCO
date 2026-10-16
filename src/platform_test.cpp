#include "platform.hpp"
#include "capture.hpp"
#include "toast.hpp"
#include <ostream>
#include <d3d11.h>
#include <wrl/implements.h>

namespace nco {
namespace {
// Real Capture::frame calls against a deterministic duplication interface.
class TestDuplication final : public Microsoft::WRL::RuntimeClass<
    Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDXGIOutputDuplication> {
  public:
    Com<IDXGIResource> resource;
    bool held = false, timeout = false, accessLost = false;
    int acquired = 0, released = 0;
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override { return E_NOINTERFACE; }
    void STDMETHODCALLTYPE GetDesc(DXGI_OUTDUPL_DESC* desc) override { *desc = {}; }
    HRESULT STDMETHODCALLTYPE AcquireNextFrame(UINT wait, DXGI_OUTDUPL_FRAME_INFO* info, IDXGIResource** out) override {
        if (held || wait != 0) return DXGI_ERROR_INVALID_CALL;
        if (timeout) return DXGI_ERROR_WAIT_TIMEOUT;
        *info = {};
        info->AccumulatedFrames = 1;
        held = true;
        ++acquired;
        return resource.CopyTo(out);
    }
    HRESULT STDMETHODCALLTYPE GetFrameDirtyRects(UINT, RECT*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFrameMoveRects(UINT, DXGI_OUTDUPL_MOVE_RECT*, UINT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetFramePointerShape(UINT, void*, UINT*, DXGI_OUTDUPL_POINTER_SHAPE_INFO*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE MapDesktopSurface(DXGI_MAPPED_RECT*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE UnMapDesktopSurface() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE ReleaseFrame() override {
        if (!held) return DXGI_ERROR_INVALID_CALL;
        held = false;
        ++released;
        return accessLost ? DXGI_ERROR_ACCESS_LOST : S_OK;
    }
};
struct KeyEvents {
    int single = 0, combination = 0;
};
LRESULT CALLBACK testProc(HWND h, UINT message, WPARAM w, LPARAM l) {
    auto state = reinterpret_cast<KeyEvents*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<KeyEvents*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (message == WM_HOTKEY && state) {
        if (w == 100)
            ++state->single;
        if (w == 101)
            ++state->combination;
        return 0;
    }
    return DefWindowProcW(h, message, w, l);
}
bool pumpUntil(const std::function<bool()>& done) {
    const auto until = GetTickCount64() + 1000;
    while (!done() && GetTickCount64() < until) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return done();
}
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(message);
}
void releaseTestKeys() {
    INPUT keys[3]{};
    const WORD codes[] = {VK_F7, VK_SHIFT, VK_CONTROL};
    for (int i = 0; i < 3; ++i) {
        keys[i].type = INPUT_KEYBOARD;
        keys[i].ki.wVk = codes[i];
        keys[i].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    SendInput(3, keys, sizeof(INPUT));
}
} // namespace

void duplicationFrameSelfTest(ID3D11Device* device, ID3D11DeviceContext* context) {
    auto duplication = Microsoft::WRL::Make<TestDuplication>();
    require(duplication != nullptr, "Cannot allocate duplication regression");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = desc.Height = 16;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    Com<ID3D11Texture2D> source;
    check(device->CreateTexture2D(&desc, nullptr, &source), "Duplication regression source");
    check(source.As(&duplication->resource), "Duplication regression resource");
    {
        Capture capture;
        capture.device_ = device;
        capture.context_ = context;
        capture.size_ = {16, 16};
        capture.dup_ = duplication;
        Com<ID3D11Texture2D> frame;
        require(capture.frame(frame) && duplication->held && duplication->released == 0,
                "Capture must retain ownership between frame ticks");
        require(capture.frame(frame) && duplication->held && duplication->released == 1 &&
                duplication->acquired == 2, "Next acquisition must release exactly once first");
        duplication->timeout = true;
        require(!capture.frame(frame) && !duplication->held && duplication->released == 2,
                "Acquisition timeout must leave no outstanding frame");
        duplication->timeout = false;
        require(capture.frame(frame), "Capture must recover after an empty frame tick");
    }
    require(!duplication->held && duplication->released == 3,
            "Capture destruction must release its outstanding frame");
    {
        Capture capture;
        capture.device_ = device;
        capture.context_ = context;
        capture.size_ = {16, 16};
        capture.dup_ = duplication;
        Com<ID3D11Texture2D> frame;
        require(capture.frame(frame), "Access-loss regression acquisition failed");
        duplication->accessLost = true;
        require(!capture.frame(frame) && !capture.dup_ && !capture.desktopFrameHeld_,
                "Release-time access loss must discard the duplication interface");
    }
    require(!duplication->held && duplication->released == 4,
            "Access loss must not double-release the desktop frame");
}

void regionInputSelfTest();
void platformSelfTest(std::ostream& report) {
    {
        Com<ID3D11Device> device;
        Com<ID3D11DeviceContext> context;
        check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                               D3D11_SDK_VERSION, &device, nullptr, &context), "Readback regression device");
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = desc.Height = 16;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        std::vector<uint32_t> pixels(256, 0x11223344);
        D3D11_SUBRESOURCE_DATA data{pixels.data(), 16 * 4, 0};
        Com<ID3D11Texture2D> source, stage;
        check(device->CreateTexture2D(&desc, &data, &source), "Readback regression texture");
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device->CreateTexture2D(&desc, nullptr, &stage), "Readback regression stage");
        context->CopyResource(stage.Get(), source.Get());
        context->Flush();
        D3D11_MAPPED_SUBRESOURCE mapped{};
        HRESULT result;
        const auto deadline = GetTickCount64() + 2000;
        while ((result = context->Map(stage.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)) ==
               DXGI_ERROR_WAS_STILL_DRAWING && GetTickCount64() < deadline) Sleep(1);
        check(result, "Nonblocking readback completion");
        const bool valid = *static_cast<uint32_t*>(mapped.pData) == pixels.front();
        context->Unmap(stage.Get(), 0);
        require(valid, "Submitted GPU copy did not reach the readback stage");
        duplicationFrameSelfTest(device.Get(), context.Get());
    }
    report << "PASS explicit GPU submission and nonblocking staging completion on D3D11 WARP\n";
    report << "PASS actual Capture frame ownership: retain between ticks, release before next acquisition, timeout, destruction and access-loss cleanup\n";
    regionInputSelfTest();
    report << "PASS region ignores opening-button release and accepts an explicit drag\n";
    HWND notification = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"STATIC", L"",
                                        WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(notification != nullptr, "Cannot create notification test");
    try {
        ToastSurface surface;
        for (float dpi : {96.f, 120.f, 144.f, 192.f}) {
            surface.prepare(L"Recording started", dpi);
            surface.frame(notification, 10, 10, 128);
            surface.frame(notification, 12, 10, 255);
        }
    } catch (...) { DestroyWindow(notification); throw; }
    DestroyWindow(notification);
    report << "PASS cached layered notification bitmap at 100/125/150/200 percent scale (render API only)\n";
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = testProc;
    wc.lpszClassName = L"NCOHotkeyIntegrationTest";
    RegisterClassW(&wc);
    KeyEvents events;
    HWND window =
        CreateWindowExW(WS_EX_TOOLWINDOW, wc.lpszClassName, L"NCO hotkey test", WS_OVERLAPPEDWINDOW,
                        0, 0, 200, 100, nullptr, nullptr, wc.hInstance, &events);
    HWND conflict = CreateWindowExW(0, L"STATIC", L"NCO shortcut owner", 0, 0, 0, 1, 1,
                                    HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    require(window && conflict, "Cannot create hotkey test windows");
    try {
        {
            Hotkeys keys(window);
            auto duplicates = keys.apply({{VK_F7, 0}, {VK_F7, 0}});
            require(!duplicates[0].empty() && !duplicates[1].empty(),
                    "Duplicate shortcuts were accepted");
            const bool owned = RegisterHotKey(conflict, 999, MOD_NOREPEAT, VK_F6) != FALSE;
            auto conflicts = keys.apply({{VK_F6, 0}});
            if (owned)
                require(!conflicts[0].empty(), "Shortcut owned by another window was accepted");
            UnregisterHotKey(conflict, 999);
            auto valid = keys.apply({{VK_F7, 0}, {VK_F7, MOD_CONTROL | MOD_SHIFT}});
            require(valid[0].empty() && valid[1].empty(),
                    "Single-key or modifier shortcut registration failed");
            report << "PASS Windows global shortcut registration, single keys, combinations and "
                      "duplicate detection\n";
            report
                << (owned
                        ? "PASS owned-shortcut conflict warning\n"
                        : "UNVERIFIED controlled shortcut conflict: test key was already owned\n");
            ShowWindow(window, SW_SHOWMINNOACTIVE);
            require(IsIconic(window) != FALSE, "Hotkey test window did not minimize");
            INPUT input[6]{};
            auto set = [&](int i, WORD key, bool up) {
                input[i].type = INPUT_KEYBOARD;
                input[i].ki.wVk = key;
                input[i].ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
            };
            set(0, VK_F7, false);
            set(1, VK_F7, true);
            if (SendInput(2, input, sizeof(INPUT)) == 2) {
                require(pumpUntil([&] { return events.single > 0; }),
                        "Global single-key shortcut did not reach the minimized window");
                set(0, VK_CONTROL, false);
                set(1, VK_SHIFT, false);
                set(2, VK_F7, false);
                set(3, VK_F7, true);
                set(4, VK_SHIFT, true);
                set(5, VK_CONTROL, true);
                require(SendInput(6, input, sizeof(INPUT)) == 6,
                        "Cannot inject shortcut combination");
                require(pumpUntil([&] { return events.combination > 0; }),
                        "Global combination did not reach the minimized window");
                report << "PASS real global key and combination delivery to a minimized Win32 "
                          "window\n";
            } else
                report << "UNVERIFIED minimized shortcut delivery: desktop input injection "
                          "unavailable\n";
        }
        releaseTestKeys();
        DestroyWindow(conflict);
        DestroyWindow(window);
    } catch (...) {
        releaseTestKeys();
        UnregisterHotKey(conflict, 999);
        DestroyWindow(conflict);
        DestroyWindow(window);
        throw;
    }
}
} // namespace nco
