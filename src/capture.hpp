#pragma once
#include "platform.hpp"
#include "pointer.hpp"
#include <array>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
namespace nco {
struct Source {
    std::wstring name;
    HMONITOR monitor = nullptr;
    HWND window = nullptr;
    RECT region{};
    bool custom = false;
};
std::vector<Source> sources(HWND exclude);
int replaceRegion(std::vector<Source>& list, Source display, const RECT& region);
class Capture {
    Com<ID3D11Device> device_;
    Com<ID3D11DeviceContext> context_;
    Com<IDXGIOutputDuplication> dup_;
    Com<IDXGIOutput1> output_;
    int64_t retryDuplication_ = 0;
    bool pointerVisible_ = false;
    POINT pointerPosition_{};
    winrt::Windows::Graphics::Capture::GraphicsCaptureItem item_{nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool pool_{nullptr};
    winrt::Windows::Graphics::Capture::GraphicsCaptureSession session_{nullptr};
    Com<ID3D11Texture2D> latest_, stage_, thumb_;
    std::array<Com<ID3D11Texture2D>, 3> thumbStages_;
    size_t thumbRead_ = 0, thumbWrite_ = 0, thumbCount_ = 0;
    bool thumbDirty_ = false;
    Size size_, thumbSize_;
    Source source_;
    bool fallback_ = false, pointerFailed_ = false;
    PointerOverlay pointer_;
    winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice wgcDevice_{nullptr};
    winrt::Windows::Graphics::SizeInt32 poolSize_{};
    Com<ID3D11VideoDevice> scaleDevice_;
    Com<ID3D11VideoContext> scaleContext_;
    Com<ID3D11VideoProcessorEnumerator> scaleEnum_;
    Com<ID3D11VideoProcessor> scaleProcessor_;
    Size scaleInput_{};
    void copyFrame(ID3D11Texture2D* raw);
    Com<ID3D11VideoDevice> thumbDevice_;
    Com<ID3D11VideoContext> thumbContext_;
    Com<ID3D11VideoProcessorEnumerator> thumbEnum_;
    Com<ID3D11VideoProcessor> thumbProcessor_;
    Com<ID3D11VideoProcessorInputView> thumbInput_;
    Com<ID3D11VideoProcessorOutputView> thumbOutput_;
    ID3D11Texture2D* thumbInputTexture_ = nullptr;

  public:
    ~Capture();
    void open(const Source& source, int backend = 0);
    bool frame(Com<ID3D11Texture2D>& texture);
    ID3D11Device* device() const {
        return device_.Get();
    }
    ID3D11DeviceContext* context() const {
        return context_.Get();
    }
    Size size() const {
        return size_;
    }
    bool fallback() const {
        return fallback_;
    }
    std::vector<uint8_t> download(ID3D11Texture2D* texture);
    std::pair<Size, std::vector<uint8_t>> thumbnail(ID3D11Texture2D* texture, bool changed = true);
};
} // namespace nco
