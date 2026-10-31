#pragma once
#include <cmath>
#include <cstdint>

namespace HXAudioDsp {
struct BridgePeakStats {
    uint64_t overRange = 0, nonFinite = 0, protectedSamples = 0, hardClips = 0;
};
// Identity below -1.4 dBFS; C1 soft knee above it, asymptotic ceiling
// 0.98 leaves modest resampler headroom. No allocation or time-varying gain.
inline float BridgePeakGuard(float input, BridgePeakStats& stats) {
    if (!std::isfinite(input)) { ++stats.nonFinite; return 0.0f; }
    const double magnitude = std::abs(static_cast<double>(input));
    if (magnitude > 1.0) ++stats.overRange;
    if (magnitude <= 0.85) return input;
    ++stats.protectedSamples;
    const double output = 0.85 + 0.13 * std::tanh((magnitude - 0.85) / 0.13);
    const float result = static_cast<float>(std::copysign(output, input));
    if (std::abs(result) >= 1.0f) ++stats.hardClips;
    return result;
}
}
