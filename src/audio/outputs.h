#pragma once

#include "audio/player_formats.h"

#include <QByteArray>
#include <QObject>
#include <QTimer>

#include <pipewire/pipewire.h>

#include <cstdint>
#include <optional>
#include <vector>

/// @brief An output device in the graph: a node with media.class Audio/Sink
struct OutputSink {
    uint32_t id{0};
    QByteArray name;  ///< node.name
    int priority{0};  ///< priority.session

    bool operator==(const OutputSink&) const = default;
};

/// @brief Where what a server plays to this PC is heard: the output chosen in Settings, else the
/// system default. Never the Sendspin output, which would send it back in as the PC's own input:
/// with that as the default it is the last default that was another output, else the best other
/// one (docs/SPEC.md, "Loop guards").
/// @param chosen The output chosen in Settings; empty to follow the default
/// @param own The Sendspin output's node.name
/// @return A node.name; nullopt when there is nowhere to play
std::optional<QByteArray> choose_target(const std::vector<OutputSink>& sinks,
                                        const QByteArray& default_sink, const QByteArray& chosen,
                                        const QByteArray& own, const QByteArray& last_default);

/// @brief The PipeWire graph's outputs, as the session manager reports them: which exist, which
/// is the default, and the rates the daemon may run at. Without a daemon it keeps trying.
class Outputs : public QObject {
    Q_OBJECT

public:
    explicit Outputs(QByteArray own_sink = "sendspin", QObject* parent = nullptr);
    ~Outputs() override;

    /// @brief Connects and waits, briefly, for the graph to be listed. Retries by itself after a
    /// failure and when the daemon goes away.
    bool start();
    void stop();

    /// @brief The output chosen in Settings, a node.name; empty to follow the default
    void set_chosen(const QByteArray& name);

    /// @brief choose_target() for the graph as it is; changes are signalled
    std::optional<QByteArray> target() const {
        return this->target_;
    }

    /// @brief This PC's own output: the one the system uses, or used last before the Sendspin
    /// output; when that's gone, the best other one. nullopt when there is none.
    std::optional<OutputSink> local() const;

    ClockRates clock() const;

    /// @brief Asks an output what it plays. Waits for the answer, briefly; empty without one.
    SinkFormats formats(const QByteArray& sink);

signals:
    void targetChanged();
    /// The outputs or the default may have changed
    void changed();

private:
    struct Metadata {
        uint32_t id{0};  ///< the global's, to know it by when it goes
        pw_proxy* proxy{nullptr};
        spa_hook hook{};
    };

    static void on_global(void* data, uint32_t id, uint32_t permissions, const char* type,
                          uint32_t version, const spa_dict* props);
    static void on_global_remove(void* data, uint32_t id);
    static void on_core_done(void* data, uint32_t id, int seq);
    static void on_core_error(void* data, uint32_t id, int seq, int res, const char* message);
    static int on_default(void* data, uint32_t subject, const char* key, const char* type,
                          const char* value);
    static int on_setting(void* data, uint32_t subject, const char* key, const char* type,
                          const char* value);
    static void on_node_param(void* data, int seq, uint32_t id, uint32_t index, uint32_t next,
                              const spa_pod* param);

    void bind_metadata_(Metadata& metadata, uint32_t id, bool settings);
    static void unbind_metadata_(Metadata& metadata);
    /// Starts a round trip to the daemon and waits it out. Caller holds the loop lock.
    bool sync_(int timeout_ms);
    /// From PipeWire's thread: has the event loop look at the graph again
    void changed_();
    void update_target_();
    void lost_();

    const QByteArray own_sink_;
    QByteArray chosen_;
    std::optional<QByteArray> target_;
    QTimer retry_;

    pw_thread_loop* loop_{nullptr};
    pw_context* context_{nullptr};
    pw_core* core_{nullptr};
    pw_registry* registry_{nullptr};
    spa_hook core_hook_{};
    spa_hook registry_hook_{};

    // Written on PipeWire's thread, read under the loop lock
    Metadata default_metadata_;
    Metadata settings_metadata_;
    std::vector<OutputSink> sinks_;
    QByteArray default_sink_;
    QByteArray last_default_;  ///< the last default that was not the Sendspin output
    ClockRates clock_;
    int sync_seq_{0};
    bool synced_{false};
    SinkFormats* asked_{nullptr};  ///< where formats() collects its answer
};
