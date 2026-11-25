// Started from sendspin-cpp-cli's src/pipewire_sink.cpp at 715f988 (Apache-2.0,
// https://github.com/Sendspin/sendspin-cpp-cli): the ring between write() and the process
// callback, the stream lifecycle and the playout report. The volume is the stream's own here,
// not a gain applied to the samples.

#include "audio/playback_stream.h"

#include "app_id.h"
#include "audio/pipewire_util.h"

#include <QLoggingCategory>

#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

Q_LOGGING_CATEGORY(lc_playback, "sendspin.playback")

namespace {

constexpr const char* STREAM_NAME = "Sendspin";

/// What the ring holds; the graph is asked for a third of it per cycle
constexpr uint32_t RING_MS = 80;
constexpr size_t MIN_RING_FRAMES = 1024;
constexpr size_t RING_QUANTA = 3;

constexpr int OPEN_TIMEOUT_MS = 500;
constexpr auto RETRY_INTERVAL = std::chrono::seconds(2);
/// How long a new stream may stay silent waiting to take its volume
constexpr int64_t GATE_NS = 1'000'000'000;
/// How long the echo of a volume set from here is waited for
constexpr int64_t ECHO_NS = 1'000'000'000;
constexpr float GAIN_EPSILON = 1e-5F;

spa_audio_format sample_format_for(uint8_t bits) {
    switch (bits) {
        case 8:
            return SPA_AUDIO_FORMAT_S8;
        case 16:
            return SPA_AUDIO_FORMAT_S16_LE;
        case 24:
            return SPA_AUDIO_FORMAT_S24_LE;
        case 32:
            return SPA_AUDIO_FORMAT_S32_LE;
        default:
            return SPA_AUDIO_FORMAT_UNKNOWN;
    }
}

size_t ring_frames(uint32_t rate) {
    return std::max<size_t>(static_cast<size_t>(rate) * RING_MS / 1000, MIN_RING_FRAMES);
}

int64_t now_us() {
    return clock_ns(CLOCK_MONOTONIC_RAW) / 1000;
}

}  // namespace

PlaybackStream::PlaybackStream(QObject* parent, QByteArray node_name)
    : QObject(parent), node_name_(std::move(node_name)) {
    pw_init(nullptr, nullptr);
}

PlaybackStream::~PlaybackStream() {
    this->stop();
    if (this->loop_ != nullptr) {
        pw_thread_loop_destroy(this->loop_);
    }
    pw_deinit();
}

void PlaybackStream::set_played(Played played) {
    this->played_ = std::move(played);
}

void PlaybackStream::set_target(const std::optional<QByteArray>& target) {
    const std::lock_guard lock(this->mutex_);
    if (target == this->target_) {
        return;
    }
    this->target_ = target;
    this->retry_at_ = {};
    if (this->stream_ != nullptr) {
        // The next write() opens one on the new target
        this->abandon_();
    }
}

size_t PlaybackStream::write(const uint8_t* data, size_t length, const Format& format,
                             uint32_t timeout_ms) {
    const size_t stride = static_cast<size_t>(format.channels) * (format.bits / 8U);
    if (data == nullptr || stride == 0) {
        return 0;
    }
    const size_t usable = length - (length % stride);
    if (usable == 0) {
        return 0;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    std::unique_lock lock(this->mutex_);

    if (this->alive_() && format != this->format_) {
        // What is queued is in the old format: it plays out before the stream reopens
        const bool drained = this->room_.wait_until(lock, deadline, [this] {
            return this->ring_.available() == 0 || this->stopping_.load() || !this->alive_();
        });
        if (!drained) {
            return 0;
        }
    }
    if (!this->alive_() || format != this->format_) {
        if (this->stream_ != nullptr) {
            this->abandon_();
        }
        if (this->stopping_.load() || !this->open_(format)) {
            // Taken, not refused: returning 0 for ever would spin the player
            const auto frames =
                static_cast<uint32_t>(usable / stride) + this->owed_frames_.exchange(0);
            lock.unlock();
            if (this->played_) {
                this->played_(frames, now_us());
            }
            return usable;
        }
    }

    if (!this->gate_warned_ && clock_ns(CLOCK_MONOTONIC) >= this->gate_until_ns_.load() &&
        !(this->gain_applied_.load() && this->mute_applied_.load())) {
        this->gate_warned_ = true;
        qCWarning(lc_playback, "the stream does not take its volume; silent unless at full volume");
    }

    const uint64_t generation = this->generation_;
    size_t done = 0;
    while (done < usable && !this->stopping_.load()) {
        const size_t remaining = usable - done;
        size_t room = this->ring_.free_space();
        if (room < remaining) {
            // Whole frames only: the process callback counts frames by division
            room -= room % stride;
        }
        if (room > 0) {
            const size_t written = this->ring_.write(data + done, std::min(remaining, room));
            if (written == 0) {
                break;
            }
            done += written;
            continue;
        }
        const bool woken = this->room_.wait_until(lock, deadline, [this, stride, generation] {
            return this->ring_.free_space() >= stride || this->stopping_.load() ||
                   this->generation_ != generation;
        });
        // Out of time, or the stream changed while unlocked: report only what landed
        if (!woken || this->generation_ != generation) {
            break;
        }
    }
    return done;
}

void PlaybackStream::stop() {
    // Before the mutex, so a waiting write() gives up
    this->stopping_.store(true);
    this->room_.notify_all();
    const std::lock_guard lock(this->mutex_);
    this->close_();
    this->owed_frames_.store(0);
    this->retry_at_ = {};
    this->stopping_.store(false);
}

void PlaybackStream::set_gain(float gain) {
    const std::lock_guard lock(this->mutex_);
    this->gain_.store(std::max(gain, 0.0F));
    this->gain_sets_.fetch_add(1);
    if (this->stream_ != nullptr) {
        const LoopLock loop_lock(this->loop_);
        this->apply_gain_();
    }
}

void PlaybackStream::set_muted(bool muted) {
    const std::lock_guard lock(this->mutex_);
    this->muted_.store(muted);
    this->mute_sets_.fetch_add(1);
    if (this->stream_ != nullptr) {
        const LoopLock loop_lock(this->loop_);
        this->apply_mute_();
    }
}

bool PlaybackStream::is_open() const {
    const std::lock_guard lock(this->mutex_);
    return this->alive_();
}

uint32_t PlaybackStream::node_id() const {
    const std::lock_guard lock(this->mutex_);
    if (this->stream_ == nullptr) {
        return PW_ID_ANY;
    }
    const LoopLock loop_lock(this->loop_);
    return pw_stream_get_node_id(this->stream_);
}

bool PlaybackStream::open_(const Format& format) {
    const auto now = std::chrono::steady_clock::now();
    if (!this->target_.has_value() || now < this->retry_at_) {
        return false;
    }
    this->retry_at_ = now + RETRY_INTERVAL;

    const spa_audio_format sample_format = sample_format_for(format.bits);
    if (sample_format == SPA_AUDIO_FORMAT_UNKNOWN || format.channels == 0 || format.rate == 0 ||
        format.channels > SPA_AUDIO_MAX_CHANNELS) {
        qCWarning(lc_playback, "cannot play %u Hz, %u channels, %u-bit", format.rate,
                  format.channels, format.bits);
        return false;
    }
    if (this->loop_ == nullptr) {
        this->loop_ = pw_thread_loop_new("sendspin-player", nullptr);
        if (this->loop_ == nullptr || pw_thread_loop_start(this->loop_) < 0) {
            qCWarning(lc_playback, "cannot start a PipeWire loop");
            if (this->loop_ != nullptr) {
                pw_thread_loop_destroy(this->loop_);
                this->loop_ = nullptr;
            }
            return false;
        }
    }

    // The format and the ring change here, while no process callback can run
    ++this->generation_;
    this->format_ = format;
    this->bytes_per_frame_ = static_cast<size_t>(format.channels) * (format.bits / 8U);
    const size_t frames = ring_frames(format.rate);
    // One byte more: the ring keeps one spare to tell full from empty
    this->ring_.reset((frames * this->bytes_per_frame_) + 1);
    this->failed_.store(false);
    this->state_.store(PW_STREAM_STATE_UNCONNECTED);
    // The stream's controls appear after it connects; on_control_info() sets them then
    this->gain_applied_.store(false);
    this->mute_applied_.store(false);
    this->gain_echoes_.clear();
    this->mute_echoes_.clear();
    this->gate_until_ns_.store(clock_ns(CLOCK_MONOTONIC) + GATE_NS);
    this->gate_warned_ = false;

    // An explicit target, which the session manager may not replace: left to follow the default
    // it would be moved into the Sendspin output the moment that becomes the default, and what
    // a server plays here would come back in as this PC's input. node.dont-move keeps a mixer's
    // "move this stream" from doing the same. Nor does it restore a volume
    // or a target of its own: both are set from here.
    const std::string latency =
        std::to_string(frames / RING_QUANTA) + "/" + std::to_string(format.rate);
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_APP_NAME, STREAM_NAME, PW_KEY_APP_ID, APP_ID, PW_KEY_NODE_NAME,
        this->node_name_.constData(), PW_KEY_TARGET_OBJECT, this->target_->constData(),
        PW_KEY_NODE_DONT_RECONNECT, "true", "node.dont-fallback", "true", "node.dont-move", "true",
        "state.restore-props", "false", "state.restore-target", "false", PW_KEY_NODE_LATENCY,
        latency.c_str(), nullptr);
    if (props == nullptr) {
        return false;
    }

    uint8_t pod_storage[1024];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_storage, sizeof(pod_storage));
    spa_audio_info_raw info{};
    info.format = sample_format;
    info.rate = format.rate;
    info.channels = format.channels;
    if (format.channels == 1) {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    } else if (format.channels == 2) {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    }
    const spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

    // Static: pw_stream_new_simple() keeps the pointer. Zeroed then assigned: the struct grows.
    static constexpr pw_stream_events events = [] {
        pw_stream_events table{};
        table.version = PW_VERSION_STREAM_EVENTS;
        table.state_changed = &PlaybackStream::on_state_changed;
        table.control_info = &PlaybackStream::on_control_info;
        table.process = &PlaybackStream::on_process;
        return table;
    }();

    int err = 0;
    {
        const LoopLock loop_lock(this->loop_);
        // pw_stream_new_simple() consumes props even on failure
        this->stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(this->loop_), STREAM_NAME,
                                             props, &events, this);
        if (this->stream_ != nullptr) {
            const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                            PW_STREAM_FLAG_MAP_BUFFERS |
                                                            PW_STREAM_FLAG_RT_PROCESS);
            err =
                pw_stream_connect(this->stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, params, 1);
        } else {
            err = -errno;
        }
        if (err >= 0) {
            // PAUSED counts: the graph has the stream
            timespec deadline{};
            pw_thread_loop_get_time(this->loop_, &deadline,
                                    static_cast<int64_t>(OPEN_TIMEOUT_MS) * 1000 * 1000);
            while (true) {
                const int state = this->state_.load();
                if (state == PW_STREAM_STATE_STREAMING || state == PW_STREAM_STATE_PAUSED ||
                    state == PW_STREAM_STATE_ERROR) {
                    break;
                }
                if (pw_thread_loop_timed_wait_full(this->loop_, &deadline) != 0) {
                    break;
                }
            }
        }
    }

    const int state = this->state_.load();
    if (err < 0 || (state != PW_STREAM_STATE_STREAMING && state != PW_STREAM_STATE_PAUSED)) {
        qCWarning(lc_playback, "cannot play on %s: %s", this->target_->constData(),
                  err < 0 ? spa_strerror(err) : "PipeWire did not take the stream");
        this->close_();
        return false;
    }

    this->retry_at_ = {};
    qCInfo(lc_playback, "playing on %s at %u Hz, %u channels, %u-bit", this->target_->constData(),
           format.rate, format.channels, format.bits);
    return true;
}

void PlaybackStream::close_() {
    if (this->stream_ != nullptr) {
        const LoopLock loop_lock(this->loop_);
        // Disconnect before destroy, under the loop lock: disconnect waits out on_process()
        pw_stream_disconnect(this->stream_);
        pw_stream_destroy(this->stream_);
        this->stream_ = nullptr;
    }
    ++this->generation_;
    this->format_ = {};
    this->bytes_per_frame_ = 0;
    this->ring_.reset(0);
    this->failed_.store(false);
    this->state_.store(PW_STREAM_STATE_UNCONNECTED);
    // Wakes a write() waiting on the old stream
    this->room_.notify_all();
}

void PlaybackStream::abandon_() {
    if (this->bytes_per_frame_ != 0) {
        this->owed_frames_.fetch_add(
            static_cast<uint32_t>(this->ring_.available() / this->bytes_per_frame_));
    }
    this->close_();
}

bool PlaybackStream::alive_() const {
    return this->stream_ != nullptr && !this->failed_.load();
}

void PlaybackStream::apply_gain_() {
    float gains[SPA_AUDIO_MAX_CHANNELS];
    std::fill_n(gains, this->format_.channels, this->gain_.load());
    // A set that changes nothing has no echo to wait for
    if (this->gain_applied_.load() &&
        std::abs(gains[0] - this->reported_gain_.load()) < GAIN_EPSILON) {
        return;
    }
    this->gain_echoes_.sent(gains[0]);
    pw_stream_set_control(this->stream_, SPA_PROP_channelVolumes, this->format_.channels, gains, 0);
}

void PlaybackStream::apply_mute_() {
    float value = this->muted_.load() ? 1.0F : 0.0F;
    if (this->mute_applied_.load() && this->muted_.load() == this->reported_muted_.load()) {
        return;
    }
    this->mute_echoes_.sent(value);
    pw_stream_set_control(this->stream_, SPA_PROP_mute, 1, &value, 0);
}

void PlaybackStream::Echoes::sent(float value) {
    const std::lock_guard lock(this->mutex_);
    this->expire_();
    this->sent_.push_back({value, clock_ns(CLOCK_MONOTONIC)});
}

bool PlaybackStream::Echoes::take(float value) {
    const std::lock_guard lock(this->mutex_);
    this->expire_();
    // The newest match: PipeWire may report only the last of several sets
    for (size_t i = this->sent_.size(); i-- > 0;) {
        if (std::abs(this->sent_[i].value - value) < GAIN_EPSILON) {
            this->sent_.erase(this->sent_.begin(), this->sent_.begin() + static_cast<long>(i) + 1);
            return true;
        }
    }
    return false;
}

bool PlaybackStream::Echoes::awaited() {
    const std::lock_guard lock(this->mutex_);
    this->expire_();
    return !this->sent_.empty();
}

void PlaybackStream::Echoes::clear() {
    const std::lock_guard lock(this->mutex_);
    this->sent_.clear();
}

/// A set that changed nothing has no echo
void PlaybackStream::Echoes::expire_() {
    const int64_t oldest = clock_ns(CLOCK_MONOTONIC) - ECHO_NS;
    std::erase_if(this->sent_, [oldest](const Sent& sent) { return sent.at_ns < oldest; });
}

void PlaybackStream::on_state_changed(void* data, pw_stream_state /*old*/, pw_stream_state state,
                                      const char* /*error*/) {
    auto* self = static_cast<PlaybackStream*>(data);
    self->state_.store(state);
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
        // Recorded only: the next write() reopens
        self->failed_.store(true);
        self->room_.notify_all();
    }
    pw_thread_loop_signal(self->loop_, false);
}

void PlaybackStream::on_control_info(void* data, uint32_t id, const pw_stream_control* control) {
    auto* self = static_cast<PlaybackStream*>(data);
    if (id == SPA_PROP_channelVolumes) {
        // A newborn stream has none yet, and reports none until it is given some
        const bool reported = control->n_values != 0;
        float gain = 0.0F;
        for (uint32_t i = 0; i < control->n_values; i++) {
            gain += control->values[i] / static_cast<float>(control->n_values);
        }
        if (reported) {
            self->reported_gain_.store(gain);
        }
        // What is wanted is taken, echo or not: a set that changes nothing has no echo
        const bool echo = reported && self->gain_echoes_.take(gain);
        if (echo || (reported && std::abs(gain - self->gain_.load()) < GAIN_EPSILON)) {
            self->gain_applied_.store(true);
        } else if (!self->gain_applied_.load()) {
            // The volume it was born with
            self->apply_gain_();
        } else if (reported && !self->gain_echoes_.awaited()) {
            // The mixer's doing
            const uint32_t sets = self->gain_sets_.load();
            QMetaObject::invokeMethod(
                self,
                [self, gain, sets] {
                    if (self->gain_sets_.load() == sets) {
                        self->gain_.store(gain);
                        emit self->gainChanged(gain);
                    }
                },
                Qt::QueuedConnection);
        }
    } else if (id == SPA_PROP_mute && control->n_values != 0) {
        const bool muted = control->values[0] >= 0.5F;
        self->reported_muted_.store(muted);
        const bool echo = self->mute_echoes_.take(muted ? 1.0F : 0.0F);
        if (echo || muted == self->muted_.load()) {
            self->mute_applied_.store(true);
        } else if (!self->mute_applied_.load()) {
            self->apply_mute_();
        } else if (!self->mute_echoes_.awaited()) {
            const uint32_t sets = self->mute_sets_.load();
            QMetaObject::invokeMethod(
                self,
                [self, muted, sets] {
                    if (self->mute_sets_.load() == sets) {
                        self->muted_.store(muted);
                        emit self->mutedChanged(muted);
                    }
                },
                Qt::QueuedConnection);
        }
    }
}

void PlaybackStream::on_process(void* data) {
    auto* self = static_cast<PlaybackStream*>(data);

    // Safe unlocked: pw_stream_disconnect() waits for this callback to return
    pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream_);
    if (buffer == nullptr) {
        return;
    }
    spa_data& chunk_data = buffer->buffer->datas[0];
    auto* dest = static_cast<uint8_t*>(chunk_data.data);
    const size_t stride = self->bytes_per_frame_;
    if (dest == nullptr || stride == 0) {
        pw_stream_queue_buffer(self->stream_, buffer);
        return;
    }
    size_t frames = chunk_data.maxsize / stride;
    if (buffer->requested != 0) {
        frames = std::min<size_t>(frames, buffer->requested);
    }
    // A new stream is born at full volume and unmuted. Until it has taken its own it plays
    // silence and the audio waits; one that never does goes on taking the audio, so the player
    // keeps time, but plays it only if full volume is what is wanted. Never louder than asked.
    const bool taken = self->gain_applied_.load(std::memory_order_relaxed) &&
                       self->mute_applied_.load(std::memory_order_relaxed);
    const bool waiting = !taken && clock_ns(CLOCK_MONOTONIC) < self->gate_until_ns_.load();
    uint32_t played = 0;
    if (!waiting) {
        played = static_cast<uint32_t>(self->ring_.read(dest, frames * stride) / stride);
    }
    if (waiting || (!taken && (self->muted_.load() || self->gain_.load() < 1.0F - GAIN_EPSILON))) {
        std::fill_n(dest, frames * stride, uint8_t{0});
    }

    // Read before queueing: `delay` is how long after `now`, the start of this graph cycle on
    // CLOCK_MONOTONIC, this buffer's first frame is heard, behind what the stream still holds.
    // The cycle's age carries onto the raw clock: over milliseconds the two do not differ.
    uint32_t report = 0;
    int64_t finish_us = 0;
    if (played != 0) {
        pw_time time{};
        if (pw_stream_get_time_n(self->stream_, &time, sizeof(time)) == 0 && time.now != 0 &&
            time.rate.denom != 0) {
            const int64_t cycle_age = clock_ns(CLOCK_MONOTONIC) - time.now;
            const double delay_ns = static_cast<double>(time.delay) * 1e9 *
                                    static_cast<double>(time.rate.num) /
                                    static_cast<double>(time.rate.denom);
            const double held_ns = static_cast<double>(time.queued + time.buffered + played) * 1e9 /
                                   static_cast<double>(self->format_.rate);
            finish_us = (clock_ns(CLOCK_MONOTONIC_RAW) - cycle_age +
                         static_cast<int64_t>(delay_ns + held_ns)) /
                        1000;
            report = played + self->owed_frames_.exchange(0, std::memory_order_relaxed);
        } else {
            // No timing this cycle: the frames ride on the next report that has some
            self->owed_frames_.fetch_add(played, std::memory_order_relaxed);
        }
    }

    chunk_data.chunk->offset = 0;
    chunk_data.chunk->stride = static_cast<int32_t>(stride);
    chunk_data.chunk->size = static_cast<uint32_t>(frames * stride);
    // In frames: pw_time::queued sums this field
    buffer->size = frames;
    pw_stream_queue_buffer(self->stream_, buffer);

    // Without the mutex: a missed wakeup costs a waiting write() one cycle
    self->room_.notify_one();
    if (report != 0 && self->played_) {
        self->played_(report, finish_us);
    }
}
