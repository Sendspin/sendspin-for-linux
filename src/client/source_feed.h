#pragma once

#include "audio/sendspin_output.h"

#include <sendspin/client.h>
#include <sendspin/source_role.h>

#include <atomic>
#include <cstdint>

/// @brief The client's source role: what apps play to the Sendspin output, offered to the server
/// as an input. The server opens and closes the stream; this feeds it while it is open.
class SourceFeed final : private sendspin::SourceRoleListener {
public:
    /// How long after the last sound the input still counts as carrying a signal
    static constexpr int64_t SIGNAL_HOLD_US = 3'000'000;

    /// FLAC, 16-bit, in the Sendspin output's rate and channels, 20 ms chunks, line sense on
    static sendspin::SourceRoleConfig config();

    /// @brief Adds the role to client. Before client.start(); client outlives this.
    explicit SourceFeed(sendspin::SendspinClient& client);

    /// @brief Takes one captured block. PipeWire's realtime thread, and only that one: no locks,
    /// allocation or blocking calls. Must have stopped before the client is destroyed.
    void write(const CapturedBlock& block);

    /// @brief While set, the server gets silence and no signal: the PC's sound is going
    /// somewhere else. The stream stays open. Any thread.
    void set_silenced(bool silenced) {
        this->silenced_.store(silenced, std::memory_order_relaxed);
    }

    /// @brief Reports the signal to the server when it changed. Main loop, every tick.
    void poll();

    /// @brief The server has the stream open. Any thread.
    bool streaming() const {
        return this->streaming_.load(std::memory_order_relaxed);
    }

    /// @brief There was sound in the last SIGNAL_HOLD_US, and it is not silenced. Main loop.
    bool signal() const {
        return this->signal_;
    }

private:
    void on_streaming_started() override;
    void on_streaming_stopped() override;

    sendspin::SourceRole& role_;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> silenced_{false};
    /// Client-clock time of the last block with sound in it; 0 for none yet
    std::atomic<int64_t> last_sound_us_{0};
    bool signal_{false};
    uint32_t dither_state_{0x9E3779B9};  ///< realtime thread only
};

/// @brief Float samples to 16-bit with TPDF dither. Digital silence stays digital silence.
/// @param state The dither generator's, kept between calls; never 0.
void to_s16_dithered(const float* in, int16_t* out, size_t count, uint32_t& state);
