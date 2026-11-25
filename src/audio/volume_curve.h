#pragma once

#include <cstdint>

/// A Sendspin volume is perceived loudness, 0–100: 50 should sound half as loud as 100. The gain
/// that does it is amplitude = (volume / 100)^1.5 (docs/SPEC.md, "Volume").

/// @brief The linear gain for a Sendspin volume; over 100 counts as 100
float volume_to_amplitude(uint8_t volume);

/// @brief The Sendspin volume nearest a linear gain; gains over 1 are 100
uint8_t amplitude_to_volume(float amplitude);
