#include "capture.hpp"
#include <d3d11_4.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.DirectX.h>
namespace nco {
std::vector<Source> sources(HWND exclude) {
    std::vector<Source> r;
    EnumDisplayMonitors(
        nullptr, nullptr,
        [](HMONITOR h, HDC, LPRECT, LPARAM p) -> BOOL {
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            GetMonitorInfoW(h, reinterpret_cast<MONITORINFO*>(&info));
            auto& v = *reinterpret_cast<std::vector<Source>*>(p);
            v.push_back({L"Display " + std::to_wstring(v.size() + 1) + L" · " +
                             std::to_wstring(info.rcMonitor.right - info.rcMonitor.left) + L" × " +
                             std::to_wstring(info.rcMonitor.bottom - info.rcMonitor.top),
                         h, nullptr, info.rcMonitor});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&r));
    struct Args {
        std::vector<Source>* r;
        HWND ex;
    };
    Args a{&r, exclude};
    EnumWindows(
        [](HWND h, LPARAM p) -> BOOL {
            auto a = reinterpret_cast<Args*>(p);
            if (h == a->ex || !IsWindowVisible(h) || GetWindow(h, GW_OWNER) ||
                !GetWindowTextLengthW(h))
                return TRUE;
            wchar_t t[256];
            GetWindowTextW(h, t, 256);
            RECT rc{};
            GetClientRect(h, &rc);
            if (rc.right > 0 && rc.bottom > 0)
                a->r->push_back({std::wstring(L"Window · ") + t, nullptr, h});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&a));
    return r;
}
Capture::~Capture() {
    try {
        if (session_)
            session_.Close();
    } catch (...) {
    }
    try {
        if (pool_)
            pool_.Close();
    } catch (...) {
    }
}
void Capture::open(const Source& source, int backend) {
    if (backend == 2 && source.window)
        throw std::runtime_error(
            "Desktop Duplication requires a display or region. Select a display or use Auto.");
    source_ = source;
    Com<IDXGIFactory1> factory;
    check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)), "DXGI factory");
    Com<IDXGIAdapter1> chosen;
    Com<IDXGIOutput> output;
    HMONITOR monitor = source.monitor ? source.monitor
                                      : MonitorFromWindow(source.window, MONITOR_DEFAULTTONEAREST);
    for (UINT i = 0;; ++i) {
        Com<IDXGIAdapter1> a;
        if (factory->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND)
            break;
        for (UINT j = 0;; ++j) {
            Com<IDXGIOutput> o;
            if (a->EnumOutputs(j, &o) == DXGI_ERROR_NOT_FOUND)
                break;
            DXGI_OUTPUT_DESC d{};
            o->GetDesc(&d);
            if (d.Monitor == monitor) {
                chosen = a;
                output = o;
                break;
            }
        }
        if (chosen)
            break;
    }
    D3D_FEATURE_LEVEL level;
    check(D3D11CreateDevice(chosen.Get(),
                            chosen ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE, nullptr,
                            D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                            nullptr, 0, D3D11_SDK_VERSION, &device_, &level, &context_),
          "D3D11 device");
    Com<ID3D11Multithread> mt;
    check(context_.As(&mt), "D3D context protection");
    mt->SetMultithreadProtected(TRUE);
    auto openDuplication = [&] {
        if (!output)
            throw std::runtime_error("No compatible display output");
        Com<IDXGIOutput1> o;
        check(output.As(&o), "Desktop Duplication output");
        check(o->DuplicateOutput(device_.Get(), &dup_), "Desktop Duplication");
        DXGI_OUTDUPL_DESC d{};
        dup_->GetDesc(&d);
        if (d.Rotation != DXGI_MODE_ROTATION_IDENTITY)
            throw std::runtime_error("Rotated displays require Windows Graphics Capture");
        size_ = {int(d.ModeDesc.Width), int(d.ModeDesc.Height)};
        fallback_ = true;
        Log::get().write("Capture backend: Desktop Duplication");
    };
    if (backend == 2)
        openDuplication();
    else {
        try {
            auto interop = winrt::get_activation_factory<
                winrt::Windows::Graphics::Capture::GraphicsCaptureItem,
                IGraphicsCaptureItemInterop>();
            if (source.window)
                check(interop->CreateForWindow(
                          source.window,
                          winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                          winrt::put_abi(item_)),
                      "Window capture");
            else
                check(interop->CreateForMonitor(
                          monitor,
                          winrt::guid_of<winrt::Windows::Graphics::Capture::GraphicsCaptureItem>(),
                          winrt::put_abi(item_)),
                      "Monitor capture");
            auto s = item_.Size();
            size_ = {s.Width, s.Height};
            Com<IDXGIDevice> dx;
            check(device_.As(&dx), "DXGI device");
            winrt::com_ptr<IInspectable> inspect;
            check(CreateDirect3D11DeviceFromDXGIDevice(dx.Get(), inspect.put()),
                  "WinRT D3D device");
            auto wd = inspect.as<winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice>();
            pool_ =
                winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                    wd,
                    winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    2, item_.Size());
            session_ = pool_.CreateCaptureSession(item_);
            session_.StartCapture();
            Log::get().write("Capture backend: Windows Graphics Capture");
        } catch (...) {
            if (source.window || backend == 1)
                throw;
            try {
                if (session_)
                    session_.Close();
            } catch (...) {
            }
            try {
                if (pool_)
                    pool_.Close();
            } catch (...) {
            }
            session_ = nullptr;
            pool_ = nullptr;
            item_ = nullptr;
            Log::get().write("WGC unavailable; using Desktop Duplication");
            openDuplication();
        }
    }
    if (source.custom) {
        int w = source.region.right - source.region.left,
            h = source.region.bottom - source.region.top;
        if (w < 64 || h < 64 || source.region.left < 0 || source.region.top < 0 ||
            source.region.right > size_.w || source.region.bottom > size_.h)
            throw std::runtime_error("Capture region is outside the selected display");
        size_ = {w, h};
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (chosen) {
        chosen->GetDesc1(&ad);
        Log::get().write("Capture GPU: " + utf8(ad.Description));
    }
}
bool Capture::frame(Com<ID3D11Texture2D>& texture) {
    Com<ID3D11Texture2D> raw;
    if (pool_) {
        auto f = pool_.TryGetNextFrame();
        if (!f)
            return false;
        auto s = f.ContentSize();
        auto access =
            f.Surface()
                .as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        check(access->GetInterface(IID_PPV_ARGS(&raw)), "Capture texture");
        if (!source_.custom && (s.Width != size_.w || s.Height != size_.h))
            throw std::runtime_error(
                "Capture source changed size. Stop and select the source again.");
        D3D11_TEXTURE2D_DESC d{};
        raw->GetDesc(&d);
        d.Width = UINT(size_.w);
        d.Height = UINT(size_.h);
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        d.MiscFlags = 0;
        d.ArraySize = 1;
        d.MipLevels = 1;
        if (!latest_)
            check(device_->CreateTexture2D(&d, nullptr, &latest_), "Capture mailbox texture");
        if (source_.custom) {
            D3D11_BOX box{UINT(source_.region.left),  UINT(source_.region.top),    0,
                          UINT(source_.region.right), UINT(source_.region.bottom), 1};
            context_->CopySubresourceRegion(latest_.Get(), 0, 0, 0, 0, raw.Get(), 0, &box);
        } else
            context_->CopyResource(latest_.Get(), raw.Get());
        f.Close();
    } else {
        DXGI_OUTDUPL_FRAME_INFO info{};
        Com<IDXGIResource> resource;
        HRESULT hr = dup_->AcquireNextFrame(0, &info, &resource);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            return false;
        check(hr, "Acquire desktop frame");
        try {
            check(resource.As(&raw), "Desktop texture");
            D3D11_TEXTURE2D_DESC d{};
            raw->GetDesc(&d);
            d.Width = size_.w;
            d.Height = size_.h;
            d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
            d.MiscFlags = 0;
            if (!latest_)
                check(device_->CreateTexture2D(&d, nullptr, &latest_), "Desktop mailbox");
            if (source_.custom) {
                D3D11_BOX b{UINT(source_.region.left),  UINT(source_.region.top),    0,
                            UINT(source_.region.right), UINT(source_.region.bottom), 1};
                context_->CopySubresourceRegion(latest_.Get(), 0, 0, 0, 0, raw.Get(), 0, &b);
            } else
                context_->CopyResource(latest_.Get(), raw.Get());
        } catch (...) {
            dup_->ReleaseFrame();
            throw;
        }
        dup_->ReleaseFrame();
    }
    texture = latest_;
    return true;
}
std::vector<uint8_t> Capture::download(ID3D11Texture2D* texture) {
    D3D11_TEXTURE2D_DESC d{};
    texture->GetDesc(&d);
    if (!stage_) {
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        d.MiscFlags = 0;
        check(device_->CreateTexture2D(&d, nullptr, &stage_), "Readback texture");
    }
    context_->CopyResource(stage_.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE m{};
    check(context_->Map(stage_.Get(), 0, D3D11_MAP_READ, 0, &m), "Readback map");
    std::vector<uint8_t> out(size_t(size_.w) * size_.h * 4);
    for (int y = 0; y < size_.h; ++y)
        memcpy(out.data() + size_t(y) * size_.w * 4,
               static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(size_.w) * 4);
    context_->Unmap(stage_.Get(), 0);
    return out;
}
std::pair<Size, std::vector<uint8_t>> Capture::thumbnail(ID3D11Texture2D* t) {
    if (!thumb_) {
        thumbSize_ = {480, std::max(1, 480 * size_.h / size_.w)};
        check(device_.As(&thumbDevice_), "Preview video device");
        check(context_.As(&thumbContext_), "Preview video context");
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputWidth = size_.w;
        desc.InputHeight = size_.h;
        desc.OutputWidth = thumbSize_.w;
        desc.OutputHeight = thumbSize_.h;
        desc.InputFrameRate = {8, 1};
        desc.OutputFrameRate = {8, 1};
        desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        check(thumbDevice_->CreateVideoProcessorEnumerator(&desc, &thumbEnum_), "Preview scaler");
        check(thumbDevice_->CreateVideoProcessor(thumbEnum_.Get(), 0, &thumbProcessor_),
              "Preview processor");
        D3D11_TEXTURE2D_DESC d{};
        d.Width = thumbSize_.w;
        d.Height = thumbSize_.h;
        d.MipLevels = 1;
        d.ArraySize = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET;
        check(device_->CreateTexture2D(&d, nullptr, &thumb_), "Preview texture");
        d.Usage = D3D11_USAGE_STAGING;
        d.BindFlags = 0;
        d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        check(device_->CreateTexture2D(&d, nullptr, &thumbStage_), "Preview staging");
    }
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id{};
    id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    Com<ID3D11VideoProcessorInputView> iv;
    check(thumbDevice_->CreateVideoProcessorInputView(t, thumbEnum_.Get(), &id, &iv),
          "Preview input");
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
    od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    Com<ID3D11VideoProcessorOutputView> ov;
    check(thumbDevice_->CreateVideoProcessorOutputView(thumb_.Get(), thumbEnum_.Get(), &od, &ov),
          "Preview output");
    RECT src{0, 0, size_.w, size_.h}, dst{0, 0, thumbSize_.w, thumbSize_.h};
    thumbContext_->VideoProcessorSetStreamSourceRect(thumbProcessor_.Get(), 0, TRUE, &src);
    thumbContext_->VideoProcessorSetStreamDestRect(thumbProcessor_.Get(), 0, TRUE, &dst);
    D3D11_VIDEO_PROCESSOR_STREAM s{};
    s.Enable = TRUE;
    s.pInputSurface = iv.Get();
    check(thumbContext_->VideoProcessorBlt(thumbProcessor_.Get(), ov.Get(), 0, 1, &s),
          "Scale preview");
    context_->CopyResource(thumbStage_.Get(), thumb_.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    check(context_->Map(thumbStage_.Get(), 0, D3D11_MAP_READ, 0, &m), "Preview map");
    std::vector<uint8_t> b(size_t(thumbSize_.w) * thumbSize_.h * 4);
    for (int y = 0; y < thumbSize_.h; ++y)
        memcpy(b.data() + size_t(y) * thumbSize_.w * 4,
               static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(thumbSize_.w) * 4);
    context_->Unmap(thumbStage_.Get(), 0);
    return {thumbSize_, std::move(b)};
}
} // namespace nco
