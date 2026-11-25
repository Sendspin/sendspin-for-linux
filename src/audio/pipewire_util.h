#pragma once

#include <pipewire/pipewire.h>

#include <cstdint>
#include <ctime>

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
