// From sendspin-cpp-cli's src/pcm_ring.cpp at 715f988 (Apache-2.0,
// https://github.com/Sendspin/sendspin-cpp-cli).

#include "audio/pcm_ring.h"

#include <algorithm>
#include <cstring>

size_t PcmRing::write(const uint8_t* data, size_t len) {
    if (this->capacity_ == 0) {
        return 0;
    }

    const size_t write_pos = this->write_pos_.load(std::memory_order_relaxed);
    const size_t read_pos = this->read_pos_.load(std::memory_order_acquire);

    // One byte stays spare: it tells a full ring from an empty one.
    const size_t used = (write_pos - read_pos + this->capacity_) % this->capacity_;
    const size_t to_write = std::min(len, this->capacity_ - 1 - used);
    if (to_write == 0) {
        return 0;
    }

    const size_t first_chunk = std::min(to_write, this->capacity_ - write_pos);
    std::memcpy(&this->buffer_[write_pos], data, first_chunk);
    if (to_write > first_chunk) {
        std::memcpy(&this->buffer_[0], data + first_chunk, to_write - first_chunk);
    }

    this->write_pos_.store((write_pos + to_write) % this->capacity_, std::memory_order_release);
    return to_write;
}

size_t PcmRing::read(uint8_t* dest, size_t len) {
    if (this->capacity_ == 0) {
        std::memset(dest, 0, len);
        return 0;
    }

    const size_t read_pos = this->read_pos_.load(std::memory_order_relaxed);
    const size_t write_pos = this->write_pos_.load(std::memory_order_acquire);

    const size_t available = (write_pos - read_pos + this->capacity_) % this->capacity_;
    const size_t to_read = std::min(len, available);

    if (to_read > 0) {
        const size_t first_chunk = std::min(to_read, this->capacity_ - read_pos);
        std::memcpy(dest, &this->buffer_[read_pos], first_chunk);
        if (to_read > first_chunk) {
            std::memcpy(dest + first_chunk, &this->buffer_[0], to_read - first_chunk);
        }
        this->read_pos_.store((read_pos + to_read) % this->capacity_, std::memory_order_release);
    }

    if (to_read < len) {
        std::memset(dest + to_read, 0, len - to_read);
    }
    return to_read;
}

size_t PcmRing::available() const {
    if (this->capacity_ == 0) {
        return 0;
    }
    const size_t write_pos = this->write_pos_.load(std::memory_order_acquire);
    const size_t read_pos = this->read_pos_.load(std::memory_order_acquire);
    return (write_pos - read_pos + this->capacity_) % this->capacity_;
}

size_t PcmRing::free_space() const {
    if (this->capacity_ == 0) {
        return 0;
    }
    return this->capacity_ - 1 - this->available();
}

void PcmRing::drop() {
    this->read_pos_.store(0, std::memory_order_relaxed);
    this->write_pos_.store(0, std::memory_order_release);
}

void PcmRing::reset(size_t capacity) {
    this->buffer_.assign(capacity, 0);
    this->capacity_ = capacity;
    this->drop();
}
