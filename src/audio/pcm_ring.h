// From sendspin-cpp-cli's src/pcm_ring.h at 715f988 (Apache-2.0,
// https://github.com/Sendspin/sendspin-cpp-cli).

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

/// Lock-free single-producer/single-consumer byte ring bridging write()'s push to PipeWire's pull.
/// The producer owns write_pos_ and the consumer read_pos_; nothing else may write either.
class PcmRing {
public:
    /// Producer side.
    /// @return Bytes written, short of `len` when the ring is nearly full.
    size_t write(const uint8_t* data, size_t len);

    /// Consumer side. Zero-fills any shortfall.
    /// @return Bytes of real audio read, not counting the padding.
    size_t read(uint8_t* dest, size_t len);

    size_t available() const;

    size_t free_space() const;

    /// Only with no reader running and the producer's mutex held.
    void drop();

    /// Drops everything in it too. Same restriction as drop().
    void reset(size_t capacity);

private:
    std::vector<uint8_t> buffer_;
    size_t capacity_{0};
    std::atomic<size_t> write_pos_{0};
    std::atomic<size_t> read_pos_{0};
};
