#pragma once

#include "mdns_advertiser.h"
#include "now_playing.h"
#include "player.h"
#include "source_feed.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <sendspin/client.h>

#include <cstdint>
#include <vector>

/// @brief This PC as a Sendspin client: one sendspin-cpp client that a server (Music Assistant)
/// finds over mDNS, connects to and pairs with, and the roles it has there. Each role is a member
/// that adds itself to the library client. The one connection owns them all.
///
/// Signals are emitted from the event loop, never from inside a library callback, so a slot may
/// call anything here.
class Client : public QObject,
               private sendspin::SendspinClientListener,
               private sendspin::SendspinNetworkProvider {
    Q_OBJECT

public:
    static constexpr uint16_t DEFAULT_PORT = sendspin::SendspinClientConfig::DEFAULT_SERVER_PORT;

    enum class State {
        Stopped,    ///< not started
        Waiting,    ///< advertised; no server has connected
        Connected,  ///< to server_name()
        Pairing,    ///< a server is pairing with this PC
    };
    Q_ENUM(State)

    struct Config {
        QString name;  ///< what servers show for this PC
        uint16_t port{DEFAULT_PORT};
        /// Off in tests, which must not draw in the servers on the network
        bool advertise{true};
        /// A server may play here without pairing; the source still needs pairing
        bool unpaired_access{true};
        /// What the player advertises, the preferred one first; empty for 48 kHz, 16-bit
        std::vector<sendspin::AudioSupportedFormatObject> player_formats{};
    };

    /// @brief preferred when nothing listens on it, else a port the system picks
    static uint16_t free_port(uint16_t preferred = DEFAULT_PORT);

    /// @param persistence keeps the identity and the pairing records; outlives the client
    Client(const Config& config, sendspin::SendspinPersistenceProvider* persistence,
           QObject* parent = nullptr);
    ~Client() override;

    /// @brief Starts listening for servers and advertising. The first start creates the identity.
    bool start();
    void stop();

    State state() const {
        return this->state_;
    }
    /// @brief What servers show for this PC; fixed while the client runs
    QString name() const {
        return this->name_;
    }
    /// @brief The connected server's name; empty when there is none
    QString server_name() const {
        return this->server_name_;
    }
    /// @brief The code a pairing server asks for; empty when none is showing. Six digits to
    /// type into the server, or with pairing_code_is_qr() a token to show it as a QR code.
    QString pairing_code() const {
        return this->pairing_code_;
    }
    bool pairing_code_is_qr() const {
        return this->code_is_qr_;
    }
    /// @brief The connection is to a server this PC is paired with
    bool is_paired() const {
        return this->client_.is_paired();
    }
    /// @brief A server's pairing waits for the user to allow it: after too many wrong codes the
    /// library holds pairing back until someone at this PC says so
    bool pairing_approval_needed() const {
        return this->approval_needed_;
    }
    /// @brief The user allows the pairing that waits, and pairing in general again
    void allow_pairing();
    /// @brief Whether a server may play here without pairing. Servers connected unpaired
    /// reconnect to see the change.
    void set_unpaired_access(bool enabled);
    /// @brief The token to paste into a server to pair by Pairing PSK ("SP:0…"); empty before
    /// start(). A secret: show it only when asked, never log it.
    QString pairing_token() const;
    /// @brief This PC's identity to servers; empty before the first start()
    QString client_id() const;
    uint16_t port() const {
        return this->port_;
    }

    SourceFeed& source() {
        return this->source_;
    }
    Player& player() {
        return this->player_;
    }
    NowPlaying& now_playing() {
        return this->now_playing_;
    }

signals:
    void stateChanged(Client::State state);
    void serverNameChanged(const QString& name);
    void pairingCodeChanged(const QString& code);
    void pairingApprovalNeededChanged(bool needed);
    void paired(const QString& serverName, const QString& serverId);
    /// A pairing ended without pairing
    void pairingEnded();

private:
    void on_pairing_attempt(std::string_view server_id) override;
    void on_paired(std::string_view server_id) override;
    void on_pairing_aborted(std::string_view server_id,
                            sendspin::SendspinPairAbortReason reason) override;
    void on_pairing_code(std::string_view code,
                         sendspin::SendspinPairingCodeFormat format) override;
    void on_clear_pairing_code() override;
    void on_open_pairing_window() override;
    void on_close_pairing_window() override;
    bool is_network_ready() override {
        return true;
    }

    void tick_();
    void sync_();

    const QString name_;
    const uint16_t port_;
    const bool advertise_;

    sendspin::SendspinClient client_;
    SourceFeed source_;
    Player player_;
    NowPlaying now_playing_;
    MdnsAdvertiser advertiser_;
    QTimer loop_timer_;

    // What the library reported, written in its callbacks
    bool pairing_{false};
    bool pairing_connected_{false};  ///< the pairing server's connection was seen up
    QString code_;
    bool code_is_qr_{false};
    QString paired_id_;
    /// The pairing server's name: its connection is renewed as it pairs, and is nameless then
    QString pairing_name_;
    bool window_wanted_{false};
    bool paired_event_{false};
    bool ended_event_{false};

    // What was last signalled
    State state_{State::Stopped};
    QString server_name_;
    QString pairing_code_;
    bool approval_needed_{false};
};
