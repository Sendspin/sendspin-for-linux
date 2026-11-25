#include "cast/cast_feed.h"

#include <thread>
#include <utility>

using sendspin::server::GroupInput;
using sendspin::server::InputState;
using sendspin::server::WriteResult;

CastFeed::~CastFeed() {
    this->detach();
}

void CastFeed::take(const CapturedBlock& block) {
    // Sequentially consistent with detach(): either this sees no input, or detach() sees
    // in_take_ and waits
    this->in_take_.store(true);
    GroupInput* input = this->rt_input_.load();
    if (input != nullptr) {
        if (!block.silent) {
            this->last_sound_ns_.store(block.capture_time_ns);
            this->streaming_.store(true);
        }
        // Silence inside a run is written, to keep its timeline; silence outside one is not,
        // or it would start a run
        if (this->streaming_.load()) {
            const WriteResult result = input->write(block.samples, block.frames);
            // Short with OPEN, the input played its latency down: the rest plays within what
            // it staged
            const size_t taken = result.state == InputState::OPEN ? block.frames : result.frames;
            this->written_frames_.fetch_add(taken, std::memory_order_relaxed);
            this->dropped_frames_.fetch_add(block.frames - taken, std::memory_order_relaxed);
            if (result.state == InputState::STOP_LATCHED) {
                this->stop_latched_.store(true);
            } else if (result.state == InputState::CLOSED) {
                this->closed_.store(true);
            }
        }
    }
    this->in_take_.store(false);
}

void CastFeed::attach(std::shared_ptr<GroupInput> input) {
    this->detach();
    this->input_ = std::move(input);
    this->rt_input_.store(this->input_.get());
}

void CastFeed::detach() {
    this->rt_input_.store(nullptr);
    while (this->in_take_.load()) {
        std::this_thread::yield();
    }
    if (this->input_) {
        this->input_->stop();
        this->input_.reset();
    }
    this->streaming_.store(false);
    this->stop_latched_.store(false);
    this->closed_.store(false);
}

CastFeed::State CastFeed::poll(int64_t now_ns) {
    if (!this->input_) {
        return State::OK;
    }
    if (this->closed_.load() || this->input_->state() == InputState::CLOSED) {
        return State::CLOSED;
    }
    const bool latched =
        this->stop_latched_.exchange(false) || this->input_->state() == InputState::STOP_LATCHED;
    if (latched || (this->streaming_.load() && !this->has_sound(now_ns))) {
        // A block with sound that arrives right now either starts the next run or loses its
        // first samples to this stop; the poll after it sees the run it left open
        this->streaming_.store(false);
        this->input_->stop();
    }
    return State::OK;
}

bool CastFeed::has_sound(int64_t now_ns) const {
    const int64_t last_ns = this->last_sound_ns_.load();
    return last_ns != 0 && now_ns - last_ns < IDLE_NS;
}
