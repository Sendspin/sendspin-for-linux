#pragma once

#include <cstdint>

/// @brief Keeps an output on a timeline its own clock drifts from: what to do with the frames
/// queued for it so that each is heard when it is due. After sendspin-mac's `PlayoutBuffer`,
/// which follows the player role's suggested correction (spec player/v1.md, "Sample deletion
/// and insertion").
///
/// In one shot at the start, after an underrun and once the error passes RESYNC_NS: drop what
/// is late, or wait in silence. Otherwise single frames are dropped or repeated while the error
/// is outside DEAD_BAND_NS, never more than one frame in CORRECTION_SPACING. The error is
/// smoothed first: the output's timestamps jitter by some tens of microseconds.
///
/// Plain logic, no audio: the caller carries out each Step. Not thread-safe.
class DriftCorrector {
public:
    /// Errors within this are left alone
    static constexpr int64_t DEAD_BAND_NS = 100'000;
    /// Errors beyond this are put right in one shot
    static constexpr int64_t RESYNC_NS = 1'000'000;
    /// How much of each cycle's error goes into the smoothed one: about 200 ms of cycles
    static constexpr double SMOOTHING = 0.05;
    /// Frames played between two single-frame corrections: 0.5 % of them at most
    static constexpr uint32_t CORRECTION_SPACING = 200;

    /// What to do at the start of one cycle, in this order
    struct Step {
        /// Queued frames to discard: they are late
        uint32_t drop{0};
        /// Frames of silence to play before the queue: it is early
        uint32_t silence{0};
        /// Play the next queued frame twice
        bool repeat{false};

        bool operator==(const Step&) const = default;
    };

    struct Counters {
        uint32_t resyncs{0};
        uint32_t dropped{0};
        uint32_t repeated{0};
    };

    explicit DriftCorrector(uint32_t rate);

    /// @brief The next cycle starts over: the start, an underrun, a jump in the timeline
    void resync();

    /// @brief A one-shot correction is due; next() carries it out once there is audio to place
    bool resyncing() const {
        return this->resyncing_;
    }

    /// @param error_ns how late the next queued frame would be: when the cycle's first frame is
    /// heard, less when that queued frame is due
    /// @param frames the cycle's length
    /// @param queued frames there are to play
    Step next(int64_t error_ns, uint32_t frames, uint32_t queued);

    /// @brief The error as corrections go by it
    int64_t smoothed_error_ns() const {
        return static_cast<int64_t>(this->smoothed_ns_);
    }

    const Counters& counters() const {
        return this->counters_;
    }

private:
    const double frame_ns_;
    bool resyncing_{true};
    double smoothed_ns_{0.0};
    uint64_t since_correction_{0};
    Counters counters_;
};
