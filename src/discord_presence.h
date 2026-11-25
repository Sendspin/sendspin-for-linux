#pragma once

#include "client/client.h"
#include "discord_ipc.h"

#include <QJsonObject>
#include <QObject>
#include <QStandardPaths>
#include <QString>

#include <memory>

/// @brief The cover's URL when Discord's servers, which fetch it, can: https, no longer than
/// Discord takes, and on a host with a public name. Not an address, a LAN's name or a URL with
/// a password in it.
/// @return url, or empty when Discord gets the logo instead
QString discord_artwork_url(const QString& url);

/// @brief "Listening to" a track, as Discord's Rich Presence takes it: the title, the artist,
/// the album over the cover, and when the track began and ends, which draws the bar
/// @param artwork_url what discord_artwork_url() let through: the cover, with the Sendspin logo
/// small beside it. Empty for the logo alone.
/// @param position_ms where the track is at now_ms
/// @param now_ms milliseconds since the epoch
QJsonObject discord_activity(const Track& track, const QString& artwork_url, qint64 position_ms,
                             qint64 now_ms);

/// @brief Shows what a server plays to this PC in Discord, while switched on in Settings. Off
/// until then, and nothing connects to Discord while it is off, paused or stopped.
class DiscordPresence : public QObject {
    Q_OBJECT

public:
    /// @brief The Discord application this build shows itself as (`SENDSPIN_DISCORD_CLIENT_ID`);
    /// empty when it was built without one
    static QString build_client_id();

    /// @param client_id empty for none: there is then nothing to switch on
    /// @param timing a test's; Discord's own pace otherwise
    DiscordPresence(
        Client& client, QString client_id,
        QString runtime_dir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation),
        DiscordIpc::Timing timing = {}, QObject* parent = nullptr);

    /// @brief There is an application to show it as
    bool available() const {
        return !this->client_id_.isEmpty();
    }
    /// @brief The switch in Settings; off by default
    void set_enabled(bool enabled);

private:
    /// @param moved the position within the track changed other than by playing on
    void update_(bool moved);

    Client& client_;
    const QString client_id_;
    const QString runtime_dir_;
    const DiscordIpc::Timing timing_;
    /// The connection, while switched on
    std::unique_ptr<DiscordIpc> ipc_;
    /// The track Discord shows; none while nothing plays
    Track shown_;
    QString shown_artwork_url_;
};
