#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <sendspin/artwork_role.h>
#include <sendspin/client.h>
#include <sendspin/controller_role.h>
#include <sendspin/metadata_role.h>

#include <mutex>

/// @brief The track the group is on, as the server describes it
struct Track {
    QString title{};
    QString artist{};
    QString album{};
    QString album_artist{};
    int number{0};          ///< on its album; 0 for unknown
    int year{0};            ///< 0 for unknown
    qint64 duration_ms{0};  ///< 0 for unknown, or live

    bool operator==(const Track&) const = default;
};

/// @brief What the client's group plays and what may be done about it: the controller, metadata
/// and artwork roles. Commands act on the group, and only those the server lists are sent.
///
/// Signals are emitted from poll(), never from inside a library callback.
class NowPlaying : public QObject,
                   private sendspin::ControllerRoleListener,
                   private sendspin::MetadataRoleListener,
                   private sendspin::ArtworkRoleListener {
    Q_OBJECT

public:
    using Command = sendspin::SendspinControllerCommand;

    /// @brief Adds the roles to client. Before client.start(); client outlives this.
    explicit NowPlaying(sendspin::SendspinClient& client);

    /// @brief Signals what changed. Main loop, every tick.
    void poll();

    /// @brief The group is playing
    bool playing() const {
        return this->playing_;
    }
    const Track& track() const {
        return this->track_;
    }
    /// @brief The album cover, a JPEG; empty for none
    const QByteArray& artwork() const {
        return this->artwork_;
    }
    /// @brief Where the track is, moving on by itself while it plays
    qint64 position_ms() const;
    /// @brief The group's volume, 0–100
    int volume() const {
        return this->controls_.volume;
    }

    /// @brief The server lists the command
    bool can(Command command) const;
    /// @brief Sends a command the server lists; any other is dropped
    void send(const sendspin::ClientCommandControllerObject& command);
    /// @brief Moves the track to a position, kept inside what the server allows
    void seek(qint64 position_ms);

signals:
    /// The track, the artwork, the volume, the commands or playing() changed
    void changed();
    /// The server moved the position, other than by playing on
    void seeked();

private:
    void on_controller_state(const sendspin::ServerStateControllerObject& state) override;
    void on_controller_state_clear() override;
    void on_track_metadata(const sendspin::TrackMetadata& metadata) override;
    void on_metadata_clear() override;
    void on_image_decode(uint8_t slot, const uint8_t* data, size_t length,
                         sendspin::SendspinImageFormat format) override;
    void on_image_display(uint8_t slot, uint32_t lateness_ms) override;
    void on_image_clear(uint8_t slot) override;

    sendspin::SendspinClient& client_;
    sendspin::ControllerRole& controller_;
    sendspin::MetadataRole& metadata_;

    bool playing_{false};
    Track track_;
    QByteArray artwork_;
    sendspin::ServerStateControllerObject controls_;
    bool dirty_{false};
    bool seeked_{false};

    /// The image that arrived, on the library's decode thread, until it is due
    std::mutex pending_mutex_;
    QByteArray pending_artwork_;
};
