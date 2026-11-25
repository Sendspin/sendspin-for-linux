#pragma once

#include "audio/drift_corrector.h"
#include "audio/sendspin_output.h"

#include <QByteArray>

#include <pipewire/pipewire.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

/// @brief This PC's own output playing along: a PipeWire playback stream that plays what the
/// Sendspin output captured, each frame at the time it is due (docs/SPEC.md, "Playing along on
/// this PC"). It is no Sendspin player and makes no connection; the server's clock is this
/// machine's, so all there is to follow is the output's delay and the drift of its clock.
///
/// Three threads: write() is the capture thread's, render() PipeWire's for this stream, and the
/// rest the main thread's. The queue between the first two is lock-free.
class PlayAlong {
public:
    static constexpr uint32_t CHANNELS = SendspinOutput::CHANNELS;
    static constexpr uint32_t RATE = SendspinOutput::RATE;
    /// What the queue holds: more than any speaker buffer
    static constexpr size_t QUEUE_FRAMES = 4 * static_cast<size_t>(RATE);
    /// Blocks the queue holds, each with its time
    static constexpr size_t QUEUE_BLOCKS = 1024;
    /// How long a change of gain takes
    static constexpr uint32_t RAMP_FRAMES = RATE / 100;

    struct Stats {
        /// How late the output plays, smoothed; negative for early
        int64_t error_ns{0};
        uint32_t resyncs{0};
        uint32_t dropped{0};
        uint32_t repeated{0};
        uint32_t underruns{0};
        /// Blocks there was no room for
        uint32_t overflows{0};
    };

    /// @param own_sink The Sendspin output's node.name, which is never played on
    /// @param node_name The stream's `node.name`; tests pass their own
    explicit PlayAlong(QByteArray own_sink = "sendspin",
                       QByteArray node_name = "sendspin-play-along");
    ~PlayAlong();
    PlayAlong(const PlayAlong&) = delete;
    PlayAlong& operator=(const PlayAlong&) = delete;

    /// @brief Sets the output to play on, a node.name; nullopt for nowhere. The Sendspin output
    /// counts as nowhere: what it captured would come back in (docs/SPEC.md, "Loop guards").
    void set_target(const std::optional<QByteArray>& target);
    const std::optional<QByteArray>& target() const {
        return this->target_;
    }

    /// @brief Opens the stream, or closes it and leaves the output to idle. Call it again while
    /// active: a stream the graph dropped is opened again.
    void set_active(bool active);

    /// @brief The gain applied to what is played; a change is ramped
    void set_gain(float gain) {
        this->gain_.store(gain, std::memory_order_relaxed);
    }
    float gain() const {
        return this->gain_.load(std::memory_order_relaxed);
    }

    /// @brief A stream is in the graph
    bool is_open() const;
    /// @brief The open stream's node id, or PW_ID_ANY
    uint32_t node_id() const;

    /// @brief How long before it is due a frame has to be queued: the output's delay and one
    /// cycle. 0 until the stream has run.
    int64_t delay_ns() const {
        return this->delay_ns_.load(std::memory_order_relaxed);
    }
    /// @brief Has delay_ns() measured again by the next stream that runs
    void forget_delay() {
        this->delay_ns_.store(0, std::memory_order_relaxed);
    }

    Stats stats() const;

    /// @brief Frames written and not yet played or dropped
    uint32_t queued_frames() const {
        // In this order: read_ only grows towards written_
        const uint64_t read = this->read_.load();
        return static_cast<uint32_t>(this->written_.load() - read);
    }

    /// @brief Queues a block. Capture thread: no locks, allocation or blocking calls.
    /// @param due_ns when its first frame is to be heard, CLOCK_MONOTONIC_RAW nanoseconds
    /// @return false when there was no room; the block is dropped
    bool write(const float* samples, uint32_t frames, int64_t due_ns);

    /// @brief Fills one cycle of output. The stream's realtime thread; public for the tests.
    /// @param heard_ns when the cycle's first frame is heard, on write()'s clock
    void render(float* out, uint32_t frames, int64_t heard_ns);

private:
    struct Mark {
        /// The block's first frame, counted from the first ever written
        uint64_t index;
        int64_t due_ns;
    };

    static void on_state_changed(void* data, pw_stream_state old, pw_stream_state state,
                                 const char* error);
    static void on_process(void* data);

    bool open_();
    void close_();
    /// render() only: the time the frame at index is due, by the block it is in
    int64_t due_of_(uint64_t index) const;
    /// render() only: moves mark_ on to the block that holds the frame at index
    void follow_marks_(uint64_t index);
    void copy_out_(float* out, uint64_t index, uint32_t frames) const;

    const QByteArray own_sink_;
    const QByteArray node_name_;

    // Main thread
    pw_thread_loop* loop_{nullptr};
    pw_stream* stream_{nullptr};
    std::optional<QByteArray> target_;
    bool active_{false};
    /// No sooner than this is a failed open tried again
    std::chrono::steady_clock::time_point retry_at_;
    /// The graph errored or dropped the stream
    std::atomic<bool> failed_{false};

    // The queue: write() owns written_ and marks_written_, render() read_ and marks_read_
    std::vector<float> queue_;
    std::atomic<uint64_t> written_{0};
    std::atomic<uint64_t> read_{0};
    std::array<Mark, QUEUE_BLOCKS> marks_{};
    std::atomic<uint64_t> marks_written_{0};
    std::atomic<uint64_t> marks_read_{0};

    // render() only
    DriftCorrector corrector_{RATE};
    /// The block being played; index 0 and no time until the first
    Mark mark_{0, 0};
    bool marked_{false};
    float ramped_gain_{1.0F};
    uint32_t underruns_{0};

    std::atomic<float> gain_{1.0F};
    std::atomic<int64_t> delay_ns_{0};
    std::atomic<uint32_t> overflows_{0};
    // What render() last saw, for stats()
    std::atomic<int64_t> stat_error_ns_{0};
    std::atomic<uint32_t> stat_resyncs_{0};
    std::atomic<uint32_t> stat_dropped_{0};
    std::atomic<uint32_t> stat_repeated_{0};
    std::atomic<uint32_t> stat_underruns_{0};
};
