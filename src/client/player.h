#pragma once

#include "audio/playback_stream.h"

#include <QObject>

#include <sendspin/client.h>
#include <sendspin/player_role.h>

#include <vector>

/// @brief The client's player role: what the server plays to this PC, written to the playback
/// stream. The player's volume and mute are the stream's in the OS mixer, both ways, by the
/// spec's curve.
class Player : public QObject, private sendspin::PlayerRoleListener {
    Q_OBJECT

public:
    /// @brief Adds the role to client. Before client.start(); client outlives this.
    /// @param formats What is advertised, the preferred one first
    Player(sendspin::SendspinClient& client,
           std::vector<sendspin::AudioSupportedFormatObject> formats);

    PlaybackStream& stream() {
        return this->stream_;
    }

    /// @brief The server has a stream open to this PC
    bool streaming() const {
        return this->streaming_;
    }

    /// @brief Signals what changed. Main loop, every tick, outside the library's calls.
    void poll();
    /// @brief The client stopped: closes the stream
    void stop();

signals:
    void streamingChanged(bool streaming);

private:
    size_t on_audio_output(std::span<const uint8_t> pcm, const sendspin::AudioFormat& format,
                           uint32_t timeout_ms) override;
    void on_stream_start() override;
    void on_stream_end() override;
    void on_volume_changed(uint8_t volume) override;
    void on_mute_changed(bool muted) override;

    sendspin::PlayerRole& role_;
    PlaybackStream stream_;
    bool started_{false};    ///< written in the library's callbacks
    bool streaming_{false};  ///< what was last signalled
};
