// The stream lifecycle started from sendspin-cpp-cli's src/pipewire_sink.cpp at 715f988
// (Apache-2.0, https://github.com/Sendspin/sendspin-cpp-cli), by way of playback_stream.cpp.
// The timing is sendspin-mac's LocalPlayer and PlayoutBuffer (Apache-2.0,
// https://github.com/Sendspin/sendspin-mac).

#include "audio/play_along.h"

#include "app_id.h"
#include "audio/pipewire_util.h"

#include <QLoggingCategory>

#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <utility>

Q_LOGGING_CATEGORY(lc_play_along, "sendspin.playalong")

namespace {

constexpr const char* STREAM_NAME = "Sendspin play-along";
constexpr auto RETRY_INTERVAL = std::chrono::seconds(2);
constexpr int64_t NS_PER_S = 1'000'000'000;
constexpr size_t FRAME_BYTES = PlayAlong::CHANNELS * sizeof(float);

int64_t frames_ns(int64_t frames) {
    return frames * NS_PER_S / PlayAlong::RATE;
}

}  // namespace

PlayAlong::PlayAlong(QByteArray own_sink, QByteArray node_name)
    : own_sink_(std::move(own_sink)),
      node_name_(std::move(node_name)),
      queue_(QUEUE_FRAMES * CHANNELS) {
    pw_init(nullptr, nullptr);
}

PlayAlong::~PlayAlong() {
    this->close_();
    if (this->loop_ != nullptr) {
        pw_thread_loop_destroy(this->loop_);
    }
    pw_deinit();
}

void PlayAlong::set_target(const std::optional<QByteArray>& target) {
    const std::optional<QByteArray> wanted = target == this->own_sink_ ? std::nullopt : target;
    if (wanted == this->target_) {
        return;
    }
    this->target_ = wanted;
    this->retry_at_ = {};
    this->close_();
    // Another output's delay is its own
    this->delay_ns_.store(0);
    this->set_active(this->active_);
}

void PlayAlong::set_active(bool active) {
    this->active_ = active;
    if (!active) {
        this->close_();
        return;
    }
    if (this->stream_ != nullptr && this->failed_.load()) {
        this->close_();
    }
    if (this->stream_ == nullptr) {
        (void)this->open_();
    }
}

bool PlayAlong::is_open() const {
    return this->stream_ != nullptr && !this->failed_.load();
}

uint32_t PlayAlong::node_id() const {
    if (this->stream_ == nullptr) {
        return PW_ID_ANY;
    }
    const LoopLock lock(this->loop_);
    return pw_stream_get_node_id(this->stream_);
}

PlayAlong::Stats PlayAlong::stats() const {
    return {this->stat_error_ns_.load(std::memory_order_relaxed),
            this->stat_resyncs_.load(std::memory_order_relaxed),
            this->stat_dropped_.load(std::memory_order_relaxed),
            this->stat_repeated_.load(std::memory_order_relaxed),
            this->stat_underruns_.load(std::memory_order_relaxed),
            this->overflows_.load(std::memory_order_relaxed)};
}

bool PlayAlong::open_() {
    const auto now = std::chrono::steady_clock::now();
    if (!this->target_.has_value() || now < this->retry_at_) {
        return false;
    }
    this->retry_at_ = now + RETRY_INTERVAL;

    if (this->loop_ == nullptr) {
        this->loop_ = pw_thread_loop_new("sendspin-play-along", nullptr);
        if (this->loop_ == nullptr || pw_thread_loop_start(this->loop_) < 0) {
            qCWarning(lc_play_along, "cannot start a PipeWire loop");
            if (this->loop_ != nullptr) {
                pw_thread_loop_destroy(this->loop_);
                this->loop_ = nullptr;
            }
            return false;
        }
    }

    // An explicit target the session manager may not replace, as the player's stream has: left
    // to follow the default it would be moved into the Sendspin output once that is the
    // default, and play what was captured back into the capture.
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_APP_NAME, "Sendspin", PW_KEY_APP_ID, APP_ID, PW_KEY_NODE_NAME,
        this->node_name_.constData(), PW_KEY_TARGET_OBJECT, this->target_->constData(),
        PW_KEY_NODE_DONT_RECONNECT, "true", "node.dont-fallback", "true", "node.dont-move", "true",
        "state.restore-props", "false", "state.restore-target", "false", nullptr);
    if (props == nullptr) {
        return false;
    }

    uint8_t pod_storage[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_storage, sizeof(pod_storage));
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = RATE;
    info.channels = CHANNELS;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

    // Static: pw_stream_new_simple() keeps the pointer. Zeroed then assigned: the struct grows.
    static constexpr pw_stream_events events = [] {
        pw_stream_events table{};
        table.version = PW_VERSION_STREAM_EVENTS;
        table.state_changed = &PlayAlong::on_state_changed;
        table.process = &PlayAlong::on_process;
        return table;
    }();

    // No render() runs yet. What the queue still holds is late by now, and is dropped.
    this->failed_.store(false);
    this->corrector_.resync();

    int err = 0;
    {
        const LoopLock lock(this->loop_);
        // pw_stream_new_simple() consumes props even on failure
        this->stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(this->loop_), STREAM_NAME,
                                             props, &events, this);
        if (this->stream_ != nullptr) {
            const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                            PW_STREAM_FLAG_MAP_BUFFERS |
                                                            PW_STREAM_FLAG_RT_PROCESS);
            // Not waited for: the audio queues until the graph runs the stream
            err =
                pw_stream_connect(this->stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1);
        } else {
            err = -errno;
        }
    }
    if (err < 0) {
        qCWarning(lc_play_along, "cannot play along on %s: %s", this->target_->constData(),
                  spa_strerror(err));
        this->close_();
        return false;
    }
    qCInfo(lc_play_along, "playing along on %s", this->target_->constData());
    return true;
}

void PlayAlong::close_() {
    if (this->stream_ != nullptr) {
        const LoopLock lock(this->loop_);
        // Disconnect before destroy, under the loop lock: disconnect waits out on_process()
        pw_stream_disconnect(this->stream_);
        pw_stream_destroy(this->stream_);
        this->stream_ = nullptr;
    }
    this->failed_.store(false);
}

// ==============================================================================
// The queue
// ==============================================================================

bool PlayAlong::write(const float* samples, uint32_t frames, int64_t due_ns) {
    const uint64_t written = this->written_.load(std::memory_order_relaxed);
    const uint64_t marks = this->marks_written_.load(std::memory_order_relaxed);
    const uint64_t queued = written - this->read_.load(std::memory_order_acquire);
    const uint64_t marked = marks - this->marks_read_.load(std::memory_order_acquire);
    if (frames == 0 || queued + frames > QUEUE_FRAMES || marked >= QUEUE_BLOCKS) {
        this->overflows_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    this->marks_[marks % QUEUE_BLOCKS] = {written, due_ns};
    this->marks_written_.store(marks + 1, std::memory_order_release);

    const size_t at = written % QUEUE_FRAMES;
    const size_t first = std::min<size_t>(frames, QUEUE_FRAMES - at);
    std::copy_n(samples, first * CHANNELS, this->queue_.data() + (at * CHANNELS));
    std::copy_n(samples + (first * CHANNELS), (frames - first) * CHANNELS, this->queue_.data());
    this->written_.store(written + frames, std::memory_order_release);
    return true;
}

void PlayAlong::copy_out_(float* out, uint64_t index, uint32_t frames) const {
    const size_t at = index % QUEUE_FRAMES;
    const size_t first = std::min<size_t>(frames, QUEUE_FRAMES - at);
    std::copy_n(this->queue_.data() + (at * CHANNELS), first * CHANNELS, out);
    std::copy_n(this->queue_.data(), (frames - first) * CHANNELS, out + (first * CHANNELS));
}

int64_t PlayAlong::due_of_(uint64_t index) const {
    return this->mark_.due_ns + frames_ns(static_cast<int64_t>(index - this->mark_.index));
}

void PlayAlong::follow_marks_(uint64_t index) {
    uint64_t marks = this->marks_read_.load(std::memory_order_relaxed);
    const uint64_t marks_written = this->marks_written_.load(std::memory_order_acquire);
    while (marks != marks_written) {
        const Mark next = this->marks_[marks % QUEUE_BLOCKS];
        if (next.index > index) {
            break;
        }
        // The timeline jumped: a new run, or the speaker buffer changed
        if (!this->marked_ ||
            std::abs(next.due_ns - this->due_of_(next.index)) > DriftCorrector::RESYNC_NS) {
            this->corrector_.resync();
        }
        this->mark_ = next;
        this->marked_ = true;
        marks++;
    }
    this->marks_read_.store(marks, std::memory_order_release);
}

void PlayAlong::render(float* out, uint32_t frames, int64_t heard_ns) {
    uint64_t read = this->read_.load(std::memory_order_relaxed);
    const uint64_t written = this->written_.load(std::memory_order_acquire);

    DriftCorrector::Step step;
    while (true) {
        this->follow_marks_(read);
        const auto queued = static_cast<uint32_t>(written - read);
        if (queued == 0) {
            break;
        }
        const bool resyncing = this->corrector_.resyncing();
        step = this->corrector_.next(heard_ns - this->due_of_(read), frames, queued);
        // Lateness is counted on this block's timeline, which the next block need not continue
        const uint64_t marks = this->marks_read_.load(std::memory_order_relaxed);
        if (resyncing && marks != this->marks_written_.load(std::memory_order_acquire)) {
            const uint64_t block_end = this->marks_[marks % QUEUE_BLOCKS].index;
            if (read + step.drop >= block_end) {
                read = block_end;
                step = {};
                this->corrector_.resync();
                continue;
            }
        }
        break;
    }
    read += step.drop;

    uint32_t done = std::min(step.silence, frames);
    std::fill_n(out, static_cast<size_t>(done) * CHANNELS, 0.0F);
    if (step.repeat && done < frames) {
        this->copy_out_(out + (static_cast<size_t>(done) * CHANNELS), read, 1);
        done++;
    }
    const uint32_t played = std::min(frames - done, static_cast<uint32_t>(written - read));
    this->copy_out_(out + (static_cast<size_t>(done) * CHANNELS), read, played);
    read += played;
    done += played;
    if (done < frames) {
        std::fill_n(out + (static_cast<size_t>(done) * CHANNELS),
                    static_cast<size_t>(frames - done) * CHANNELS, 0.0F);
        // Ran dry while playing; a wait for the first audio, or for audio in time, is none
        if (this->marked_ && !this->corrector_.resyncing()) {
            this->underruns_++;
            this->corrector_.resync();
        }
    }
    this->read_.store(read, std::memory_order_release);

    const float gain = this->gain_.load(std::memory_order_relaxed);
    if (gain != 1.0F || this->ramped_gain_ != 1.0F) {
        constexpr float STEP = 1.0F / static_cast<float>(RAMP_FRAMES);
        for (uint32_t i = 0; i < frames; i++) {
            this->ramped_gain_ += std::clamp(gain - this->ramped_gain_, -STEP, STEP);
            for (uint32_t channel = 0; channel < CHANNELS; channel++) {
                out[(static_cast<size_t>(i) * CHANNELS) + channel] *= this->ramped_gain_;
            }
        }
    }

    const DriftCorrector::Counters& counters = this->corrector_.counters();
    this->stat_error_ns_.store(this->corrector_.smoothed_error_ns(), std::memory_order_relaxed);
    this->stat_resyncs_.store(counters.resyncs, std::memory_order_relaxed);
    this->stat_dropped_.store(counters.dropped, std::memory_order_relaxed);
    this->stat_repeated_.store(counters.repeated, std::memory_order_relaxed);
    this->stat_underruns_.store(this->underruns_, std::memory_order_relaxed);
}

// ==============================================================================
// PipeWire
// ==============================================================================

void PlayAlong::on_state_changed(void* data, pw_stream_state /*old*/, pw_stream_state state,
                                 const char* /*error*/) {
    auto* self = static_cast<PlayAlong*>(data);
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
        // Recorded only: the next set_active() opens another
        self->failed_.store(true);
    }
}

void PlayAlong::on_process(void* data) {
    auto* self = static_cast<PlayAlong*>(data);

    // Safe unlocked: pw_stream_disconnect() waits for this callback to return
    pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream_);
    if (buffer == nullptr) {
        return;
    }
    spa_data& chunk_data = buffer->buffer->datas[0];
    auto* dest = static_cast<float*>(chunk_data.data);
    if (dest == nullptr) {
        pw_stream_queue_buffer(self->stream_, buffer);
        return;
    }
    size_t frames = chunk_data.maxsize / FRAME_BYTES;
    if (buffer->requested != 0) {
        frames = std::min<size_t>(frames, buffer->requested);
    }

    // Read before queueing, which changes what the stream holds
    if (const std::optional<int64_t> heard = heard_at_ns(self->stream_, RATE)) {
        self->delay_ns_.store(
            *heard - clock_ns(CLOCK_MONOTONIC_RAW) + frames_ns(static_cast<int64_t>(frames)),
            std::memory_order_relaxed);
        self->render(dest, static_cast<uint32_t>(frames), *heard);
    } else {
        // No telling when this cycle is heard: the audio waits for one that says
        std::fill_n(dest, frames * CHANNELS, 0.0F);
    }

    chunk_data.chunk->offset = 0;
    chunk_data.chunk->stride = static_cast<int32_t>(FRAME_BYTES);
    chunk_data.chunk->size = static_cast<uint32_t>(frames * FRAME_BYTES);
    // In frames: pw_time::queued sums this field
    buffer->size = frames;
    pw_stream_queue_buffer(self->stream_, buffer);
}
