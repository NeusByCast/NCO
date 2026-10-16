#pragma once
#include "capture.hpp"
#include "config.hpp"
#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <thread>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}
namespace nco {
void avcheck(int result, const char* what);
struct Packet {
    std::shared_ptr<AVPacket> p;
    int stream = 0;
    AVRational tb{1, 1};
    size_t bytes() const {
        return size_t(p->size);
    }
    int64_t us() const {
        return av_rescale_q(p->pts, tb, {1, 1000000});
    }
    bool key() const {
        return stream == 0 && (p->flags & AV_PKT_FLAG_KEY);
    }
};
struct Track {
    std::shared_ptr<AVCodecParameters> par;
    AVRational tb;
    AVRational frameRate{0, 1};
    std::string name;
};
struct EncoderChoice {
    std::string name;
    std::wstring label;
    bool gpu = false;
};
std::vector<EncoderChoice> probeEncoders();
class VideoEncoder {
    AVCodecContext* codec_ = nullptr;
    AVBufferRef* device_ = nullptr;
    AVBufferRef* frames_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwsContext* sws_ = nullptr;
    Com<ID3D11VideoDevice> vd_;
    Com<ID3D11VideoContext> vc_;
    Com<ID3D11VideoProcessorEnumerator> enumerator_;
    Com<ID3D11VideoProcessor> processor_;
    Size input_, output_;
    FitRect viewport_{};
    bool gpu_ = false;
    Capture* capture_ = nullptr;
    Com<ID3D11Texture2D> uploadNv12_, uploadStage_;
    bool nv12Readback_ = false;
    void convert(ID3D11Texture2D* input, ID3D11Texture2D* output, UINT slice);

  public:
    std::string selected, selectedAdapter;
    double prepareMs = 0, submitMs = 0;
    ~VideoEncoder();
    void open(const Config& config, Capture* capture, Size input, bool synthetic = false);
    Track track() const;
    std::vector<Packet> encode(ID3D11Texture2D* texture, int64_t pts, bool key = false);
    std::vector<Packet> synthetic(int64_t pts, bool key = false);
    std::vector<Packet> flush();
    bool gpu() const {
        return gpu_;
    }

  private:
    std::vector<Packet> send(AVFrame* f);
    void prepareCpu(const uint8_t* bytes, int stride);
};
class AudioEncoder {
    AVCodecContext* codec_ = nullptr;
    SwrContext* swr_ = nullptr;
    AVFrame* frame_ = nullptr;
    int stream_ = 1;

  public:
    ~AudioEncoder();
    void open(bool webm, int stream);
    int frameSize() const {
        return codec_->frame_size;
    }
    Track track() const;
    std::vector<Packet> encode(const std::vector<float>& samples, int64_t pts);
    std::vector<Packet> flush();

  private:
    std::vector<Packet> send(AVFrame* f);
};
class Muxer {
    AVFormatContext* ctx_ = nullptr;
    std::vector<Track> tracks_;
    int64_t origin_ = AV_NOPTS_VALUE;
    std::vector<int64_t> last_;
    bool wrote_ = false, preserve_ = false;

  public:
    ~Muxer();
    void open(const std::filesystem::path& path, const std::vector<Track>& tracks,
              bool fragmented = false, bool preserveTimeline = false);
    void write(const Packet& p, int64_t removedUs = 0);
    void close();
};
void remuxFile(const std::filesystem::path& source, const std::filesystem::path& target);
bool verifyFile(const std::filesystem::path& path);
class DiskWriter {
    struct Job {
        enum Type { Open, Write, Close, Replay, Remux, Verify, Quit } type;
        Packet packet;
        int64_t removed = 0;
        std::filesystem::path path, source;
        std::vector<Track> tracks;
        std::vector<Packet> clip;
        bool remux = false, keepSource = false;
        std::string format;
    };
    std::jthread thread_, exportThread_;
    std::mutex mutex_, exportMutex_;
    std::condition_variable cv_, exportCv_;
    std::deque<Job> queue_, exports_;
    size_t bytes_ = 0, exportBytes_ = 0;
    std::function<void(std::string, bool)> notify_;
    void push(Job job);
    void pushExport(Job job);
    void run();
    void exportRun();

  public:
    explicit DiskWriter(std::function<void(std::string, bool)> notify);
    ~DiskWriter();
    void open(std::filesystem::path path, std::vector<Track> tracks, bool remux,
              std::string format);
    void write(Packet p, int64_t removed);
    void close();
    void remux(std::filesystem::path source, std::filesystem::path target);
    void replay(std::filesystem::path path, std::vector<Track> tracks, std::vector<Packet> clip);
};
int mediaSelfTest();
} // namespace nco
