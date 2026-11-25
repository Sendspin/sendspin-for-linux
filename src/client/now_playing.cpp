#include "now_playing.h"

#include <algorithm>
#include <cstdlib>
#include <utility>

namespace {

constexpr uint16_t ARTWORK_SIZE = 512;
/// A 512-pixel JPEG stays well under this
constexpr uint32_t ARTWORK_MAX_BYTES = 1024 * 1024;
/// A position this far from where playing on would have put it was moved
constexpr int64_t SEEK_JUMP_MS = 1000;

sendspin::ArtworkRoleConfig artwork_config() {
    sendspin::ArtworkRoleConfig config;
    config.preferred_formats = {{.source = sendspin::SendspinImageSource::ALBUM,
                                 .format = sendspin::SendspinImageFormat::JPEG,
                                 .width = ARTWORK_SIZE,
                                 .height = ARTWORK_SIZE,
                                 .max_image_bytes = ARTWORK_MAX_BYTES}};
    return config;
}

QString text(const std::optional<std::string_view>& field) {
    return field ? QString::fromUtf8(field->data(), static_cast<qsizetype>(field->size()))
                 : QString();
}

}  // namespace

NowPlaying::NowPlaying(sendspin::SendspinClient& client)
    : client_(client), controller_(client.add_controller()), metadata_(client.add_metadata({})) {
    this->controller_.set_listener(this);
    this->metadata_.set_listener(this);
    client.add_artwork(artwork_config()).set_listener(this);
}

void NowPlaying::poll() {
    const bool playing =
        this->client_.is_connected() &&
        this->client_.group_state().playback_state == sendspin::SendspinPlaybackState::PLAYING;
    if (playing != this->playing_) {
        this->playing_ = playing;
        this->dirty_ = true;
    }
    if (std::exchange(this->dirty_, false)) {
        emit this->changed();
    }
    if (std::exchange(this->seeked_, false)) {
        emit this->seeked();
    }
}

qint64 NowPlaying::position_ms() const {
    return this->metadata_.get_track_progress_ms();
}

bool NowPlaying::can(Command command) const {
    const auto& supported = this->controls_.supported_commands;
    return std::find(supported.begin(), supported.end(), command) != supported.end();
}

void NowPlaying::send(const sendspin::ClientCommandControllerObject& command) {
    if (this->can(command.command)) {
        this->controller_.send_command(command);
    }
}

void NowPlaying::seek(qint64 position_ms) {
    // No range, as on a live stream: nowhere to seek to
    if (const auto end = this->controls_.seek_max_ms) {
        this->send(
            {.command = Command::SEEK,
             .position_ms = static_cast<uint32_t>(std::clamp<qint64>(position_ms, 0, *end))});
    }
}

void NowPlaying::on_controller_state(const sendspin::ServerStateControllerObject& state) {
    this->controls_ = state;
    this->dirty_ = true;
}

void NowPlaying::on_controller_state_clear() {
    this->controls_ = {};
    this->dirty_ = true;
}

void NowPlaying::on_track_metadata(const sendspin::TrackMetadata& metadata) {
    Track track;
    track.title = text(metadata.title);
    track.artist = text(metadata.artist);
    track.album = text(metadata.album);
    track.album_artist = text(metadata.album_artist);
    track.number = metadata.track.value_or(0);
    track.duration_ms = metadata.progress ? metadata.progress->track_duration : 0;
    // Where the track would be had it only played on from the last state
    const int64_t now_us = sendspin::SendspinClient::now_us();
    const bool same_track = track == this->track_;
    if (same_track && this->progress_ && metadata.progress) {
        const int64_t expected_ms =
            this->progress_->track_progress +
            ((now_us - this->progress_us_) * this->progress_->playback_speed / 1'000'000);
        if (std::abs(static_cast<int64_t>(metadata.progress->track_progress) - expected_ms) >
            SEEK_JUMP_MS) {
            this->seeked_ = true;
        }
    }
    this->progress_ = metadata.progress;
    this->progress_us_ = now_us;
    if (!same_track) {
        this->track_ = std::move(track);
        this->dirty_ = true;
    }
    if (const QString url = text(metadata.artwork_url); url != this->artwork_url_) {
        this->artwork_url_ = url;
        this->dirty_ = true;
    }
}

void NowPlaying::on_metadata_clear() {
    this->progress_.reset();
    if (!this->artwork_url_.isEmpty()) {
        this->artwork_url_.clear();
        this->dirty_ = true;
    }
    if (this->track_ != Track{}) {
        this->track_ = {};
        this->dirty_ = true;
    }
}

void NowPlaying::on_image_decode(uint8_t /*slot*/, const uint8_t* data, size_t length,
                                 sendspin::SendspinImageFormat /*format*/) {
    // Only JPEG is asked for, and it is passed on as it came
    QByteArray image(reinterpret_cast<const char*>(data), static_cast<qsizetype>(length));
    const std::lock_guard lock(this->pending_mutex_);
    this->pending_artwork_ = std::move(image);
}

void NowPlaying::on_image_display(uint8_t /*slot*/, uint32_t /*lateness_ms*/) {
    const std::lock_guard lock(this->pending_mutex_);
    if (!this->pending_artwork_.isEmpty()) {
        this->artwork_ = std::exchange(this->pending_artwork_, {});
        this->dirty_ = true;
    }
}

void NowPlaying::on_image_clear(uint8_t /*slot*/) {
    if (!this->artwork_.isEmpty()) {
        this->artwork_.clear();
        this->dirty_ = true;
    }
}
