#include "diagnostics.hpp"
#include <fstream>
#include <iomanip>
#include <winternl.h>
namespace nco {
void exportDiagnostics(const std::filesystem::path& path, const Config& c,
                       const EngineStats& s, float dpi,
                       const std::vector<EncoderChoice>& encoders) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("Cannot create diagnostic report");
    out << std::fixed << std::setprecision(3);
    out << "NCO 0.4.0 diagnostic report\r\n"
           "No screen content or audio samples are included. Device identifiers, window titles and save paths are omitted.\r\n\r\n";
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    using VersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto versionFunction = reinterpret_cast<VersionFunction>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    if (versionFunction && versionFunction(&version) == 0)
        out << "Windows=" << version.dwMajorVersion << '.' << version.dwMinorVersion
            << " build=" << version.dwBuildNumber << "\r\n";
    MEMORYSTATUSEX memory{sizeof(memory)};
    GlobalMemoryStatusEx(&memory);
    out << "Logical processors=" << GetActiveProcessorCount(ALL_PROCESSOR_GROUPS)
        << " RAM_MiB=" << memory.ullTotalPhys / 1048576 << " UI_DPI=" << dpi << "\r\n";
    Com<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        for (UINT index = 0;; ++index) {
            Com<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            if (!adapter) continue;
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            LARGE_INTEGER driver{};
            const bool driverKnown = SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &driver));
            out << "Adapter[" << index << "]=" << utf8(d.Description)
                << " vendor=" << d.VendorId << " device=" << d.DeviceId
                << " dedicated_MiB=" << d.DedicatedVideoMemory / 1048576
                << " shared_MiB=" << d.SharedSystemMemory / 1048576;
            if (driverKnown)
                out << " driver=" << HIWORD(driver.HighPart) << '.' << LOWORD(driver.HighPart)
                    << '.' << HIWORD(driver.LowPart) << '.' << LOWORD(driver.LowPart);
            else
                out << " driver=unavailable_from_DXGI";
            out << "\r\n";
        }
    }
    out << "\r\nConfiguration\r\nFPS=" << c.fps << " resolution_mode=" << c.resolutionMode
        << " custom=" << c.width << 'x' << c.height << " quality=" << c.quality
        << " requested_bitrate_kbps=" << c.bitrateKbps << " requested_encoder=" << c.encoder
        << " container=" << c.format << " capture_backend=" << c.captureBackend
        << " auto_remux=" << c.remux << " replay=" << c.replay
        << " replay_seconds=" << c.replaySeconds << " preview=" << c.preview << "\r\n"
        << "Audio: mic_enabled=" << c.mic << " desktop_enabled=" << c.desktop
        << " mic_volume=" << c.micVolume << " mic_gain=" << c.micGain
        << " desktop_volume=" << c.desktopVolume << " separate_tracks=" << c.separateTracks
        << " offset_ms=" << c.audioOffsetMs << " custom_mic_device=" << !c.micDevice.empty()
        << " custom_desktop_device=" << !c.desktopDevice.empty() << "\r\n";
    out << "Available encoders:";
    for (const auto& encoder : encoders) out << ' ' << encoder.name;
    out << "\r\n\r\nCurrent pipeline\r\nrecording=" << s.recording << " paused=" << s.paused
        << " replay=" << s.replay << " selected_encoder=" << s.encoder
        << " encoder_adapter=" << s.encoderAdapter << " GPU_texture_path=" << s.gpuTexturePath
        << " output=" << s.output.w << 'x' << s.output.h
        << " fps=" << s.actualFps << " video_Mbps=" << s.bitrateMbps
        << " frames=" << s.frames << " dropped=" << s.dropped << " fresh_capture_frames=" << s.freshFrames
        << " encode_queue=" << s.encodeQueue << " encode_ms=" << s.encodeMs << " prepare_ms=" << s.prepareMs
        << " submit_ms=" << s.submitMs << " capture_ms=" << s.captureMs
        << " cpu_percent=" << s.cpuPercent << " process_MiB=" << s.workingSetMiB
        << " replay_MiB=" << s.replayBytes / 1048576.0 << "\r\n";
    out << "\r\nHow to interpret\r\n"
           "60 FPS needs a complete frame every 16.67 ms. High prepare_ms indicates GPU conversion/readback waits; high submit_ms indicates encoder backpressure.\r\n"
           "Capture and encoder adapters may differ on hybrid laptops. 'Hardware encoding with frame upload' is hardware encoding, with CPU transport.\r\n"
           "Actual bitrate can be low for a still desktop; output dimensions do not automatically decrease under load.\r\n"
           "Dropped frames are cumulative since pipeline restart. Audio discontinuities are WASAPI endpoint events, not proof of audible corruption.\r\n"
           "\r\nRecent timestamped log (bounded to 1 MiB)\r\n";
    out << Log::get().snapshot();
    out.flush();
    if (!out) throw std::runtime_error("Diagnostic report could not be fully written");
}
}
