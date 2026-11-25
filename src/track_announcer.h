#pragma once

#include "client/now_playing.h"

#include <QByteArray>
#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>

/// @brief Says when a track a server plays to this PC is worth a notification: when playing
/// starts, and when the track changes while it plays. Not on a pause, a resume or a seek, and
/// once for a run of skips. The cover is only ever the track's own: one that arrives after its
/// track was announced is announced once more, with it.
class TrackAnnouncer : public QObject {
    Q_OBJECT

public:
    explicit TrackAnnouncer(QObject* parent = nullptr);

    /// @brief The switch in Settings; off by default
    void set_enabled(bool enabled);
    /// @brief What the group plays now. On every change of it, and any number of times between.
    /// @param artwork The cover, a JPEG; empty for none
    void update(bool playing, const Track& track, const QByteArray& artwork);

    /// @brief `Artist — Album`, or the one there is
    static QString body(const Track& track);

signals:
    /// A track to show, or the one just shown again with its cover
    /// @param cover null for none, or for one not known to be this track's
    void announced(const QString& title, const QString& body, const QImage& cover);

private:
    /// What makes a track another track to the listener
    struct Identity {
        QString title{};
        QString artist{};
        QString album{};
        bool operator==(const Identity&) const = default;
    };
    void announce_();
    /// The cover as an image; null unless it is the track's own
    QImage cover_() const;

    bool enabled_{false};
    /// Waits for a run of skips to settle on a track
    QTimer settle_{this};
    /// While it runs, the track just announced without a cover is announced again when its
    /// cover comes: later the notification has gone, and would come back
    QTimer cover_wait_{this};
    Track track_;
    QByteArray artwork_;
    /// The cover came with this track or after it, or with an earlier track of its album: one
    /// left over from another album is not shown beside this title
    bool artwork_is_the_tracks_{false};
    /// The track the timer waits on; empty for none
    Identity pending_;
    /// The last track that was due, shown or not: a resume of it is no news
    Identity announced_;
};
