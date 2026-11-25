// The stream lifecycle and the capture timestamp started from sendspin-cpp-cli's
// src/pipewire_source.cpp and src/pipewire_sink.cpp at 090e1bb (Apache-2.0,
// https://github.com/Sendspin/sendspin-cpp-cli).

#include "audio/sendspin_output.h"

#include "audio/pipewire_util.h"

#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <utility>

namespace {

constexpr const char* NODE_DESCRIPTION = "Sendspin";

constexpr int START_TIMEOUT_MS = 3000;
constexpr int64_t NS_PER_S = 1000000000;
constexpr size_t FRAME_BYTES = SendspinOutput::CHANNELS * sizeof(float);

// Before PipeWire 1.6 the converter reports volume and mute only once it has a channel mixer,
// which it builds when a player links (audioconvert.c, set_volume()). Setting any converter
// option builds it early (parse_prop_params()); this one is set to its default.
void build_mixer(pw_stream* stream) {
    uint8_t pod_storage[256];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_storage, sizeof(pod_storage));
    spa_pod_frame object{};
    spa_pod_frame options{};
    spa_pod_builder_push_object(&builder, &object, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(&builder, SPA_PROP_params, 0);
    spa_pod_builder_push_struct(&builder, &options);
    spa_pod_builder_string(&builder, "debug.wav-path");
    spa_pod_builder_string(&builder, "");
    spa_pod_builder_pop(&builder, &options);
    const auto* props = static_cast<const spa_pod*>(spa_pod_builder_pop(&builder, &object));
    pw_stream_set_param(stream, SPA_PARAM_Props, props);
}

}  // namespace

bool is_silent(const float* samples, size_t count) {
    return std::all_of(samples, samples + count, [](float sample) { return sample == 0.0F; });
}

SendspinOutput::SendspinOutput(QObject* parent, QByteArray node_name)
    : QObject(parent), node_name_(std::move(node_name)) {
    pw_init(nullptr, nullptr);
}

SendspinOutput::~SendspinOutput() {
    this->stop();
    pw_deinit();
}

void SendspinOutput::set_consumer(Consumer consumer) {
    this->consumer_ = std::move(consumer);
}

bool SendspinOutput::start(QString* error) {
    this->stop();

    const auto fail = [this, error](const QString& reason) {
        if (error != nullptr) {
            *error = reason;
        }
        this->stop();
        return false;
    };

    this->loop_ = pw_thread_loop_new("sendspin-output", nullptr);
    if (this->loop_ == nullptr || pw_thread_loop_start(this->loop_) < 0) {
        return fail(QStringLiteral("cannot start a PipeWire loop"));
    }

    // node.network rather than node.virtual: Plasma hides virtual sinks by default.
    // priority.session 0 keeps WirePlumber from choosing it as the default while another output
    // exists. No object.linger, so the node goes with the process.
    pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CLASS, "Audio/Sink", PW_KEY_NODE_NAME,
        this->node_name_.constData(), PW_KEY_NODE_DESCRIPTION, NODE_DESCRIPTION,
        PW_KEY_NODE_NETWORK, "true", PW_KEY_PRIORITY_SESSION, "0", nullptr);
    if (props == nullptr) {
        return fail(QStringLiteral("cannot describe the Sendspin output to PipeWire"));
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
        table.state_changed = &SendspinOutput::on_state_changed;
        table.control_info = &SendspinOutput::on_control_info;
        table.param_changed = &SendspinOutput::on_param_changed;
        table.process = &SendspinOutput::on_process;
        return table;
    }();

    int err = -ENOMEM;
    {
        const LoopLock lock(this->loop_);
        // pw_stream_new_simple() consumes props even on failure.
        this->stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(this->loop_), NODE_DESCRIPTION,
                                             props, &events, this);
        if (this->stream_ != nullptr) {
            const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                                            PW_STREAM_FLAG_MAP_BUFFERS |
                                                            PW_STREAM_FLAG_RT_PROCESS);
            err = pw_stream_connect(this->stream_, PW_DIRECTION_INPUT, PW_ID_ANY, flags, params, 1);
        } else {
            err = -errno;
        }
        if (err >= 0) {
            // PAUSED counts: the graph has the node and runs it once something plays to it.
            // UNCONNECTED after connect() set CONNECTING is a refusal.
            timespec deadline{};
            pw_thread_loop_get_time(this->loop_, &deadline,
                                    static_cast<int64_t>(START_TIMEOUT_MS) * 1000 * 1000);
            while (true) {
                const int state = this->state_.load();
                if (state != PW_STREAM_STATE_CONNECTING) {
                    break;
                }
                if (pw_thread_loop_timed_wait_full(this->loop_, &deadline) != 0) {
                    break;
                }
            }
        }
    }

    if (err < 0) {
        return fail(
            QStringLiteral("cannot reach PipeWire: %1").arg(QString::fromUtf8(spa_strerror(err))));
    }
    const int state = this->state_.load();
    if (state != PW_STREAM_STATE_STREAMING && state != PW_STREAM_STATE_PAUSED) {
        return fail(QStringLiteral("PipeWire did not take the Sendspin output"));
    }
    this->connected_.store(true);
    const LoopLock lock(this->loop_);
    build_mixer(this->stream_);
    if (this->latency_ns_.load() != 0) {
        this->publish_latency_();
    }
    return true;
}

void SendspinOutput::stop() {
    this->connected_.store(false);
    if (this->stream_ != nullptr) {
        const LoopLock lock(this->loop_);
        // Disconnect before destroy, under the loop lock: disconnect waits out on_process().
        pw_stream_disconnect(this->stream_);
        pw_stream_destroy(this->stream_);
        this->stream_ = nullptr;
    }
    if (this->loop_ != nullptr) {
        pw_thread_loop_destroy(this->loop_);
        this->loop_ = nullptr;
    }
    this->state_.store(PW_STREAM_STATE_UNCONNECTED);
    this->untimed_blocks_.store(0);
    if (this->rate_.exchange(0) != 0) {
        // A rateChanged still queued from the old stream is delivered after this one.
        emit this->rateChanged(0);
    }
}

uint32_t SendspinOutput::node_id() const {
    if (this->stream_ == nullptr) {
        return PW_ID_ANY;
    }
    const LoopLock lock(this->loop_);
    return pw_stream_get_node_id(this->stream_);
}

void SendspinOutput::set_volume(float volume) {
    if (this->stream_ == nullptr) {
        return;
    }
    // The desktop's scale is the cube root of the gain.
    const float gain = std::pow(std::max(volume, 0.0F), 3.0F);
    float gains[CHANNELS];
    std::fill(std::begin(gains), std::end(gains), gain);
    const LoopLock lock(this->loop_);
    pw_stream_set_control(this->stream_, SPA_PROP_channelVolumes, CHANNELS, gains, 0);
}

void SendspinOutput::set_muted(bool muted) {
    if (this->stream_ == nullptr) {
        return;
    }
    float value = muted ? 1.0F : 0.0F;
    const LoopLock lock(this->loop_);
    pw_stream_set_control(this->stream_, SPA_PROP_mute, 1, &value, 0);
}

void SendspinOutput::set_latency(int64_t latency_ns) {
    this->latency_ns_.store(std::max<int64_t>(latency_ns, 0));
    if (this->stream_ != nullptr) {
        const LoopLock lock(this->loop_);
        this->publish_latency_();
    }
}

void SendspinOutput::publish_latency_() {
    uint8_t pod_storage[256];
    spa_pod_builder builder = SPA_POD_BUILDER_INIT(pod_storage, sizeof(pod_storage));
    spa_latency_info latency{};
    latency.direction = SPA_DIRECTION_INPUT;
    latency.min_ns = this->latency_ns_.load();
    latency.max_ns = latency.min_ns;
    const spa_pod* params[1];
    params[0] = spa_latency_build(&builder, SPA_PARAM_Latency, &latency);
    pw_stream_update_params(this->stream_, params, 1);
}

void SendspinOutput::on_state_changed(void* data, pw_stream_state /*old*/, pw_stream_state state,
                                      const char* error) {
    auto* self = static_cast<SendspinOutput*>(data);
    self->state_.store(state);
    const bool gone = state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED;
    if (gone && self->connected_.exchange(false)) {
        const QString reason =
            (error != nullptr) ? QString::fromUtf8(error) : QStringLiteral("disconnected");
        QMetaObject::invokeMethod(
            self, [self, reason] { emit self->lost(reason); }, Qt::QueuedConnection);
    }
    pw_thread_loop_signal(self->loop_, false);
}

void SendspinOutput::on_param_changed(void* data, uint32_t id, const spa_pod* param) {
    auto* self = static_cast<SendspinOutput*>(data);
    if (id == SPA_PARAM_Latency) {
        // Before PipeWire 1.6 the peer's latency arriving replaces ours (stream.c,
        // clear_params()), so ours goes back after each one.
        if (self->latency_ns_.load() != 0) {
            self->publish_latency_();
        }
        return;
    }
    if (id != SPA_PARAM_Format) {
        return;
    }
    // A null param is the format being cleared while idle; the last rate stands.
    spa_audio_info_raw info{};
    if (param == nullptr || spa_format_audio_raw_parse(param, &info) < 0) {
        return;
    }
    const uint32_t rate = info.rate;
    if (self->rate_.exchange(rate) != rate) {
        QMetaObject::invokeMethod(
            self, [self, rate] { emit self->rateChanged(rate); }, Qt::QueuedConnection);
    }
}

void SendspinOutput::on_control_info(void* data, uint32_t id, const pw_stream_control* control) {
    auto* self = static_cast<SendspinOutput*>(data);
    if (control->n_values == 0) {
        return;
    }
    if (id == SPA_PROP_channelVolumes) {
        float gain = 0.0F;
        for (uint32_t i = 0; i < control->n_values; i++) {
            gain += control->values[i];
        }
        const float volume = std::cbrt(gain / static_cast<float>(control->n_values));
        QMetaObject::invokeMethod(
            self,
            [self, volume] {
                if (self->volume_ != volume) {
                    self->volume_ = volume;
                    emit self->volumeChanged(volume);
                }
            },
            Qt::QueuedConnection);
    } else if (id == SPA_PROP_mute) {
        const bool muted = control->values[0] >= 0.5F;
        QMetaObject::invokeMethod(
            self,
            [self, muted] {
                if (self->muted_ != muted) {
                    self->muted_ = muted;
                    emit self->mutedChanged(muted);
                }
            },
            Qt::QueuedConnection);
    }
}

int64_t SendspinOutput::capture_time_ns_(uint32_t rate) {
    const int64_t raw_now = clock_ns(CLOCK_MONOTONIC_RAW);
    const int64_t monotonic_now = clock_ns(CLOCK_MONOTONIC);
    pw_time time{};
    if (pw_stream_get_time_n(this->stream_, &time, sizeof(time)) != 0 || time.now == 0) {
        this->untimed_blocks_.fetch_add(1, std::memory_order_relaxed);
        return raw_now;
    }
    // `now` is the start of this graph cycle on CLOCK_MONOTONIC. Its age carries onto the raw
    // clock: over a few milliseconds the two clocks' rates do not differ measurably. `buffered`
    // is what the converter still holds, which this block's frames queued behind.
    const int64_t cycle_age = monotonic_now - time.now;
    const int64_t held = static_cast<int64_t>(time.buffered) * NS_PER_S / rate;
    return raw_now - cycle_age - held;
}

void SendspinOutput::on_process(void* data) {
    auto* self = static_cast<SendspinOutput*>(data);

    // Safe unlocked: pw_stream_disconnect() waits for this callback to return.
    pw_buffer* buffer = pw_stream_dequeue_buffer(self->stream_);
    if (buffer == nullptr) {
        return;
    }
    const spa_data& chunk_data = buffer->buffer->datas[0];
    const uint32_t rate = self->rate_.load(std::memory_order_relaxed);
    if (chunk_data.data != nullptr && rate != 0 && self->consumer_) {
        const uint32_t offset = std::min(chunk_data.chunk->offset, chunk_data.maxsize);
        const size_t frames =
            std::min(chunk_data.chunk->size, chunk_data.maxsize - offset) / FRAME_BYTES;
        if (frames != 0) {
            // PipeWire maps buffers page-aligned, so the float view is aligned.
            const auto* samples = reinterpret_cast<const float*>(
                static_cast<const uint8_t*>(chunk_data.data) + offset);
            const CapturedBlock block{samples, static_cast<uint32_t>(frames), rate,
                                      self->capture_time_ns_(rate),
                                      is_silent(samples, frames * CHANNELS)};
            self->consumer_(block);
        }
    }
    pw_stream_queue_buffer(self->stream_, buffer);
}
