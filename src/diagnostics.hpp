#pragma once
#include "engine.hpp"
namespace nco {
void exportDiagnostics(const std::filesystem::path& path, const Config& config,
                       const EngineStats& stats, float dpi,
                       const std::vector<EncoderChoice>& encoders);
}
