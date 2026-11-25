#pragma once

#include <sendspin/config.h>

#include <cstdint>
#include <string_view>
#include <vector>

/// @brief What an output's EnumFormat offers. Empty when it could not be asked.
struct SinkFormats {
    std::vector<uint32_t> rates{};  ///< the rates it names
    uint32_t rate_min{0};           ///< the range it gives, both 0 for none
    uint32_t rate_max{0};
    bool over_16_bit{false};  ///< it has a sample format that carries more than 16 bits

    bool known() const {
        return !this->rates.empty() || this->rate_max != 0;
    }
    bool plays(uint32_t rate) const;
};

/// @brief The rates the PipeWire graph may run at: the daemon's `clock.*` settings
struct ClockRates {
    uint32_t rate{48000};             ///< clock.rate, what an idle graph runs at
    std::vector<uint32_t> allowed{};  ///< clock.allowed-rates
    uint32_t forced{0};               ///< clock.force-rate, 0 for none
};

/// @brief The rates in a `clock.allowed-rates` value, "[ 44100 48000 ]"
std::vector<uint32_t> parse_rates(std::string_view value);

/// @brief The rates that reach the output without a resampler: the ones the graph may switch to
/// that the output also plays. The graph's own rate first; never empty.
std::vector<uint32_t> native_rates(const SinkFormats& sink, const ClockRates& clock);

/// @brief What the player role advertises: stereo FLAC, then PCM, at native_rates(), 24-bit
/// ahead of 16-bit where the output carries it. The first is what a server prefers.
std::vector<sendspin::AudioSupportedFormatObject> player_formats(const SinkFormats& sink,
                                                                 const ClockRates& clock);
