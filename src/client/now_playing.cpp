#include "now_playing.h"

#include <algorithm>
#include <utility>

namespace {

constexpr uint16_t ARTWORK_SIZE = 512;
/// A 512-pixel JPEG stays well under this
constexpr uint32_t ARTWORK_MAX_BYTES = 1024 * 1024;

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
    : client_(client),
      controller_(client.add_controller()),
      // The cover comes through the artwork role
      metadata_(client.add_metadata({.artwork_url = false})) {
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
    const qint64 end = this->controls_.seek_max_ms.value_or(0);
    this->send({.command = Command::SEEK,
                .position_ms = static_cast<uint32_t>(std::clamp<qint64>(position_ms, 0, end))});
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
    track.year = metadata.year.value_or(0);
    track.duration_ms = metadata.progress ? metadata.progress->track_duration : 0;
    if (track != this->track_) {
        this->track_ = std::move(track);
        this->dirty_ = true;
    }
    // Every state restates the position
    this->seeked_ = true;
}

void NowPlaying::on_metadata_clear() {
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
