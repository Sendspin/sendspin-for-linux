#include "audio/drift_corrector.h"

#include <algorithm>
#include <cmath>

DriftCorrector::DriftCorrector(uint32_t rate) : frame_ns_(1e9 / static_cast<double>(rate)) {}

void DriftCorrector::resync() {
    this->resyncing_ = true;
}

DriftCorrector::Step DriftCorrector::next(int64_t error_ns, uint32_t frames, uint32_t queued) {
    Step step;
    if (queued == 0) {
        return step;
    }
    const auto error = static_cast<double>(error_ns);
    if (!this->resyncing_) {
        this->smoothed_ns_ += SMOOTHING * (error - this->smoothed_ns_);
        if (std::abs(this->smoothed_ns_) > static_cast<double>(RESYNC_NS)) {
            this->resyncing_ = true;
        }
    }

    if (this->resyncing_) {
        const double off = std::round(std::abs(error) / this->frame_ns_);
        if (error >= 0) {
            step.drop = static_cast<uint32_t>(std::min<double>(off, queued));
        } else {
            step.silence = static_cast<uint32_t>(std::min<double>(off, frames));
        }
        // With nothing left that is in time, or a cycle that is all waiting, the next one tries
        // again
        if (step.drop < queued && step.silence < frames) {
            this->resyncing_ = false;
            this->smoothed_ns_ = 0.0;
            this->since_correction_ = 0;
            this->counters_.resyncs++;
        }
    } else if (std::abs(this->smoothed_ns_) > static_cast<double>(DEAD_BAND_NS) &&
               this->since_correction_ >= CORRECTION_SPACING) {
        if (this->smoothed_ns_ > 0) {
            step.drop = 1;
            this->smoothed_ns_ -= this->frame_ns_;
            this->counters_.dropped++;
        } else {
            step.repeat = true;
            this->smoothed_ns_ += this->frame_ns_;
            this->counters_.repeated++;
        }
        this->since_correction_ = 0;
    }
    this->since_correction_ += frames;
    return step;
}
