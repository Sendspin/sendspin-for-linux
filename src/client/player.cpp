#include "player.h"

#include "audio/volume_curve.h"

#include <utility>

namespace {

sendspin::PlayerRoleConfig role_config(std::vector<sendspin::AudioSupportedFormatObject> formats) {
    sendspin::PlayerRoleConfig config;
    config.audio_formats = std::move(formats);
    return config;
}

}  // namespace

Player::Player(sendspin::SendspinClient& client,
               std::vector<sendspin::AudioSupportedFormatObject> formats)
    : role_(client.add_player(role_config(std::move(formats)))) {
    this->role_.set_listener(this);
    this->stream_.set_played([this](uint32_t frames, int64_t finish_us) {
        this->role_.notify_audio_played(frames, finish_us);
    });

    // The OS mixer moved the stream's volume: that is the player's volume now
    connect(&this->stream_, &PlaybackStream::gainChanged, this, [this](float gain) {
        const uint8_t volume = amplitude_to_volume(gain);
        if (volume != this->role_.get_volume()) {
            this->role_.update_volume(volume);
        }
    });
    connect(&this->stream_, &PlaybackStream::mutedChanged, this, [this](bool muted) {
        if (muted != this->role_.get_muted()) {
            this->role_.update_muted(muted);
        }
    });
}

void Player::poll() {
    if (this->started_ != this->streaming_) {
        this->streaming_ = this->started_;
        emit this->streamingChanged(this->streaming_);
    }
}

void Player::stop() {
    // A stopped client delivers no stream end
    this->started_ = false;
    this->stream_.stop();
    this->poll();
}

size_t Player::on_audio_output(std::span<const uint8_t> pcm, const sendspin::AudioFormat& format,
                               uint32_t timeout_ms) {
    return this->stream_.write(pcm.data(), pcm.size(),
                               {format.sample_rate, format.channels, format.bits_per_sample},
                               timeout_ms);
}

void Player::on_stream_start() {
    this->started_ = true;
    // The volume kept from the last run, or set while nothing played
    this->stream_.set_gain(volume_to_amplitude(this->role_.get_volume()));
    this->stream_.set_muted(this->role_.get_muted());
}

void Player::on_stream_end() {
    this->started_ = false;
    this->stream_.clear();
}

void Player::on_volume_changed(uint8_t volume) {
    this->stream_.set_gain(volume_to_amplitude(volume));
}

void Player::on_mute_changed(bool muted) {
    this->stream_.set_muted(muted);
}
