#include "track_announcer.h"

#include <QLoggingCategory>

#include <utility>

Q_LOGGING_CATEGORY(lc_announce, "sendspin.notify")

namespace {

/// Long enough for skipping through an album, and for the cover to follow its track
constexpr int SETTLE_MS = 1000;
/// About as long as a desktop shows a notification
constexpr int COVER_WAIT_MS = 5000;

}  // namespace

TrackAnnouncer::TrackAnnouncer(QObject* parent) : QObject(parent) {
    this->settle_.setSingleShot(true);
    this->settle_.setInterval(SETTLE_MS);
    this->settle_.setObjectName(QStringLiteral("settle"));
    this->cover_wait_.setSingleShot(true);
    this->cover_wait_.setInterval(COVER_WAIT_MS);
    this->cover_wait_.setObjectName(QStringLiteral("cover"));
    connect(&this->settle_, &QTimer::timeout, this, &TrackAnnouncer::announce_);
}

void TrackAnnouncer::set_enabled(bool enabled) {
    this->enabled_ = enabled;
    if (!enabled) {
        this->cover_wait_.stop();
    }
}

QString TrackAnnouncer::body(const Track& track) {
    if (track.artist.isEmpty() || track.album.isEmpty()) {
        return track.artist + track.album;
    }
    return tr("%1 — %2").arg(track.artist, track.album);
}

void TrackAnnouncer::update(bool playing, const Track& track, const QByteArray& artwork) {
    const Identity identity{track.title, track.artist, track.album};
    const bool another_track =
        identity != Identity{this->track_.title, this->track_.artist, this->track_.album};
    const bool another_cover = artwork != this->artwork_;
    if (another_cover) {
        this->artwork_is_the_tracks_ = true;
    } else if (another_track) {
        // The server sends a track before its cover: what is here is the last track's, and
        // right only for another track of that album
        this->artwork_is_the_tracks_ = this->artwork_is_the_tracks_ && !track.album.isEmpty() &&
                                       track.album == this->track_.album;
    }
    this->track_ = track;
    this->artwork_ = artwork;
    if (another_track) {
        this->cover_wait_.stop();
    } else if (another_cover && this->cover_wait_.isActive()) {
        const QImage cover = this->cover_();
        if (!cover.isNull()) {
            this->cover_wait_.stop();
            qCDebug(lc_announce, "the track's cover came: shown again with it");
            emit this->announced(track.title, body(track), cover);
        }
    }
    if (identity == Identity{}) {
        // Idle: the same track again is a new start
        this->announced_ = {};
    }
    // Paused on a track, the timer stops: a resume of a track not yet shown starts it again
    if (!playing || track.title.isEmpty() || identity == this->announced_) {
        this->settle_.stop();
        this->pending_ = {};
        return;
    }
    if (identity != this->pending_) {
        this->pending_ = identity;
        this->settle_.start();
    }
}

void TrackAnnouncer::announce_() {
    this->announced_ = std::exchange(this->pending_, {});
    // Kept up to date while off, so that switching it on mid-track shows nothing
    // Per track, so debug; never the title, which is the listener's business
    qCDebug(lc_announce, "a track is due: %s", this->enabled_ ? "shown" : "switched off");
    if (this->enabled_) {
        const QImage cover = this->cover_();
        if (cover.isNull()) {
            this->cover_wait_.start();
        }
        emit this->announced(this->track_.title, body(this->track_), cover);
    }
}

QImage TrackAnnouncer::cover_() const {
    return this->artwork_is_the_tracks_ ? QImage::fromData(this->artwork_, "JPEG") : QImage();
}
