#pragma once

#include <QJsonObject>
#include <QLocalSocket>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

#include <chrono>
#include <optional>

/// @brief Where a Discord client may listen, in the order they are tried: `discord-ipc-0`…`9`
/// in the runtime directory, then in Flatpak Discord's, Flatpak Discord Canary's and the snap's
QStringList discord_socket_paths(const QString& runtime_dir);

/// @brief One Rich Presence activity on the Discord client running here, over its local socket.
/// Connects while there is an activity to show, and tries again, more and more rarely, while
/// there is no Discord to show it to. Never blocks.
class DiscordIpc : public QObject {
    Q_OBJECT

public:
    struct Timing {
        /// Before the first new attempt; doubles up to MAX_RETRY. And how long a socket has to
        /// answer the handshake.
        std::chrono::milliseconds retry{std::chrono::seconds(15)};
        /// Between two updates: Discord takes five in twenty seconds
        std::chrono::milliseconds pace{std::chrono::seconds(4)};
        /// With nothing to show for this long, the connection is closed
        std::chrono::milliseconds idle{std::chrono::minutes(1)};
    };
    static constexpr std::chrono::milliseconds MAX_RETRY{std::chrono::minutes(5)};

    /// @param client_id the Discord application the activity belongs to
    /// @param runtime_dir `$XDG_RUNTIME_DIR`
    DiscordIpc(QString client_id, QString runtime_dir, Timing timing, QObject* parent = nullptr);
    /// Discord drops the activity with the connection
    ~DiscordIpc() override;

    /// @brief Shows the activity in place of the one showing; an empty one shows none, ends
    /// the attempts to reach Discord, and in time the connection
    void set_activity(const QJsonObject& activity);

signals:
    /// Discord took the last activity for an error, and shows what it showed before
    /// @param message Discord's own words for why
    void refused(const QString& message);

private:
    void connect_first_();
    void connect_next_();
    void on_unconnected_();
    void on_ready_read_();
    void on_frame_(qint32 opcode, const QByteArray& payload);
    void send_(qint32 opcode, const QByteArray& payload);
    void flush_();

    const QString client_id_;
    const QString runtime_dir_;
    const Timing timing_;

    /// The sockets of this attempt not tried yet
    QStringList candidates_;
    /// Discord answered the handshake
    bool ready_{false};
    QJsonObject wanted_;
    /// What Discord shows: empty on a new connection, unknown once it refused one
    std::optional<QJsonObject> sent_{QJsonObject()};
    /// The activity Discord refused: not one to send again as it is
    QJsonObject refused_;
    std::chrono::milliseconds next_retry_;
    QTimer retry_{this};
    /// Gives up on a socket that takes the handshake and says nothing
    QTimer handshake_{this};
    QTimer pace_{this};
    QTimer idle_{this};
    /// Acts on a lost or failed connection from the event loop, not from inside its signal
    QTimer unconnected_{this};
    QLocalSocket socket_{this};
};
