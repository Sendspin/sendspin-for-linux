#include "track_announcer.h"

#include <utility>

namespace {

/// Long enough for skipping through an album, and for the cover to follow its track
constexpr int SETTLE_MS = 1000;

}  // namespace

TrackAnnouncer::TrackAnnouncer(QObject* parent) : QObject(parent) {
    this->settle_.setSingleShot(true);
    this->settle_.setInterval(SETTLE_MS);
    connect(&this->settle_, &QTimer::timeout, this, &TrackAnnouncer::announce);
}

void TrackAnnouncer::set_enabled(bool enabled) {
    this->enabled_ = enabled;
}

QString TrackAnnouncer::body(const Track& track) {
    if (track.artist.isEmpty() || track.album.isEmpty()) {
        return track.artist + track.album;
    }
    return tr("%1 — %2").arg(track.artist, track.album);
}

void TrackAnnouncer::update(bool playing, const Track& track, const QByteArray& artwork) {
    this->track_ = track;
    this->artwork_ = artwork;
    const Identity identity{track.title, track.artist, track.album};
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

void TrackAnnouncer::announce() {
    this->announced_ = std::exchange(this->pending_, {});
    // Kept up to date while off, so that switching it on mid-track shows nothing
    if (this->enabled_) {
        emit this->announced(this->track_.title, body(this->track_),
                             QImage::fromData(this->artwork_, "JPEG"));
    }
}
