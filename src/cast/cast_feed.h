#pragma once

#include "audio/sendspin_output.h"

#include <sendspin/server/server.h>

#include <atomic>
#include <cstdint>
#include <memory>

/// @brief Writes what the Sendspin output captures into the cast group's live input, and ends
/// the stream while there is nothing to hear.
///
/// take() runs on PipeWire's realtime thread and only writes; everything else is the main
/// thread's, which ends a run from poll() because GroupInput::stop() is not real-time safe.
class CastFeed {
public:
    /// Digital silence for this long ends the stream; the next sound starts a new one
    static constexpr int64_t IDLE_NS = 3'000'000'000;

    enum class State : uint8_t {
        OK,
        /// The server closed the input: release it with detach()
        CLOSED,
    };

    ~CastFeed();

    /// @brief Realtime thread. No locks, allocation or blocking calls.
    void take(const CapturedBlock& block);

    /// @brief Writes into input from the next block on; an input attached before is released
    void attach(std::shared_ptr<sendspin::server::GroupInput> input);

    /// @brief Stops and releases the input, once take() no longer uses it
    void detach();

    const std::shared_ptr<sendspin::server::GroupInput>& input() const {
        return this->input_;
    }

    /// @brief Ends the run after IDLE_NS without sound, which a stopped graph is too: it sends
    /// no blocks at all. Call it a few times a second.
    /// @param now_ns on the clock of CapturedBlock::capture_time_ns
    State poll(int64_t now_ns);

    /// @brief There was sound in the last IDLE_NS
    bool has_sound(int64_t now_ns) const;

    /// @brief A run is open: blocks are written, silent ones included
    bool streaming() const {
        return this->streaming_.load();
    }

    uint64_t written_frames() const {
        return this->written_frames_.load(std::memory_order_relaxed);
    }
    /// @brief Frames the input refused; a live input's writer drops them
    uint64_t dropped_frames() const {
        return this->dropped_frames_.load(std::memory_order_relaxed);
    }

private:
    std::shared_ptr<sendspin::server::GroupInput> input_;
    /// input_ for take(). With in_take_, detach() knows when the realtime thread has let go.
    std::atomic<sendspin::server::GroupInput*> rt_input_{nullptr};
    std::atomic<bool> in_take_{false};

    std::atomic<bool> streaming_{false};
    /// Capture time of the last block with sound; 0 for none yet
    std::atomic<int64_t> last_sound_ns_{0};
    /// What the last write reported and poll() acts on
    std::atomic<bool> stop_latched_{false};
    std::atomic<bool> closed_{false};
    std::atomic<uint64_t> written_frames_{0};
    std::atomic<uint64_t> dropped_frames_{0};
};
