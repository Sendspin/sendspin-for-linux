#pragma once

#include "audio/play_along.h"
#include "audio/sendspin_output.h"
#include "cast/cast_feed.h"
#include "cast/speaker_menu.h"
#include "cast/speaker_model.h"
#include "persistence.h"

#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QSettings>
#include <QString>
#include <QTimer>

#include <sendspin/server/server.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

/// @brief Whether a browsed instance is this app's own Sendspin client: its port, at one of this
/// machine's addresses. The server must never dial it; its one connection is Music Assistant's.
bool is_own_client(const sendspin::server::DiscoveredClient& found, uint16_t own_port,
                   const QList<QHostAddress>& own_addresses);

/// @brief Casts the Sendspin output to the added speakers through an embedded SendspinServer.
/// Discovery, pairing, trust, reconnects and the speaker buffer are the library's; the engine
/// decides who is in the group and feeds it.
///
/// The server's own group is a lobby with no input; the cast group holds exactly the added
/// speakers, so nothing plays on one the user didn't add (docs/SPEC.md, "What ticking does").
///
/// This PC's own output is in the group or not, like a speaker, without being a Sendspin player:
/// beside a speaker it plays in the group's time, alone it gets the capture directly
/// (docs/SPEC.md, "This PC's output").
///
/// Main thread only. The library's callbacks arrive on its dispatch thread and are queued here.
class CastEngine : public QObject {
    Q_OBJECT
    Q_PROPERTY(SpeakerModel* speakers READ speakers CONSTANT)
    Q_PROPERTY(int groupVolume READ group_volume WRITE setGroupVolume NOTIFY groupVolumeChanged)
    Q_PROPERTY(bool groupMuted READ group_muted WRITE setGroupMuted NOTIFY groupMutedChanged)
    Q_PROPERTY(bool playing READ playing NOTIFY playingChanged)
    Q_PROPERTY(bool discoveryAvailable READ discovery_available NOTIFY discoveryAvailableChanged)

public:
    /// How late a write may come. The graph hands over one quantum at a time, so a quantum that
    /// grows makes the next write late by the difference: this covers the step from the smallest
    /// quantum to PipeWire's default largest, 2048 frames.
    static constexpr int64_t WRITE_JITTER_US = 45000;

    /// @param output told the speaker buffer as its latency. Its consumer hands feed() each
    /// block.
    CastEngine(const std::filesystem::path& data_dir, SendspinOutput& output,
               QObject* parent = nullptr);
    ~CastEngine() override;

    /// @brief This app's own Sendspin client, which the server never dials, approves, pairs or
    /// lists. Before start().
    /// @param port where it listens, which is how browsing knows its instance
    void set_own_client(uint16_t port, std::string client_id) {
        this->own_client_port_ = port;
        this->own_client_id_ = std::move(client_id);
    }

    /// @brief A taken port is replaced by one the system picks
    /// @param discovery advertise the server and browse for speakers; tests go without
    bool start(uint16_t port = sendspin::server::config_defaults::LISTEN_PORT,
               bool discovery = true);
    void stop();

    /// @brief The port speakers connect to; 0 while stopped
    uint16_t port() const;

    SpeakerModel* speakers() {
        return &this->model_;
    }

    /// @brief The cast group's volume as last set from this PC, 0 to 100. The library reports
    /// none back, so a change made at a speaker doesn't show here.
    int group_volume() const {
        return this->volume_.value_or(DEFAULT_VOLUME);
    }
    bool group_muted() const {
        return this->muted_;
    }

    /// @brief Sendspin plays: there was sound in the last 3 s and a connected speaker gets it
    bool playing() const {
        return this->playing_;
    }

    /// @brief False while mDNS can't be reached (avahi-daemon isn't running)
    bool discovery_available() const {
        return this->discovery_available_;
    }

    /// @brief False while what plays can't be joined (a server): the rows have no check
    void set_plays_groupable(bool groupable);

    /// @brief Where the Sendspin output's blocks go: CastFeed::take(), on the realtime thread
    CastFeed& feed() {
        return this->feed_;
    }

    /// @brief The output this PC plays along on, a node.name; nullopt for none. Never the
    /// Sendspin output.
    void set_local_target(const std::optional<QByteArray>& target);
    bool local_in_group() const {
        return this->local_in_group_;
    }
    /// @brief This PC's own output as the picker's rules see it: in the group, found, and
    /// whether it plays. Beside speakers whose buffer is shorter than its own delay it can't.
    speaker_menu::Output local_output() const;
    PlayAlong& play_along() {
        return this->play_along_;
    }

public slots:
    // id is a row's SpeakerModel::IdRole

    /// @brief Adds a speaker to the group: it is connected, approved for unpaired playback or
    /// paired as it needs, and plays. Adding a row that shows "Identity changed" accepts the new
    /// identity.
    void add(const QString& id);
    /// @brief Takes a speaker out of the group; its stream ends
    void remove(const QString& id);

    void setGroupVolume(int volume);
    void setGroupMuted(bool muted);

    /// @brief The picker keeps the choice: in the group until told otherwise
    void setLocalInGroup(bool in_group);

    /// @brief Starts pairing by the best method the speaker offers: the dynamic code, the static
    /// code, then the token. pairingCodeNeeded() asks for what to submit.
    /// @return false when it offers none, or the server refused
    bool pair(const QString& id);
    bool submitPairingCode(const QString& id, const QString& code);
    /// @brief Pairs by the speaker's token ("SP:0..."), whether or not pair() asked for it
    bool submitPairingToken(const QString& id, const QString& token);
    void cancelPairing(const QString& id);
    /// @brief Unpairs the speaker and takes it out of the group
    void forget(const QString& id);

signals:
    void groupVolumeChanged(int volume);
    void groupMutedChanged(bool muted);
    void playingChanged(bool playing);
    void discoveryAvailableChanged(bool available);
    /// local_output() changed
    void localChanged();

    void pairingCodeNeeded(const QString& id, sendspin::server::PairMethod method,
                           sendspin::server::CodeFormat format);
    /// The speaker waits for its own button. message is the device's text: show it as plain text.
    void pairingPending(const QString& id, const QString& message);
    void pairingFinished(const QString& id, sendspin::server::PairingResult result);

private:
    class Listener;
    class Discovery;
    static constexpr int DEFAULT_VOLUME = 100;

    struct Speaker {
        /// Empty until it has connected once
        std::string client_id;
        std::string url;
        QString name;
        /// In SendspinServer::clients(): connected, or the server can dial it
        bool listed{false};
        speaker_menu::Facts facts;
        bool has_player{false};
        /// In the cast group with a player, where the group's volume and mute reach it
        bool plays{false};
        bool can_pair{false};
        sendspin::server::PairingOffers offers;
        /// Why it isn't connected, from how it left or how the dial failed
        std::optional<speaker_menu::Connection> left_as;
        /// It lists no format the server can produce
        bool cant_play{false};
        /// The group it was last reported in
        std::optional<sendspin::server::GroupId> group;
        /// A step is under way; until this passes or its group changes, none follows, and a
        /// change of group is ours rather than the speaker's own
        int64_t busy_until_ns{0};
        /// Pairing was started for this connection; it is not started twice
        bool pairing_asked{false};
        /// "Identity changed": the added speaker this unknown one presents itself as
        QString presents_as;

        QString key() const;
    };

    void refresh_();
    void sync_clients_();
    void detect_identity_changes_();
    void take_steps_(int64_t now_ns);
    void update_input_(int64_t now_ns);
    void update_local_(int64_t now_ns);
    void refresh_local_();
    void publish_rows_();
    void release_cast_();
    void send_group_levels_();

    void on_connected_(const sendspin::server::ClientInfo& info);
    void on_disconnected_(const sendspin::server::ClientInfo& info,
                          const sendspin::server::ClientDisconnect& disconnect);
    void on_error_(const sendspin::server::ServerError& error);
    void on_group_changed_(const sendspin::server::GroupInfo& group);
    void on_pairing_result_(const sendspin::server::ClientInfo& info,
                            sendspin::server::PairingResult result);

    Speaker& speaker_for_(const sendspin::server::ClientInfo& info);
    Speaker* find_(const QString& key);
    Speaker* find_client_(const std::string& client_id);
    speaker_menu::Connection connection_of_(const Speaker& speaker) const;
    void set_ticked_(Speaker& speaker, bool ticked);
    void set_discovery_available_(bool available);

    void load_settings_();
    void save_ticked_();

    SendspinOutput& output_;
    ServerPersistence persistence_;
    QSettings settings_;
    uint16_t own_client_port_{0};
    std::string own_client_id_;
    std::unique_ptr<Listener> listener_;
    std::unique_ptr<sendspin::server::SendspinServer> server_;
    // Before the feed, which writes to it. It knows the Sendspin output by its default name, as
    // Outputs does: the app's output is not constructed yet when the engine is.
    PlayAlong play_along_;
    CastFeed feed_;
    SpeakerModel model_;
    QTimer timer_;

    std::vector<Speaker> speakers_;
    /// The group the app casts into; it lasts while a speaker is added
    std::optional<sendspin::server::GroupId> cast_group_;
    /// The server listed it: it takes an input and moves
    bool cast_exists_{false};
    /// When a group asked for and never listed is given up: its first member didn't move
    int64_t cast_deadline_ns_{0};
    /// A speaker joined the cast group: until this passes its volume and mute are sent again
    int64_t levels_until_ns_{0};

    /// Unset until the user sets one, so adding a speaker leaves its volume alone
    std::optional<int> volume_;
    bool muted_{false};
    bool playing_{false};
    bool plays_groupable_{true};
    bool discovery_available_{true};
    int64_t latency_ns_{0};

    bool local_in_group_{true};
    /// Its delay is longer than the speaker buffer: it can't play in time
    bool local_late_{false};
    /// What local_output() last said, to tell a change
    bool local_plays_{false};
    int64_t local_logged_ns_{0};
};
