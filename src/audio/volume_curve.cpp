#include "audio/volume_curve.h"

#include <algorithm>
#include <cmath>

float volume_to_amplitude(uint8_t volume) {
    return std::pow(static_cast<float>(std::min<uint8_t>(volume, 100)) / 100.0F, 1.5F);
}

uint8_t amplitude_to_volume(float amplitude) {
    const float clamped = std::clamp(amplitude, 0.0F, 1.0F);
    return static_cast<uint8_t>(std::lround(100.0F * std::pow(clamped, 1.0F / 1.5F)));
}
