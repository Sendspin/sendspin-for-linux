#include "cast/cast_feed.h"

#include "audio/pipewire_util.h"

#include <algorithm>
#include <cstdlib>
#include <thread>
#include <utility>

using sendspin::server::GroupInput;
using sendspin::server::InputState;
using sendspin::server::WriteResult;

namespace {

constexpr int64_t NS_PER_S = 1'000'000'000;
/// The least a block played directly is given beyond the output's delay, for a late callback
constexpr int64_t DIRECT_MARGIN_NS = 5'000'000;
/// A lead is kept while the one wanted is within this of it: each change is heard
constexpr int64_t DIRECT_STEP_NS = 2'000'000;

}  // namespace

CastFeed::~CastFeed() {
    this->detach();
}

void CastFeed::take(const CapturedBlock& block) {
    // Sequentially consistent with detach(): either this sees no input, or detach() sees
    // in_take_ and waits
    this->in_take_.store(true);
    GroupInput* input = this->rt_input_.load();
    if (!block.silent) {
        this->last_sound_ns_.store(block.capture_time_ns);
    }
    const Local local = this->play_along_ != nullptr ? this->local_.load() : Local::NOTHING;
    if (input != nullptr) {
        if (!block.silent) {
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
            if (local == Local::GROUP && result.frames != 0) {
                // The head is the end of what this write staged, and its latency says when that
                // plays. No count of positions to keep, which a stop from poll() could race.
                const int64_t end_ns = clock_ns(CLOCK_MONOTONIC_RAW) + (input->latency_us() * 1000);
                const int64_t staged_ns =
                    static_cast<int64_t>(result.frames) * NS_PER_S / block.rate;
                (void)this->play_along_->write(block.samples, static_cast<uint32_t>(taken),
                                               end_ns - staged_ns);
            }
        }
    } else if (local == Local::DIRECT && this->has_sound(block.capture_time_ns)) {
        // As soon as the output can: its delay, and a block for a callback that comes late
        const int64_t block_ns = static_cast<int64_t>(block.frames) * NS_PER_S / block.rate;
        const int64_t wanted = this->play_along_->delay_ns() + std::max(block_ns, DIRECT_MARGIN_NS);
        if (std::abs(wanted - this->direct_lead_ns_) > DIRECT_STEP_NS) {
            this->direct_lead_ns_ = wanted;
        }
        (void)this->play_along_->write(block.samples, block.frames,
                                       block.capture_time_ns + this->direct_lead_ns_);
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
