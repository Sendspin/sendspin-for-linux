#include "audio/player_formats.h"

#include <algorithm>
#include <charconv>

bool SinkFormats::plays(uint32_t rate) const {
    if (!this->known()) {
        return true;
    }
    return (rate >= this->rate_min && rate <= this->rate_max && this->rate_max != 0) ||
           std::find(this->rates.begin(), this->rates.end(), rate) != this->rates.end();
}

std::vector<uint32_t> parse_rates(std::string_view value) {
    std::vector<uint32_t> rates;
    const char* at = value.data();
    const char* const end = at + value.size();
    while (at < end) {
        uint32_t rate = 0;
        const auto [next, error] = std::from_chars(at, end, rate);
        if (error == std::errc() && rate != 0) {
            rates.push_back(rate);
        }
        // Past the number, or past the bracket, comma or space that was not one
        at = next > at ? next : at + 1;
    }
    return rates;
}

std::vector<uint32_t> native_rates(const SinkFormats& sink, const ClockRates& clock) {
    if (clock.forced != 0) {
        return {clock.forced};
    }
    std::vector<uint32_t> rates;
    const auto add = [&](uint32_t rate) {
        if (rate != 0 && sink.plays(rate) &&
            std::find(rates.begin(), rates.end(), rate) == rates.end()) {
            rates.push_back(rate);
        }
    };
    add(clock.rate);
    std::vector<uint32_t> others = clock.allowed;
    std::sort(others.begin(), others.end());
    std::for_each(others.begin(), others.end(), add);
    if (rates.empty()) {
        // Whatever plays is resampled; the graph's rate costs it the least
        rates.push_back(clock.rate);
    }
    return rates;
}

std::vector<sendspin::AudioSupportedFormatObject> player_formats(const SinkFormats& sink,
                                                                 const ClockRates& clock) {
    constexpr uint8_t CHANNELS = 2;
    const std::vector<uint32_t> rates = native_rates(sink, clock);
    std::vector<sendspin::AudioSupportedFormatObject> formats;
    for (const auto codec :
         {sendspin::SendspinCodecFormat::FLAC, sendspin::SendspinCodecFormat::PCM}) {
        for (const uint32_t rate : rates) {
            if (sink.over_16_bit) {
                formats.push_back({codec, CHANNELS, rate, 24});
            }
            formats.push_back({codec, CHANNELS, rate, 16});
        }
    }
    return formats;
}
