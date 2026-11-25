#include "discord_presence.h"

#include <QDateTime>
#include <QHostAddress>
#include <QLoggingCategory>
#include <QUrl>

#include <algorithm>
#include <utility>

Q_DECLARE_LOGGING_CATEGORY(lc_discord)

namespace {

/// The image uploaded to the Discord application under this name
const QString LOGO_ASSET = QStringLiteral("sendspin");
constexpr int ACTIVITY_LISTENING = 2;
// Discord refuses an activity with a text outside these
constexpr qsizetype TEXT_MIN = 2;
constexpr qsizetype TEXT_MAX = 128;
constexpr qsizetype URL_MAX = 256;

QString fitted(const QString& text) {
    return text.left(TEXT_MAX).leftJustified(TEXT_MIN, QLatin1Char(' '));
}

}  // namespace

QString discord_artwork_url(const QString& url) {
    const QUrl parsed(url, QUrl::StrictMode);
    if (url.size() > URL_MAX || !parsed.isValid() || parsed.scheme() != QLatin1String("https") ||
        !parsed.userInfo().isEmpty()) {
        return {};
    }
    QString host = parsed.host().toLower();
    if (host.endsWith(QLatin1Char('.'))) {
        host.chop(1);
    }
    // Discord fetches it from the internet: an address or a name only this network knows, as
    // Music Assistant's own image proxy has, is out of its reach
    if (!host.contains(QLatin1Char('.')) || !QHostAddress(host).isNull()) {
        return {};
    }
    for (const char* suffix : {".local", ".lan", ".localhost", ".internal", ".home.arpa"}) {
        if (host.endsWith(QLatin1String(suffix))) {
            return {};
        }
    }
    return url;
}

QJsonObject discord_activity(const Track& track, const QString& artwork_url, qint64 position_ms,
                             qint64 now_ms) {
    QJsonObject activity{{QStringLiteral("type"), ACTIVITY_LISTENING},
                         {QStringLiteral("details"), fitted(track.title)}};
    if (!track.artist.isEmpty()) {
        activity[QStringLiteral("state")] = fitted(track.artist);
    }
    QJsonObject assets{{QStringLiteral("large_image"), LOGO_ASSET}};
    if (!artwork_url.isEmpty()) {
        assets[QStringLiteral("large_image")] = artwork_url;
        assets[QStringLiteral("small_image")] = LOGO_ASSET;
    }
    if (!track.album.isEmpty()) {
        assets[QStringLiteral("large_text")] = fitted(track.album);
    }
    activity[QStringLiteral("assets")] = assets;

    const bool has_end = track.duration_ms > 0;
    const qint64 start_ms =
        now_ms - (has_end ? std::clamp<qint64>(position_ms, 0, track.duration_ms)
                          : std::max<qint64>(position_ms, 0));
    QJsonObject timestamps{{QStringLiteral("start"), start_ms}};
    // Without one, as on a live stream, Discord counts up instead
    if (has_end) {
        timestamps[QStringLiteral("end")] = start_ms + track.duration_ms;
    }
    activity[QStringLiteral("timestamps")] = timestamps;
    return activity;
}

QString DiscordPresence::build_client_id() {
    return QStringLiteral(SENDSPIN_DISCORD_CLIENT_ID);
}

DiscordPresence::DiscordPresence(Client& client, QString client_id, QString runtime_dir,
                                 DiscordIpc::Timing timing, QObject* parent)
    : QObject(parent),
      client_(client),
      client_id_(std::move(client_id)),
      runtime_dir_(std::move(runtime_dir)),
      timing_(timing) {
    if (!this->available()) {
        qCInfo(lc_discord, "built without a Discord application ID: no Rich Presence");
    }
    connect(&client, &Client::stateChanged, this, [this] { this->update_(false); });
    connect(&client.now_playing(), &NowPlaying::changed, this, [this] { this->update_(false); });
    connect(&client.now_playing(), &NowPlaying::seeked, this, [this] { this->update_(true); });
}

void DiscordPresence::set_enabled(bool enabled) {
    if (!this->available() || enabled == (this->ipc_ != nullptr)) {
        return;
    }
    this->shown_ = {};
    this->shown_artwork_url_.clear();
    if (enabled) {
        this->ipc_ =
            std::make_unique<DiscordIpc>(this->client_id_, this->runtime_dir_, this->timing_);
        this->update_(false);
    } else {
        // Discord drops the activity with the connection
        this->ipc_.reset();
    }
}

void DiscordPresence::update_(bool moved) {
    if (!this->ipc_) {
        return;
    }
    const NowPlaying& now_playing = this->client_.now_playing();
    const bool plays = this->client_.state() == Client::State::Connected && now_playing.playing() &&
                       !now_playing.track().title.isEmpty();
    const Track track = plays ? now_playing.track() : Track{};
    const QString artwork_url = plays ? discord_artwork_url(now_playing.artwork_url()) : QString();
    // The volume changes too: the same track playing on is nothing new to Discord
    if (track == this->shown_ && artwork_url == this->shown_artwork_url_ && !(plays && moved)) {
        return;
    }
    this->shown_ = track;
    this->shown_artwork_url_ = artwork_url;
    this->ipc_->set_activity(plays ? discord_activity(track, artwork_url, now_playing.position_ms(),
                                                      QDateTime::currentMSecsSinceEpoch())
                                   : QJsonObject());
}
