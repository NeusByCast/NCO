#include "capture.hpp"
#include <d3d11_4.h>
#include <dwmapi.h>
#include <dxgi1_6.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Graphics.DirectX.h>
namespace nco {
int replaceRegion(std::vector<Source>& list, Source display, const RECT& region) {
    if (!display.monitor || region.right - region.left < 64 || region.bottom - region.top < 64)
        throw std::runtime_error("Choose a display and drag a region at least 64 by 64 pixels");
    display.custom = true;
    display.window = nullptr;
    display.region = region;
    display.name = L"Region · " + std::to_wstring(region.right - region.left) + L" × " +
                   std::to_wstring(region.bottom - region.top);
    std::erase_if(list, [](const Source& source) { return source.custom; });
    list.push_back(std::move(display));
    return int(list.size()) - 1;
}
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
            DWORD processId = 0;
            GetWindowThreadProcessId(h, &processId);
            DWORD cloaked = 0;
            DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
            if (processId == GetCurrentProcessId() || cloaked || IsIconic(h) ||
                (GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW))
                return TRUE;
            HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
            if (process) {
                wchar_t path[32768]{};
                DWORD length = DWORD(std::size(path));
                if (QueryFullProcessImageNameW(process, 0, path, &length)) {
                    auto name = std::filesystem::path(path).filename().wstring();
                    if (_wcsicmp(name.c_str(), L"TextInputHost.exe") == 0 ||
                        _wcsicmp(name.c_str(), L"SearchHost.exe") == 0 ||
                        _wcsicmp(name.c_str(), L"StartMenuExperienceHost.exe") == 0) {
                        CloseHandle(process);
                        return TRUE;
                    }
                }
                CloseHandle(process);
            }
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
    Com<IDXGIAdapter1> chosen, outputAdapter;
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
                outputAdapter = a;
                output = o;
                break;
            }
        }
        if (chosen)
            break;
    }
    // For WGC, create the capture pool on the high-performance adapter so
    // capture and hardware encoding can share D3D textures on hybrid-GPU PCs.
    // Desktop Duplication still has to run on the adapter driving the monitor.
    if (backend != 2) {
        Com<IDXGIFactory6> modern;
        Com<IDXGIAdapter1> performance;
        if (SUCCEEDED(factory.As(&modern)) &&
            SUCCEEDED(modern->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                         IID_PPV_ARGS(&performance)))) {
            DXGI_ADAPTER_DESC1 desc{};
            performance->GetDesc1(&desc);
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
                chosen = performance;
        }
    }
    auto createDevice = [&](IDXGIAdapter1* adapter) {
        device_.Reset();
        context_.Reset();
        D3D_FEATURE_LEVEL level;
        check(D3D11CreateDevice(adapter,
                                adapter ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                                nullptr,
                                D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT,
                                nullptr, 0, D3D11_SDK_VERSION, &device_, &level, &context_),
              "D3D11 device");
        Com<ID3D11Multithread> mt;
        check(context_.As(&mt), "D3D context protection");
        mt->SetMultithreadProtected(TRUE);
    };
    auto sameAdapter = [](IDXGIAdapter1* a, IDXGIAdapter1* b) {
        if (!a || !b) return a == b;
        DXGI_ADAPTER_DESC1 da{}, db{};
        return SUCCEEDED(a->GetDesc1(&da)) && SUCCEEDED(b->GetDesc1(&db)) &&
               da.AdapterLuid.HighPart == db.AdapterLuid.HighPart &&
               da.AdapterLuid.LowPart == db.AdapterLuid.LowPart;
    };
    createDevice(chosen.Get());
    auto openDuplication = [&] {
        if (!output)
            throw std::runtime_error("No compatible display output");
        Com<IDXGIOutput1> o;
        check(output.As(&o), "Desktop Duplication output");
        output_ = o;
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
        bool duplicated = false;
        if (backend == 0 && !source.window && sameAdapter(chosen.Get(), outputAdapter.Get())) {
            try {
                openDuplication();
                duplicated = true;
            } catch (const std::exception& e) {
                dup_.Reset();
                Log::get().write(std::string("Desktop Duplication unavailable: ") + e.what());
            }
        } else if (backend == 0 && !source.window) {
            Log::get().write("Auto capture: preferring WGC on the high-performance GPU to avoid cross-adapter frame readback");
        }
        if (!duplicated) {
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
            wgcDevice_ = wd;
            poolSize_ = item_.Size();
            pool_ =
                winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool::CreateFreeThreaded(
                    wd,
                    winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                    2, item_.Size());
            session_ = pool_.CreateCaptureSession(item_);
            try {
                auto border = session_.try_as<winrt::Windows::Graphics::Capture::IGraphicsCaptureSession3>();
                if (border)
                    border.IsBorderRequired(false);
            } catch (...) {
                Log::get().write("Window capture border is controlled by Windows permissions");
            }
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
            if (outputAdapter) {
                chosen = outputAdapter;
                createDevice(outputAdapter.Get());
            }
            openDuplication();
        }
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
void Capture::copyFrame(ID3D11Texture2D* raw) {
    D3D11_TEXTURE2D_DESC sourceDesc{};
    raw->GetDesc(&sourceDesc);
    if (!latest_) {
        D3D11_TEXTURE2D_DESC d{};
        d.Width = size_.w;
        d.Height = size_.h;
        d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        check(device_->CreateTexture2D(&d, nullptr, &latest_), "Capture mailbox texture");
    }
    if (source_.custom) {
        if (UINT(source_.region.right) > sourceDesc.Width ||
            UINT(source_.region.bottom) > sourceDesc.Height)
            throw std::runtime_error("Display resized outside capture region; recording retained");
        D3D11_BOX box{UINT(source_.region.left), UINT(source_.region.top), 0,
                     UINT(source_.region.right), UINT(source_.region.bottom), 1};
        context_->CopySubresourceRegion(latest_.Get(), 0, 0, 0, 0, raw, 0, &box);
        return;
    }
    if (sourceDesc.Width == UINT(size_.w) && sourceDesc.Height == UINT(size_.h)) {
        context_->CopyResource(latest_.Get(), raw);
        return;
    }
    // Keep a fixed encoder input when games switch modes or windows are resized.
    if (!scaleProcessor_ || scaleInput_.w != int(sourceDesc.Width) ||
        scaleInput_.h != int(sourceDesc.Height)) {
        scaleProcessor_.Reset();
        scaleEnum_.Reset();
        check(device_.As(&scaleDevice_), "Capture resize device");
        check(context_.As(&scaleContext_), "Capture resize context");
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC d{};
        d.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        d.InputWidth = sourceDesc.Width;
        d.InputHeight = sourceDesc.Height;
        d.OutputWidth = size_.w;
        d.OutputHeight = size_.h;
        d.InputFrameRate = d.OutputFrameRate = {60, 1};
        d.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;
        check(scaleDevice_->CreateVideoProcessorEnumerator(&d, &scaleEnum_), "Capture resize enumerator");
        check(scaleDevice_->CreateVideoProcessor(scaleEnum_.Get(), 0, &scaleProcessor_), "Capture resize processor");
        scaleInput_ = {int(sourceDesc.Width), int(sourceDesc.Height)};
        Log::get().write("Capture size changed=" + std::to_string(sourceDesc.Width) + "x" +
                         std::to_string(sourceDesc.Height) + "; fitting to original recording dimensions");
    }
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC id{};
    id.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    Com<ID3D11VideoProcessorInputView> input;
    check(scaleDevice_->CreateVideoProcessorInputView(raw, scaleEnum_.Get(), &id, &input), "Capture resize input");
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC od{};
    od.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2D;
    Com<ID3D11VideoProcessorOutputView> output;
    check(scaleDevice_->CreateVideoProcessorOutputView(latest_.Get(), scaleEnum_.Get(), &od, &output), "Capture resize output");
    auto fit = letterbox(scaleInput_, size_);
    RECT source{0, 0, scaleInput_.w, scaleInput_.h}, destination{fit.x, fit.y, fit.x + fit.w, fit.y + fit.h};
    RECT full{0, 0, size_.w, size_.h};
    scaleContext_->VideoProcessorSetStreamSourceRect(scaleProcessor_.Get(), 0, TRUE, &source);
    scaleContext_->VideoProcessorSetStreamDestRect(scaleProcessor_.Get(), 0, TRUE, &destination);
    scaleContext_->VideoProcessorSetOutputTargetRect(scaleProcessor_.Get(), TRUE, &full);
    D3D11_VIDEO_COLOR black{};
    black.RGBA.A = 1;
    scaleContext_->VideoProcessorSetOutputBackgroundColor(scaleProcessor_.Get(), FALSE, &black);
    D3D11_VIDEO_PROCESSOR_STREAM stream{};
    stream.Enable = TRUE;
    stream.pInputSurface = input.Get();
    check(scaleContext_->VideoProcessorBlt(scaleProcessor_.Get(), output.Get(), 0, 1, &stream), "Capture resize");
}
bool Capture::frame(Com<ID3D11Texture2D>& texture) {
    Com<ID3D11Texture2D> raw;
    if (pool_) {
        auto f = pool_.TryGetNextFrame();
        if (!f)
            return false;
        // Consume the newest queued frame instead of replaying an older preview.
        while (auto newer = pool_.TryGetNextFrame()) {
            f.Close();
            f = std::move(newer);
        }
        auto s = f.ContentSize();
        auto access =
            f.Surface()
                .as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        check(access->GetInterface(IID_PPV_ARGS(&raw)), "Capture texture");
        if (s.Width <= 0 || s.Height <= 0) {
            f.Close();
            return false;
        }
        copyFrame(raw.Get());
        f.Close();
        if (s.Width != poolSize_.Width || s.Height != poolSize_.Height) {
            pool_.Recreate(wgcDevice_,
                winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                2, s);
            poolSize_ = s;
            Log::get().write("Window capture pool resized=" + std::to_string(s.Width) + "x" +
                             std::to_string(s.Height) + "; recording dimensions preserved");
        }
    } else {
        DXGI_OUTDUPL_FRAME_INFO info{};
        Com<IDXGIResource> resource;
        if (!dup_) {
            if (clockUs() < retryDuplication_)
                return false;
            retryDuplication_ = clockUs() + 500000;
            HRESULT reopen = output_->DuplicateOutput(device_.Get(), &dup_);
            if (FAILED(reopen))
                return false;
            DXGI_OUTDUPL_DESC recovered{};
            dup_->GetDesc(&recovered);
            if (recovered.Rotation != DXGI_MODE_ROTATION_IDENTITY)
                throw std::runtime_error("Display rotated; use Windows Graphics Capture for this display");
            pointerVisible_ = false;
            Log::get().write("Desktop Duplication recovered after display transition");
        }
        HRESULT hr = dup_->AcquireNextFrame(0, &info, &resource);
        if (hr == DXGI_ERROR_ACCESS_LOST || hr == DXGI_ERROR_ACCESS_DENIED) {
            Log::get().write("Desktop Duplication interrupted; retrying after display transition");
            dup_.Reset();
            retryDuplication_ = clockUs() + 250000;
            return false;
        }
        if (hr == DXGI_ERROR_WAIT_TIMEOUT)
            return false;
        check(hr, "Acquire desktop frame");
        try {
            check(resource.As(&raw), "Desktop texture");
            copyFrame(raw.Get());
        } catch (...) {
            dup_->ReleaseFrame();
            throw;
        }
        dup_->ReleaseFrame();
        if (info.LastMouseUpdateTime.QuadPart) {
            pointerVisible_ = info.PointerPosition.Visible != FALSE;
            pointerPosition_ = info.PointerPosition.Position;
        }
        if (pointerVisible_ && !pointerFailed_) {
            try {
                Com<IDXGISurface> surface;
                check(latest_.As(&surface), "Cursor capture surface");
                RECT region = source_.region;
                MONITORINFO mi{sizeof(mi)};
                GetMonitorInfoW(source_.monitor, &mi);
                if (source_.custom) {
                    OffsetRect(&region, mi.rcMonitor.left, mi.rcMonitor.top);
                } else
                    region = mi.rcMonitor;
                POINT position{mi.rcMonitor.left + pointerPosition_.x,
                               mi.rcMonitor.top + pointerPosition_.y};
                float pointerScale = 1;
                D3D11_TEXTURE2D_DESC rawDescription{};
                raw->GetDesc(&rawDescription);
                if (!source_.custom && (int(rawDescription.Width) != size_.w ||
                                        int(rawDescription.Height) != size_.h)) {
                    auto fit = letterbox({int(rawDescription.Width), int(rawDescription.Height)}, size_);
                    pointerScale = float(fit.w) / rawDescription.Width;
                    position.x = region.left + fit.x + LONG(pointerPosition_.x * pointerScale);
                    position.y = region.top + fit.y + LONG(pointerPosition_.y * pointerScale);
                }
                pointer_.draw(surface.Get(), region, position, pointerScale);
            } catch (const std::exception& e) {
                pointerFailed_ = true;
                Log::get().write(std::string("Cursor composition unavailable: ") + e.what());
            }
        }
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
        const int width = std::min(1280, size_.w);
        thumbSize_ = {width, std::max(1, width * size_.h / size_.w)};
        check(device_.As(&thumbDevice_), "Preview video device");
        check(context_.As(&thumbContext_), "Preview video context");
        D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc{};
        desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
        desc.InputWidth = size_.w;
        desc.InputHeight = size_.h;
        desc.OutputWidth = thumbSize_.w;
        desc.OutputHeight = thumbSize_.h;
        desc.InputFrameRate = {60, 1};
        desc.OutputFrameRate = {60, 1};
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
        for (auto& stage : thumbStages_)
            check(device_->CreateTexture2D(&d, nullptr, &stage), "Preview staging");
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
    context_->CopyResource(thumbStages_[thumbCursor_].Get(), thumb_.Get());
    thumbPending_[thumbCursor_] = true;
    thumbCursor_ = (thumbCursor_ + 1) % thumbStages_.size();
    if (!thumbPending_[thumbCursor_])
        return {};
    auto stage = thumbStages_[thumbCursor_].Get();
    D3D11_MAPPED_SUBRESOURCE m{};
    const HRESULT mapped = context_->Map(stage, 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (mapped == DXGI_ERROR_WAS_STILL_DRAWING)
        return {};
    check(mapped, "Preview map");
    std::vector<uint8_t> b(size_t(thumbSize_.w) * thumbSize_.h * 4);
    for (int y = 0; y < thumbSize_.h; ++y)
        memcpy(b.data() + size_t(y) * thumbSize_.w * 4,
               static_cast<uint8_t*>(m.pData) + size_t(y) * m.RowPitch, size_t(thumbSize_.w) * 4);
    context_->Unmap(stage, 0);
    thumbPending_[thumbCursor_] = false;
    return {thumbSize_, std::move(b)};
}
} // namespace nco
