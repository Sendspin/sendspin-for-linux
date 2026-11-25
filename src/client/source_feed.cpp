#include "source_feed.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

// write_audio() takes little-endian PCM and gets the samples as they are in memory
static_assert(std::endian::native == std::endian::little);

namespace {

constexpr uint32_t CHUNK_MS = 20;
constexpr size_t CHUNK_FRAMES = SendspinOutput::RATE * CHUNK_MS / 1000;
constexpr size_t CHUNK_SAMPLES = CHUNK_FRAMES * SendspinOutput::CHANNELS;

/// Uniform in [0, 1)
float next_uniform(uint32_t& state) {
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return static_cast<float>(state >> 8) * (1.0F / 16777216.0F);
}

}  // namespace

void to_s16_dithered(const float* in, int16_t* out, size_t count, uint32_t& state) {
    for (size_t i = 0; i < count; ++i) {
        if (in[i] == 0.0F) {
            out[i] = 0;
            continue;
        }
        const float dither = next_uniform(state) - next_uniform(state);
        const float scaled = std::floor(in[i] * 32768.0F + dither + 0.5F);
        out[i] = static_cast<int16_t>(std::clamp(scaled, -32768.0F, 32767.0F));
    }
}

sendspin::SourceRoleConfig SourceFeed::config() {
    sendspin::SourceRoleConfig config;
    config.codec = sendspin::SendspinCodecFormat::FLAC;
    config.sample_rate = SendspinOutput::RATE;
    config.channels = SendspinOutput::CHANNELS;
    config.bit_depth = 16;
    config.chunk_duration_ms = CHUNK_MS;
    config.line_sense = true;
    return config;
}

SourceFeed::SourceFeed(sendspin::SendspinClient& client) : role_(client.add_source(config())) {
    this->role_.set_listener(this);
    // The library reports no signal until one is set, and the spec asks for it always
    this->role_.set_signal(sendspin::SourceSignal::ABSENT);
}

void SourceFeed::write(const CapturedBlock& block) {
    // The output has one format; anything else could not go into the stream as announced
    if (block.rate != SendspinOutput::RATE) {
        return;
    }
    const bool silent = block.silent || this->silenced_.load(std::memory_order_relaxed);
    if (!block.silent) {
        this->last_sound_us_.store(sendspin::SendspinClient::now_us(), std::memory_order_relaxed);
    }
    if (!this->streaming_.load(std::memory_order_relaxed)) {
        return;
    }

    std::array<int16_t, CHUNK_SAMPLES> pcm{};
    const int64_t capture_time_us = block.capture_time_ns / 1000;
    for (size_t frame = 0; frame < block.frames; frame += CHUNK_FRAMES) {
        const size_t frames = std::min<size_t>(CHUNK_FRAMES, block.frames - frame);
        const size_t samples = frames * SendspinOutput::CHANNELS;
        if (!silent) {
            to_s16_dithered(block.samples + frame * SendspinOutput::CHANNELS, pcm.data(), samples,
                            this->dither_state_);
        }
        const int64_t offset_us = static_cast<int64_t>(frame) * 1'000'000 / SendspinOutput::RATE;
        // A refusal (stream just closed, or the buffer full after a stall) drops the audio
        this->role_.write_audio(reinterpret_cast<const uint8_t*>(pcm.data()),
                                samples * sizeof(int16_t), capture_time_us + offset_us);
    }
}

void SourceFeed::poll() {
    const int64_t last_sound_us = this->last_sound_us_.load(std::memory_order_relaxed);
    const bool signal = !this->silenced_.load(std::memory_order_relaxed) && last_sound_us != 0 &&
                        sendspin::SendspinClient::now_us() - last_sound_us < SIGNAL_HOLD_US;
    if (signal == this->signal_) {
        return;
    }
    this->signal_ = signal;
    this->role_.set_signal(signal ? sendspin::SourceSignal::PRESENT
                                  : sendspin::SourceSignal::ABSENT);
}

void SourceFeed::on_streaming_started() {
    this->streaming_.store(true, std::memory_order_relaxed);
}

void SourceFeed::on_streaming_stopped() {
    this->streaming_.store(false, std::memory_order_relaxed);
}
