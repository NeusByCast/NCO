#pragma once
// Included only by media.cpp. No staging allocation, readback, or disk access
// occurs until the user explicitly requests a comparison in Diagnostics.
#include "media.hpp"
#include <fstream>
namespace nco {
namespace {
std::atomic<int> qualityState{0}; // idle, waiting, collecting, complete, failed
std::atomic<int64_t> qualityDue{0};
void failQualityComparison(const char* reason) {
    Log::get().write(std::string("Quality comparison failed: ") + reason);
    qualityState.store(4);
}
bool takeQualityComparison(int64_t now) {
    if (qualityState.load(std::memory_order_acquire) != 1 || now < qualityDue.load()) return false;
    int expected = 1;
    return qualityState.compare_exchange_strong(expected, 2);
}
void probeBitmap(const std::filesystem::path& path, Size size, const std::vector<uint8_t>& pixels) {
    BITMAPFILEHEADER file{};
    BITMAPINFOHEADER info{};
    info.biSize = sizeof(info);
    info.biWidth = size.w;
    info.biHeight = -size.h; // top-down, unscaled BGRA
    info.biPlanes = 1;
    info.biBitCount = 32;
    info.biCompression = BI_RGB;
    info.biSizeImage = DWORD(pixels.size());
    file.bfType = 0x4d42;
    file.bfOffBits = sizeof(file) + sizeof(info);
    file.bfSize = file.bfOffBits + info.biSizeImage;
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof(file));
    out.write(reinterpret_cast<const char*>(&info), sizeof(info));
    out.write(reinterpret_cast<const char*>(pixels.data()), std::streamsize(pixels.size()));
    if (!out) throw std::runtime_error("Cannot write comparison bitmap");
}
}
bool requestQualityComparison() {
    int state = qualityState.load();
    do {
        if (state == 1 || state == 2) return false;
    } while (!qualityState.compare_exchange_weak(state, 2));
    qualityDue.store(clockUs() + 3000000);
    qualityState.store(1, std::memory_order_release);
    return true;
}
const wchar_t* qualityComparisonStatus() {
    switch (qualityState.load()) {
    case 1: return L"Waiting 3 seconds — switch to the affected screen";
    case 2: return L"Saving comparison…";
    case 3: return L"Saved — open comparison folder below";
    case 4: return L"Failed — see diagnostic log; click to retry";
    default: return L"Save one frame at each stage (screen images stay local)";
    }
}
class QualityProbe {
    Com<ID3D11Texture2D> original_, converted_;
    Size input_, output_;
    bool copied_ = false;
    static Com<ID3D11Texture2D> staging(Capture* capture, Size size, DXGI_FORMAT format) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(size.w);
        desc.Height = UINT(size.h);
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = format;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Com<ID3D11Texture2D> texture;
        check(capture->device()->CreateTexture2D(&desc, nullptr, &texture), "Comparison staging texture");
        return texture;
    }
    static std::vector<uint8_t> read(Capture* capture, ID3D11Texture2D* texture, int rowBytes, int rows) {
        std::vector<uint8_t> bytes(size_t(rowBytes) * rows);
        D3D11_MAPPED_SUBRESOURCE map{};
        // Only this explicit diagnostic waits, after the frame was submitted to
        // the encoder. Normal recording continues to use its existing path.
        check(capture->context()->Map(texture, 0, D3D11_MAP_READ, 0, &map), "Comparison readback");
        for (int y = 0; y < rows; ++y)
            memcpy(bytes.data() + size_t(y) * rowBytes,
                   static_cast<const uint8_t*>(map.pData) + size_t(y) * map.RowPitch, rowBytes);
        capture->context()->Unmap(texture, 0);
        return bytes;
    }
    std::vector<uint8_t> render(AVPixelFormat format, const uint8_t* const* source, const int* strides) {
        SwsContext* context = sws_getContext(output_.w, output_.h, format,
            output_.w, output_.h, AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
        if (!context) throw std::runtime_error("Cannot create comparison colour converter");
        const auto coefficients = sws_getCoefficients(SWS_CS_ITU709);
        const int colorResult = sws_setColorspaceDetails(context, coefficients, 0, coefficients, 1, 0, 1 << 16, 1 << 16);
        if (colorResult < 0) { sws_freeContext(context); avcheck(colorResult, "Comparison BT.709 conversion"); }
        std::vector<uint8_t> rgb(size_t(output_.w) * output_.h * 4);
        uint8_t* destination[4] = {rgb.data()};
        int destinationStrides[4] = {output_.w * 4};
        const int rows = sws_scale(context, source, strides, 0, output_.h, destination, destinationStrides);
        sws_freeContext(context);
        if (rows != output_.h) throw std::runtime_error("Incomplete comparison colour conversion");
        return rgb;
    }
    void decode(const Packet& packet, const Track& track, const std::filesystem::path& path) {
        const auto decoder = avcodec_find_decoder(track.par->codec_id);
        if (!decoder) throw std::runtime_error("No software decoder for comparison");
        AVCodecContext* context = avcodec_alloc_context3(decoder);
        AVFrame* frame = av_frame_alloc();
        try {
            if (!context || !frame) throw std::bad_alloc();
            avcheck(avcodec_parameters_to_context(context, track.par.get()), "Comparison decoder parameters");
            context->thread_count = 1;
            avcheck(avcodec_open2(context, decoder, nullptr), "Open comparison software decoder");
            avcheck(avcodec_send_packet(context, packet.p.get()), "Decode comparison keyframe");
            int result = avcodec_receive_frame(context, frame);
            if (result == AVERROR(EAGAIN)) {
                avcheck(avcodec_send_packet(context, nullptr), "Drain comparison decoder");
                result = avcodec_receive_frame(context, frame);
            }
            avcheck(result, "Receive comparison decoded frame");
            if (frame->width != output_.w || frame->height != output_.h)
                throw std::runtime_error("Comparison decoder returned unexpected dimensions");
            const uint8_t* data[4] = {frame->data[0], frame->data[1], frame->data[2], frame->data[3]};
            probeBitmap(path, output_, render(AVPixelFormat(frame->format), data, frame->linesize));
        } catch (...) {
            av_frame_free(&frame);
            avcodec_free_context(&context);
            throw;
        }
        av_frame_free(&frame);
        avcodec_free_context(&context);
    }
  public:
    int64_t pts = 0, deadline = 0;
    QualityProbe(Capture* capture, ID3D11Texture2D* input, Size output, int64_t framePts)
        : output_(output), pts(framePts), deadline(clockUs() + 5000000) {
        D3D11_TEXTURE2D_DESC desc{};
        input->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM)
            throw std::runtime_error("Comparison input is not BGRA8");
        input_ = {int(desc.Width), int(desc.Height)};
        original_ = staging(capture, input_, desc.Format);
        converted_ = staging(capture, output_, DXGI_FORMAT_NV12);
        capture->context()->CopyResource(original_.Get(), input);
    }
    void copyConverted(Capture* capture, ID3D11Texture2D* texture, UINT slice) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_NV12 || desc.Width != UINT(output_.w) || desc.Height != UINT(output_.h))
            throw std::runtime_error("Unexpected encoder surface format or dimensions");
        capture->context()->CopySubresourceRegion(converted_.Get(), 0, 0, 0, 0, texture,
                                                  D3D11CalcSubresource(0, slice, desc.MipLevels), nullptr);
        copied_ = true;
    }
    void save(const Packet& packet, const Track& track, Capture* capture) {
        if (!copied_ || !packet.key())
            throw std::runtime_error("Matching frame is not an independently decodable keyframe");
        auto folder = uniquePath(appDir() / L"quality-comparisons", "Comparison-", "capture");
        std::filesystem::create_directories(folder);
        std::ofstream report(folder / L"comparison.txt");
        report << "Matching pipeline frame pts=" << pts << " time_base=" << track.tb.num << '/' << track.tb.den
               << "\nInput=" << input_.w << 'x' << input_.h << " output=" << output_.w << 'x' << output_.h
               << "\nEncoder=" << avcodec_get_name(track.par->codec_id)
               << "\nConversion: full-range BGRA to limited-range BT.709 NV12 (4:2:0)."
               << "\n01-before.bmp: capture mailbox before colour conversion."
               << "\n02-converted.nv12: exact converted encoder input, tightly packed Y then interleaved UV."
               << "\n02-converted.bmp: NV12 rendered with BT.709 limited-range conversion, no scaling."
               << "\n03-encoded.mkv: matching keyframe taken from the actual encoder output."
               << "\n03-decoded.bmp: matching encoded frame decoded in software, rendered with the same BT.709 converter."
               << "\nCompare at native size. Different input/output sizes include the configured scaling/letterboxing."
               << "\nThis one-shot diagnostic adds GPU copies, a keyframe and readback; it can briefly affect timing."
               << "\nAbsence of artifacts in this sample alone cannot rule out a timing-sensitive fault."
               << "\nThese files contain screen images. They remain local until you choose to share them.\n";
        if (!report) throw std::runtime_error("Cannot write comparison report");
        auto bgra = read(capture, original_.Get(), input_.w * 4, input_.h);
        auto nv12 = read(capture, converted_.Get(), output_.w, output_.h + output_.h / 2);
        probeBitmap(folder / L"01-before.bmp", input_, bgra);
        std::ofstream raw(folder / L"02-converted.nv12", std::ios::binary);
        raw.write(reinterpret_cast<const char*>(nv12.data()), std::streamsize(nv12.size()));
        raw.close();
        if (!raw) throw std::runtime_error("Cannot write comparison NV12 data");
        const uint8_t* source[4] = {nv12.data(), nv12.data() + size_t(output_.w) * output_.h};
        int strides[4] = {output_.w, output_.w};
        probeBitmap(folder / L"02-converted.bmp", output_, render(AV_PIX_FMT_NV12, source, strides));
        Muxer muxer;
        muxer.open(folder / L"03-encoded.mkv", {track});
        muxer.write(packet);
        muxer.close();
        decode(packet, track, folder / L"03-decoded.bmp");
        report << "Complete.\n";
        report.close();
        if (!report) throw std::runtime_error("Cannot finish comparison report");
        Log::get().write("Quality comparison saved: " + utf8(folder.wstring()));
        qualityState.store(3);
    }
};
}
