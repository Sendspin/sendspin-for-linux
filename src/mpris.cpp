#include "mpris.h"

#include "app_id.h"

#include <QCryptographicHash>
#include <QDBusAbstractAdaptor>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDir>
#include <QFile>
#include <QLoggingCategory>
#include <QSaveFile>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <utility>

Q_LOGGING_CATEGORY(lc_mpris, "sendspin.mpris")

namespace {

const QString OBJECT_PATH = QStringLiteral("/org/mpris/MediaPlayer2");
const QString PLAYER_INTERFACE = QStringLiteral("org.mpris.MediaPlayer2.Player");
const QString NO_TRACK = QStringLiteral("/org/mpris/MediaPlayer2/TrackList/NoTrack");

/// The server gives tracks no id: one from what identifies the track to the listener
QString track_id(const Track& track) {
    if (track == Track{}) {
        return NO_TRACK;
    }
    const QByteArray key =
        (track.title + QChar(0) + track.artist + QChar(0) + track.album).toUtf8();
    return QStringLiteral("/com/sendspin_audio/Sendspin/track/") +
           QString::fromLatin1(QCryptographicHash::hash(key, QCryptographicHash::Sha1).toHex());
}

}  // namespace

// Outside the unnamed namespace for moc
class MprisRootAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2")
    Q_PROPERTY(bool CanQuit READ no CONSTANT)
    Q_PROPERTY(bool CanRaise READ no CONSTANT)
    Q_PROPERTY(bool HasTrackList READ no CONSTANT)
    Q_PROPERTY(QString Identity READ identity CONSTANT)
    Q_PROPERTY(QString DesktopEntry READ desktopEntry CONSTANT)
    Q_PROPERTY(QStringList SupportedUriSchemes READ none CONSTANT)
    Q_PROPERTY(QStringList SupportedMimeTypes READ none CONSTANT)

public:
    explicit MprisRootAdaptor(QObject* parent) : QDBusAbstractAdaptor(parent) {}

    bool no() const {
        return false;
    }
    QString identity() const {
        return QStringLiteral("Sendspin");
    }
    QString desktopEntry() const {
        return QLatin1String(APP_ID);
    }
    QStringList none() const {
        return {};
    }

public slots:
    void Raise() {}
    void Quit() {}
};

class MprisPlayerAdaptor : public QDBusAbstractAdaptor {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.mpris.MediaPlayer2.Player")
    Q_PROPERTY(QString PlaybackStatus READ playbackStatus)
    Q_PROPERTY(double Rate READ one WRITE setRate)
    Q_PROPERTY(QVariantMap Metadata READ metadata)
    Q_PROPERTY(double Volume READ volume WRITE setVolume)
    Q_PROPERTY(qlonglong Position READ position)
    Q_PROPERTY(double MinimumRate READ one CONSTANT)
    Q_PROPERTY(double MaximumRate READ one CONSTANT)
    Q_PROPERTY(bool CanGoNext READ canGoNext)
    Q_PROPERTY(bool CanGoPrevious READ canGoPrevious)
    Q_PROPERTY(bool CanPlay READ canPlay)
    Q_PROPERTY(bool CanPause READ canPause)
    Q_PROPERTY(bool CanSeek READ canSeek)
    Q_PROPERTY(bool CanControl READ canControl CONSTANT)

public:
    explicit MprisPlayerAdaptor(Mpris* mpris) : QDBusAbstractAdaptor(mpris), mpris_(mpris) {
        connect(mpris, &Mpris::seeked, this, &MprisPlayerAdaptor::Seeked);
    }

    QString playbackStatus() const {
        return this->get_("PlaybackStatus").toString();
    }
    double one() const {
        return 1.0;
    }
    void setRate(double /*rate*/) {}
    QVariantMap metadata() const {
        return this->get_("Metadata").toMap();
    }
    double volume() const {
        return this->get_("Volume").toDouble();
    }
    void setVolume(double volume) {
        this->mpris_->set_volume(volume);
    }
    qlonglong position() const {
        return this->mpris_->position_us();
    }
    bool canGoNext() const {
        return this->get_("CanGoNext").toBool();
    }
    bool canGoPrevious() const {
        return this->get_("CanGoPrevious").toBool();
    }
    bool canPlay() const {
        return this->get_("CanPlay").toBool();
    }
    bool canPause() const {
        return this->get_("CanPause").toBool();
    }
    bool canSeek() const {
        return this->get_("CanSeek").toBool();
    }
    bool canControl() const {
        return true;
    }

public slots:
    void Next() {
        this->mpris_->command(NowPlaying::Command::NEXT);
    }
    void Previous() {
        this->mpris_->command(NowPlaying::Command::PREVIOUS);
    }
    void Pause() {
        this->mpris_->command(NowPlaying::Command::PAUSE);
    }
    void PlayPause() {
        this->mpris_->play_pause();
    }
    void Stop() {
        this->mpris_->command(NowPlaying::Command::STOP);
    }
    void Play() {
        this->mpris_->command(NowPlaying::Command::PLAY);
    }
    void Seek(qlonglong Offset) {
        this->mpris_->seek_by(Offset);
    }
    void SetPosition(const QDBusObjectPath& TrackId, qlonglong Position) {
        this->mpris_->set_position(TrackId.path(), Position);
    }
    void OpenUri(const QString& /*Uri*/) {}

signals:
    void Seeked(qlonglong Position);

private:
    QVariant get_(const char* name) const {
        return this->mpris_->properties().value(QLatin1String(name));
    }

    Mpris* mpris_;
};

QVariantMap mpris_metadata(const Track& track, const QString& art_url) {
    QVariantMap metadata;
    metadata[QStringLiteral("mpris:trackid")] =
        QVariant::fromValue(QDBusObjectPath(track_id(track)));
    if (track == Track{}) {
        return metadata;
    }
    const auto add = [&metadata](const char* key, const QVariant& value) {
        metadata[QLatin1String(key)] = value;
    };
    if (!track.title.isEmpty()) {
        add("xesam:title", track.title);
    }
    if (!track.artist.isEmpty()) {
        add("xesam:artist", QStringList{track.artist});
    }
    if (!track.album.isEmpty()) {
        add("xesam:album", track.album);
    }
    if (!track.album_artist.isEmpty()) {
        add("xesam:albumArtist", QStringList{track.album_artist});
    }
    if (track.number > 0) {
        add("xesam:trackNumber", track.number);
    }
    if (track.duration_ms > 0) {
        add("mpris:length", QVariant::fromValue<qlonglong>(track.duration_ms * 1000));
    }
    if (!art_url.isEmpty()) {
        add("mpris:artUrl", art_url);
    }
    return metadata;
}

QVariantMap mpris_properties(const MprisState& state) {
    const bool has_track = state.track != Track{};
    QVariantMap properties;
    properties[QStringLiteral("PlaybackStatus")] = state.playing ? QStringLiteral("Playing")
                                                   : has_track   ? QStringLiteral("Paused")
                                                                 : QStringLiteral("Stopped");
    properties[QStringLiteral("Metadata")] = mpris_metadata(state.track, state.art_url);
    properties[QStringLiteral("Volume")] = std::clamp(state.volume, 0, 100) / 100.0;
    // Playing must show the card even when the server lists no `play` next to its `pause`
    properties[QStringLiteral("CanPlay")] = state.playing || (has_track && state.can_play);
    properties[QStringLiteral("CanPause")] = state.can_pause;
    properties[QStringLiteral("CanGoNext")] = state.can_next;
    properties[QStringLiteral("CanGoPrevious")] = state.can_previous;
    properties[QStringLiteral("CanSeek")] = state.can_seek && state.track.duration_ms > 0;
    return properties;
}

ArtworkFile::ArtworkFile(QString dir) : dir_(std::move(dir)) {}

ArtworkFile::~ArtworkFile() {
    this->set({});
}

QString ArtworkFile::set(const QByteArray& image) {
    QString path;
    if (!image.isEmpty()) {
        const QByteArray hash = QCryptographicHash::hash(image, QCryptographicHash::Sha256);
        path = this->dir_ + QLatin1Char('/') + QString::fromLatin1(hash.first(16).toHex()) +
               QStringLiteral(".jpg");
    }
    if (path != this->path_) {
        if (!path.isEmpty()) {
            QSaveFile file(path);
            if (!QDir().mkpath(this->dir_) || !file.open(QIODevice::WriteOnly) ||
                file.write(image) != image.size() || !file.commit()) {
                qCWarning(lc_mpris, "cannot write the cover to %s", qUtf8Printable(path));
                path.clear();
            }
        }
        if (!this->path_.isEmpty()) {
            QFile::remove(this->path_);
        }
        this->path_ = path;
    }
    return path.isEmpty() ? QString() : QUrl::fromLocalFile(path).toString();
}

Mpris::Mpris(Client& client, const QString& art_dir, const QString& instance, QObject* parent)
    : QObject(parent),
      client_(client),
      service_(QStringLiteral("org.mpris.MediaPlayer2.") + QLatin1String(APP_ID) + instance),
      artwork_(art_dir) {
    new MprisRootAdaptor(this);
    new MprisPlayerAdaptor(this);
    this->properties_ = mpris_properties({});
    QDBusConnection::sessionBus().registerObject(OBJECT_PATH, this,
                                                 QDBusConnection::ExportAdaptors);

    connect(&client, &Client::stateChanged, this, &Mpris::update_);
    connect(&client.now_playing(), &NowPlaying::changed, this, &Mpris::update_);
    connect(&client.now_playing(), &NowPlaying::seeked, this, [this] {
        if (this->registered_) {
            emit this->seeked(this->position_us());
        }
    });
}

Mpris::~Mpris() {
    this->set_enabled(false);
    QDBusConnection::sessionBus().unregisterObject(OBJECT_PATH);
}

void Mpris::set_enabled(bool enabled) {
    this->enabled_ = enabled;
    this->update_();
}

qint64 Mpris::position_us() const {
    return this->client_.now_playing().position_ms() * 1000;
}

void Mpris::command(NowPlaying::Command command) {
    this->client_.now_playing().send({.command = command});
}

void Mpris::play_pause() {
    this->command(this->client_.now_playing().playing() ? NowPlaying::Command::PAUSE
                                                        : NowPlaying::Command::PLAY);
}

void Mpris::seek_by(qint64 offset_us) {
    NowPlaying& now_playing = this->client_.now_playing();
    now_playing.seek(now_playing.position_ms() + (offset_us / 1000));
}

void Mpris::set_position(const QString& track_id, qint64 position_us) {
    NowPlaying& now_playing = this->client_.now_playing();
    // A position for a track that has since ended is dropped, as MPRIS asks
    if (track_id == ::track_id(now_playing.track()) && position_us >= 0 &&
        position_us <= now_playing.track().duration_ms * 1000) {
        now_playing.seek(position_us / 1000);
    }
}

void Mpris::set_volume(double volume) {
    this->client_.now_playing().send(
        {.command = NowPlaying::Command::VOLUME,
         .volume = static_cast<uint8_t>(std::lround(std::clamp(volume, 0.0, 1.0) * 100.0))});
}

void Mpris::update_() {
    QDBusConnection bus = QDBusConnection::sessionBus();
    const bool wanted =
        this->enabled_ && bus.isConnected() && this->client_.state() == Client::State::Connected;
    if (wanted != this->registered_) {
        if (!wanted) {
            bus.unregisterService(this->service_);
            this->registered_ = false;
        } else if (bus.registerService(this->service_)) {
            this->registered_ = true;
        } else {
            qCWarning(lc_mpris, "cannot take %s on the session bus",
                      qUtf8Printable(this->service_));
        }
    }

    using Command = NowPlaying::Command;
    const NowPlaying& now_playing = this->client_.now_playing();
    MprisState state;
    if (this->registered_) {
        state = {.playing = now_playing.playing(),
                 .track = now_playing.track(),
                 .art_url = this->artwork_.set(now_playing.artwork()),
                 .volume = now_playing.volume(),
                 .can_play = now_playing.can(Command::PLAY),
                 .can_pause = now_playing.can(Command::PAUSE),
                 .can_next = now_playing.can(Command::NEXT),
                 .can_previous = now_playing.can(Command::PREVIOUS),
                 .can_seek = now_playing.can(Command::SEEK)};
    } else {
        this->artwork_.set({});
    }

    const QVariantMap properties = mpris_properties(state);
    QVariantMap changed;
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (this->properties_.value(it.key()) != it.value()) {
            changed[it.key()] = it.value();
        }
    }
    this->properties_ = properties;
    if (this->registered_ && !changed.isEmpty()) {
        QDBusMessage signal = QDBusMessage::createSignal(
            OBJECT_PATH, QStringLiteral("org.freedesktop.DBus.Properties"),
            QStringLiteral("PropertiesChanged"));
        signal << PLAYER_INTERFACE << changed << QStringList();
        bus.send(signal);
    }
}

#include "mpris.moc"
