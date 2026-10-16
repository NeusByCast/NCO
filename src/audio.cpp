#include "audio.hpp"
#include <avrt.h>
#include <functiondiscoverykeys_devpkey.h>
#include <ksmedia.h>
#include <propsys.h>
namespace nco {
std::vector<AudioDevice> audioDevices(bool input) {
    std::vector<AudioDevice> r = {{L"", L"System default"}};
    Com<IMMDeviceEnumerator> e;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e)),
          "Audio enumerator");
    Com<IMMDeviceCollection> all;
    check(e->EnumAudioEndpoints(input ? eCapture : eRender, DEVICE_STATE_ACTIVE, &all),
          "Audio devices");
    UINT n = 0;
    all->GetCount(&n);
    for (UINT i = 0; i < n; ++i) {
        Com<IMMDevice> d;
        all->Item(i, &d);
        PWSTR id = nullptr;
        if (FAILED(d->GetId(&id)))
            continue;
        Com<IPropertyStore> p;
        d->OpenPropertyStore(STGM_READ, &p);
        PROPVARIANT v;
        PropVariantInit(&v);
        if (p)
            p->GetValue(PKEY_Device_FriendlyName, &v);
        r.push_back({id, v.vt == VT_LPWSTR ? v.pwszVal : L"Audio device"});
        PropVariantClear(&v);
        CoTaskMemFree(id);
    }
    return r;
}
void AudioCapture::start(bool input, std::wstring id, std::function<void(std::string)> error) {
    stop();
    thread_ = std::jthread([this, input, id, error](std::stop_token token) {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        DWORD task = 0;
        HANDLE mm = AvSetMmThreadCharacteristicsW(L"Audio", &task);
        try {
            Com<IMMDeviceEnumerator> e;
            check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                   IID_PPV_ARGS(&e)),
                  "Audio enumerator");
            Com<IMMDevice> d;
            if (id.empty())
                check(e->GetDefaultAudioEndpoint(input ? eCapture : eRender, eConsole, &d),
                      "Default audio endpoint");
            else
                check(e->GetDevice(id.c_str(), &d), "Selected audio endpoint");
            Com<IAudioClient> client;
            check(d->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(client.GetAddressOf())),
                  "Audio client");
            WAVEFORMATEX fmt{};
            fmt.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            fmt.nChannels = 2;
            fmt.nSamplesPerSec = 48000;
            fmt.wBitsPerSample = 32;
            fmt.nBlockAlign = 8;
            fmt.nAvgBytesPerSec = 384000;
            DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                          AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                          (input ? 0 : AUDCLNT_STREAMFLAGS_LOOPBACK);
            check(client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 200000, 0, &fmt, nullptr),
                  "Initialize 48 kHz audio");
            check(client->SetEventHandle(event), "Audio event");
            Com<IAudioCaptureClient> capture;
            check(client->GetService(IID_PPV_ARGS(&capture)), "Audio capture service");
            check(client->Start(), "Start audio");
            while (!token.stop_requested()) {
                WaitForSingleObject(event, 50);
                UINT32 count = 0;
                check(capture->GetNextPacketSize(&count), "Audio packet size");
                while (count) {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD f = 0;
                    UINT64 pos = 0, qpc = 0;
                    check(capture->GetBuffer(&data, &frames, &f, &pos, &qpc), "Read audio packet");
                    AudioChunk chunk;
                    chunk.us = (f & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
                                   ? clockUs() - int64_t(frames) * 1000000 / 48000
                                   : int64_t(qpc / 10);
                    chunk.samples.resize(size_t(frames) * 2, 0);
                    if (!(f & AUDCLNT_BUFFERFLAGS_SILENT))
                        memcpy(chunk.samples.data(), data, chunk.samples.size() * sizeof(float));
                    if (f & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                        ++discontinuities;
                    capture->ReleaseBuffer(frames);
                    {
                        std::lock_guard lock(mutex_);
                        samples_ += chunk.samples.size();
                        queue_.push_back(std::move(chunk));
                        while (samples_ > 48000 * 4 && !queue_.empty()) {
                            samples_ -= queue_.front().samples.size();
                            queue_.pop_front();
                            ++discontinuities;
                        }
                    }
                    check(capture->GetNextPacketSize(&count), "Audio next packet");
                }
            }
            client->Stop();
        } catch (const std::exception& ex) {
            Log::get().write(std::string(input ? "Microphone: " : "Desktop audio: ") + ex.what());
            error(std::string(input ? "Microphone unavailable: " : "Desktop audio unavailable: ") +
                  ex.what());
        }
        if (mm)
            AvRevertMmThreadCharacteristics(mm);
        if (event)
            CloseHandle(event);
        if (SUCCEEDED(co))
            CoUninitialize();
    });
}
void AudioCapture::stop() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    std::lock_guard lock(mutex_);
    queue_.clear();
    samples_ = 0;
}
std::vector<AudioChunk> AudioCapture::drain() {
    std::lock_guard lock(mutex_);
    std::vector<AudioChunk> r;
    while (!queue_.empty()) {
        r.push_back(std::move(queue_.front()));
        queue_.pop_front();
    }
    samples_ = 0;
    return r;
}
void AudioTimeline::add(const AudioChunk& c, int64_t epoch) {
    int64_t start = (c.us - epoch) * 48000 / 1000000;
    for (size_t i = 0; i < c.samples.size() / 2; ++i) {
        int64_t sample = start + int64_t(i);
        if (sample < 0)
            continue;
        size_t slot = size_t(sample) % tags_.size();
        tags_[slot] = sample;
        data_[slot * 2] = c.samples[i * 2];
        data_[slot * 2 + 1] = c.samples[i * 2 + 1];
    }
}
void AudioTimeline::read(int64_t sample, int count, float gain, std::vector<float>& out) const {
    out.assign(size_t(count) * 2, 0);
    for (int i = 0; i < count; ++i) {
        int64_t s = sample + i;
        if (s < 0)
            continue;
        size_t slot = size_t(s) % tags_.size();
        if (tags_[slot] == s) {
            out[size_t(i) * 2] = data_[slot * 2] * gain;
            out[size_t(i) * 2 + 1] = data_[slot * 2 + 1] * gain;
        }
    }
}
} // namespace nco
