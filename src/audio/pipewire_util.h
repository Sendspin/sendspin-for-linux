#pragma once

#include <pipewire/pipewire.h>

#include <cstdint>
#include <ctime>
#include <optional>

/// Holds the thread-loop lock for a scope; it does not hold off the realtime thread.
class LoopLock {
public:
    explicit LoopLock(pw_thread_loop* loop) : loop_(loop) {
        pw_thread_loop_lock(this->loop_);
    }
    ~LoopLock() {
        pw_thread_loop_unlock(this->loop_);
    }
    LoopLock(const LoopLock&) = delete;
    LoopLock& operator=(const LoopLock&) = delete;

private:
    pw_thread_loop* loop_;
};

/// PipeWire stamps in CLOCK_MONOTONIC; the Sendspin libraries count CLOCK_MONOTONIC_RAW
inline int64_t clock_ns(clockid_t clock) {
    timespec ts{};
    clock_gettime(clock, &ts);
    return (static_cast<int64_t>(ts.tv_sec) * 1'000'000'000) + ts.tv_nsec;
}

/// When the first frame of the buffer a playback stream's process callback is about to queue is
/// heard, in CLOCK_MONOTONIC_RAW nanoseconds; nullopt in a cycle the graph gives no timing for.
/// Call it before queueing the buffer.
/// @param rate the stream's own, which counts what it still holds
inline std::optional<int64_t> heard_at_ns(pw_stream* stream, uint32_t rate) {
    pw_time time{};
    if (pw_stream_get_time_n(stream, &time, sizeof(time)) != 0 || time.now == 0 ||
        time.rate.denom == 0 || rate == 0) {
        return std::nullopt;
    }
    // `delay` is how long after `now`, the start of this graph cycle on CLOCK_MONOTONIC, the
    // buffer is heard, behind what the stream still holds. The cycle's age carries onto the raw
    // clock: over milliseconds the two do not differ.
    const int64_t cycle_age = clock_ns(CLOCK_MONOTONIC) - time.now;
    const double delay_ns = static_cast<double>(time.delay) * 1e9 *
                            static_cast<double>(time.rate.num) /
                            static_cast<double>(time.rate.denom);
    const double held_ns =
        static_cast<double>(time.queued + time.buffered) * 1e9 / static_cast<double>(rate);
    return clock_ns(CLOCK_MONOTONIC_RAW) - cycle_age + static_cast<int64_t>(delay_ns + held_ns);
}
