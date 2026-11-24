#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <pipewire/pipewire.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>

/// @brief One block of what apps played to the Sendspin output. Valid only during the callback.
struct CapturedBlock {
    const float* samples;  ///< interleaved, SendspinOutput::CHANNELS per frame
    uint32_t frames;
    uint32_t rate;
    /// Graph time of the first frame on the Sendspin libraries' clock: CLOCK_MONOTONIC_RAW
    /// nanoseconds.
    int64_t capture_time_ns;
    bool silent;  ///< every sample is zero
};

/// @brief True when every sample is digital silence.
bool is_silent(const float* samples, size_t count);

/// @brief The "Sendspin" output device: a PipeWire Audio/Sink node whose input is handed to a
/// consumer. The node lives from start() until stop() or the end of the process.
class SendspinOutput : public QObject {
    Q_OBJECT

public:
    static constexpr uint32_t CHANNELS = 2;
    static constexpr uint32_t RATE = 48000;

    /// Called on PipeWire's realtime thread: no locks, allocation or blocking calls.
    using Consumer = std::function<void(const CapturedBlock&)>;

    /// @param node_name The node's `node.name`, which the session manager keys its saved volume
    /// on. Tests pass their own so they leave the app's alone.
    explicit SendspinOutput(QObject* parent = nullptr, QByteArray node_name = "sendspin");
    ~SendspinOutput() override;

    /// @brief Sets where captured audio goes. Only while stopped.
    void set_consumer(Consumer consumer);

    /// @brief Creates the node and waits for the graph to take it.
    /// @param error Set to the reason on failure, if given.
    bool start(QString* error = nullptr);
    void stop();

    /// @brief The node's id in the graph, or PW_ID_ANY while stopped.
    uint32_t node_id() const;

    /// @brief The rate the graph last negotiated. 0 until it has, which is when something first
    /// plays to the output.
    uint32_t rate() const {
        return this->rate_.load(std::memory_order_relaxed);
    }

    /// @brief The node's volume as the desktop shows it (cubic), 1.0 being 100 %.
    float volume() const {
        return this->volume_;
    }
    bool muted() const {
        return this->muted_;
    }

    /// PipeWire applies both to the captured audio. The session manager restores the node's saved
    /// volume shortly after start(), over anything set before then.
    void set_volume(float volume);
    void set_muted(bool muted);

    /// @brief Reports how long after its capture time a frame is heard, for players that keep
    /// A/V sync against the output's latency.
    void set_latency(int64_t latency_ns);

signals:
    void rateChanged(quint32 rate);
    void volumeChanged(float volume);
    void mutedChanged(bool muted);
    /// The graph dropped the node (daemon gone, or an error). stop() and start() to try again.
    void lost(const QString& reason);

private:
    static void on_state_changed(void* data, pw_stream_state old, pw_stream_state state,
                                 const char* error);
    static void on_param_changed(void* data, uint32_t id, const spa_pod* param);
    static void on_control_info(void* data, uint32_t id, const pw_stream_control* control);
    static void on_process(void* data);

    int64_t capture_time_ns_(uint32_t rate) const;
    void publish_latency_();

    const QByteArray node_name_;
    Consumer consumer_;

    pw_thread_loop* loop_{nullptr};
    pw_stream* stream_{nullptr};
    std::atomic<int> state_{PW_STREAM_STATE_UNCONNECTED};
    std::atomic<bool> connected_{false};  ///< taken by the graph, so losing it is reported
    std::atomic<uint32_t> rate_{0};
    int64_t latency_ns_{0};

    float volume_{1.0F};
    bool muted_{false};
};
