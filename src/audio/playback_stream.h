#pragma once

#include "audio/pcm_ring.h"

#include <QByteArray>
#include <QObject>

#include <pipewire/pipewire.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

/// @brief The "Sendspin" playback stream: what a server plays to this PC, into a PipeWire output.
/// The stream opens with the first audio written and reopens when the format or the target
/// changes. Its volume and mute are the stream's own in the OS mixer.
///
/// write() belongs to one thread, the player's; everything else to the event loop.
/// Lock order is mutex_ then the thread-loop lock; no PipeWire callback takes mutex_.
class PlaybackStream : public QObject {
    Q_OBJECT

public:
    struct Format {
        uint32_t rate{0};
        uint8_t channels{0};
        uint8_t bits{0};  ///< 8, 16, 24 or 32: signed little-endian, 24 packed in 3 bytes

        bool operator==(const Format&) const = default;
    };

    /// Frames that left for the output, and when the last of them is heard, in microseconds on
    /// the Sendspin libraries' clock. Called on PipeWire's realtime thread, and on the writer's
    /// for audio there was nowhere to play.
    using Played = std::function<void(uint32_t frames, int64_t finish_us)>;

    /// @param node_name The stream's `node.name`; tests pass their own
    explicit PlaybackStream(QObject* parent = nullptr, QByteArray node_name = "sendspin-player");
    ~PlaybackStream() override;

    /// @brief Sets who hears what was played. Before the first write().
    void set_played(Played played);

    /// @brief Sets the output to play on, a node.name; nullopt for nowhere. An open stream moves
    /// there with its next write.
    void set_target(const std::optional<QByteArray>& target);

    /// @brief Queues PCM, waiting up to timeout_ms for room. Audio there is nowhere to play is
    /// taken and reported played, so the player keeps time.
    /// @return Bytes taken, a whole number of frames
    size_t write(const uint8_t* data, size_t length, const Format& format, uint32_t timeout_ms);

    /// @brief The server's stream ended: drops what is queued and lets the output idle
    void clear();
    /// @brief Closes the stream; the next write() opens one again
    void stop();

    /// @brief The stream's volume as a linear gain, which the OS mixer shows as its own
    void set_gain(float gain);
    void set_muted(bool muted);
    float gain() const {
        return this->gain_.load();
    }
    bool muted() const {
        return this->muted_.load();
    }

    /// @brief A stream is open in the graph
    bool is_open() const;
    /// @brief The open stream's node id, or PW_ID_ANY
    uint32_t node_id() const;

signals:
    /// The OS mixer changed the stream's volume
    void gainChanged(float gain);
    void mutedChanged(bool muted);

private:
    static void on_state_changed(void* data, pw_stream_state old, pw_stream_state state,
                                 const char* error);
    static void on_control_info(void* data, uint32_t id, const pw_stream_control* control);
    static void on_process(void* data);

    /// Opens a stream for a format on target_. Caller holds mutex_ and has closed any other.
    bool open_(const Format& format);
    /// Closes the stream and forgets the format. Caller holds mutex_.
    void close_();
    /// Closes a stream whose queued audio will not be heard, which the player still counts.
    /// Caller holds mutex_.
    void abandon_();
    bool alive_() const;
    /// Caller holds the loop lock, or is a PipeWire callback
    void apply_gain_();
    void apply_mute_();
    /// The stream reported a volume or mute that was set from here
    void took_(std::atomic<bool>& applied);
    /// A volume reported now is not the mixer's doing: the one the stream was born with, or the
    /// echo of a value since replaced
    bool own_time_() const;

    const QByteArray node_name_;
    Played played_;

    /// Serialises the stream, the format and the ring's producer side
    mutable std::mutex mutex_;
    /// Signalled by the process callback when it has taken audio from the ring
    std::condition_variable room_;
    PcmRing ring_;

    pw_thread_loop* loop_{nullptr};
    pw_stream* stream_{nullptr};
    std::optional<QByteArray> target_;
    Format format_;
    /// Read by on_process() unlocked: changed only while no stream is connected
    size_t bytes_per_frame_{0};
    /// Bumped on open, close and clear; a write() that waited compares it on waking
    uint64_t generation_{0};
    /// Paused by clear(), so the output can suspend between streams
    bool paused_{false};
    /// No sooner than this is a failed open tried again
    std::chrono::steady_clock::time_point retry_at_;

    // The volume wanted, and whether the stream has taken it: a new stream is born at full
    // volume and unmuted, and plays silence until it has both
    std::atomic<float> gain_{1.0F};
    std::atomic<bool> muted_{false};
    std::atomic<bool> gain_applied_{false};
    std::atomic<bool> mute_applied_{false};
    /// CLOCK_MONOTONIC nanoseconds: see own_time_()
    std::atomic<int64_t> own_until_ns_{0};
    /// By then a new stream plays whether it took its volume or not
    std::atomic<int64_t> gate_until_ns_{0};

    /// Set before stop() takes the mutex, so a waiting write() gives up
    std::atomic<bool> stopping_{false};
    /// The graph errored or dropped the stream
    std::atomic<bool> failed_{false};
    std::atomic<int> state_{PW_STREAM_STATE_UNCONNECTED};
    /// Frames the player counts as queued that no timed report has covered yet
    std::atomic<uint32_t> owed_frames_{0};
};
