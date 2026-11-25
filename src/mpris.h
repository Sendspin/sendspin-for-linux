#pragma once

#include "client/client.h"

#include <QObject>
#include <QString>
#include <QVariantMap>

/// @brief What MPRIS shows of the group the client is in
struct MprisState {
    bool playing{false};
    Track track{};
    QString art_url{};  ///< the cover as a file:// URL; empty for none
    int volume{0};      ///< the group's, 0–100
    // What the server lists
    bool can_play{false};
    bool can_pause{false};
    bool can_next{false};
    bool can_previous{false};
    bool can_seek{false};
};

/// @brief The `Metadata` property for a track: only `mpris:trackid`, NoTrack, for none
QVariantMap mpris_metadata(const Track& track, const QString& art_url);

/// @brief Every property of org.mpris.MediaPlayer2.Player but `Position`.
/// `CanPlay` is false while the group is idle, stopped with no track, which is what hides the
/// card in GNOME; stopped on a track is `Paused`, and can play if the server lists it.
QVariantMap mpris_properties(const MprisState& state);

/// @brief The cover as a file other apps can read, named by a hash of its bytes so a new cover
/// is a new URL. One file at a time; gone with this.
class ArtworkFile {
public:
    explicit ArtworkFile(QString dir);
    ~ArtworkFile();
    ArtworkFile(const ArtworkFile&) = delete;
    ArtworkFile& operator=(const ArtworkFile&) = delete;

    /// @param image A JPEG; empty for none
    /// @return Its file:// URL; empty for none, or when it could not be written
    QString set(const QByteArray& image);

private:
    const QString dir_;
    QString path_;
};

/// @brief The desktop's media controls for what a server plays to this PC: an MPRIS player on
/// the session bus while a server is connected. Does nothing without a session bus.
class Mpris : public QObject {
    Q_OBJECT

public:
    /// @param art_dir Where the cover is written; created when there is one
    /// @param instance Appended to the bus name, as MPRIS has it for a second instance; for tests
    Mpris(Client& client, const QString& art_dir, const QString& instance = {},
          QObject* parent = nullptr);
    ~Mpris() override;

    /// @brief The switch in Settings; on by default
    void set_enabled(bool enabled);

    // For the D-Bus adaptors
    const QVariantMap& properties() const {
        return this->properties_;
    }
    qint64 position_us() const;
    void command(NowPlaying::Command command);
    void play_pause();
    void seek_by(qint64 offset_us);
    void set_position(const QString& track_id, qint64 position_us);
    void set_volume(double volume);

signals:
    void seeked(qint64 position_us);

private:
    void update_();

    Client& client_;
    const QString service_;
    ArtworkFile artwork_;
    bool enabled_{true};
    bool registered_{false};
    QVariantMap properties_;
};
