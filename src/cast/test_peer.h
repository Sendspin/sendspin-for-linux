#pragma once

#include "persistence.h"

#include <QObject>
#include <QString>
#include <QTcpServer>
#include <QTimer>

#include <sendspin/client.h>
#include <sendspin/player_role.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

// For the tests that need a speaker: no part of the app

inline uint16_t free_port() {
    QTcpServer server;
    server.listen(QHostAddress::LocalHost, 0);
    return server.serverPort();
}

/// A Sendspin speaker: sendspin-cpp's client with a player role, listening on a local port and
/// counting what it is given to play.
class Peer : public sendspin::PlayerRoleListener,
             public sendspin::SendspinClientListener,
             public sendspin::SendspinNetworkProvider {
public:
    enum class Offer { DynamicCode, StaticCode, TokenOnly };
    static constexpr const char* STATIC_CODE = "49302718";

    /// @param dir where it keeps its identity, so a second Peer there is the same speaker
    /// @param offer the code it pairs by; it takes its token whichever that is
    Peer(const QString& dir, uint16_t port, bool unpaired_access, std::string name = "Kitchen",
         Offer offer = Offer::DynamicCode)
        : persistence_(dir.toStdString()), client_(config(port, std::move(name), offer)) {
        this->client_.set_persistence_provider(&this->persistence_);
        this->client_.set_network_provider(this);
        this->client_.set_listener(this);
        this->client_.set_unpaired_access_enabled(unpaired_access);
        sendspin::PlayerRoleConfig player;
        player.audio_formats = {{sendspin::SendspinCodecFormat::PCM, 2, 48000, 16}};
        this->player_ = &this->client_.add_player(std::move(player));
        this->player_->set_listener(this);
        this->started = this->client_.start();
        QObject::connect(&this->pump_, &QTimer::timeout, [this] { this->client_.loop(); });
        this->pump_.start(2);
    }

    ~Peer() override {
        this->pump_.stop();
        this->client_.stop();
    }

    QString id() const {
        return QString::fromStdString(this->client_.client_id());
    }

    /// Its pairing token, as its leaflet would carry it
    QString token() const {
        const auto token = this->client_.client_pairing_token();
        return token ? QString::fromLatin1(token->data(), static_cast<qsizetype>(token->size()))
                     : QString();
    }

    /// As its own button would, when it waits for one before it pairs
    void allow_pairing() {
        this->client_.confirm_pairing_window();
    }

    /// As its own button would: client/leave
    void leave() {
        this->client_.leave();
    }

    // An audio device with a short buffer: it takes audio no faster than it plays it, and says
    // when each block will have played. Sync task thread.
    size_t on_audio_output(std::span<const uint8_t> pcm, const sendspin::AudioFormat& format,
                           uint32_t /*timeout_ms*/) override {
        constexpr int64_t BUFFER_US = 20000;
        uint64_t loud = 0;
        for (size_t i = 0; i + 1 < pcm.size(); i += 2) {
            loud += (pcm[i] != 0 || pcm[i + 1] != 0) ? 1 : 0;
        }
        this->loud_samples += loud;

        const auto frames = static_cast<uint32_t>(
            pcm.size() / (static_cast<size_t>(format.channels) * (format.bits_per_sample / 8)));
        const int64_t now_us = sendspin::SendspinClient::now_us();
        this->played_until_us_ = std::max(this->played_until_us_, now_us) +
                                 (int64_t{frames} * 1'000'000 / format.sample_rate);
        this->player_->notify_audio_played(frames, this->played_until_us_);
        if (this->played_until_us_ - now_us > BUFFER_US) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(this->played_until_us_ - now_us - BUFFER_US));
        }
        return pcm.size();
    }
    void on_stream_start() override {
        this->streams_started++;
    }
    void on_stream_end() override {
        this->streams_ended++;
    }
    void on_volume_changed(uint8_t value) override {
        this->volume = value;
    }
    void on_mute_changed(bool value) override {
        this->muted = value;
    }
    void on_pairing_code(std::string_view code, sendspin::SendspinPairingCodeFormat) override {
        this->pairing_code = QString::fromUtf8(code.data(), static_cast<qsizetype>(code.size()));
    }
    void on_open_pairing_window() override {
        this->waits_for_button = true;
    }
    bool is_network_ready() override {
        return true;
    }

    bool started{false};
    std::atomic<uint64_t> loud_samples{0};
    int streams_started{0};
    int streams_ended{0};
    int volume{-1};
    bool muted{false};
    QString pairing_code;
    bool waits_for_button{false};

private:
    static sendspin::SendspinClientConfig config(uint16_t port, std::string name, Offer offer) {
        sendspin::SendspinClientConfig config;
        config.name = std::move(name);
        config.server_port = port;
        if (offer == Offer::DynamicCode) {
            config.pairing_code_out_channels = {sendspin::SendspinPairingCodeChannel::DISPLAY};
            config.pairing_code_formats = {sendspin::SendspinPairingCodeFormat::DIGITS};
        } else if (offer == Offer::StaticCode) {
            config.static_pairing_code = STATIC_CODE;
        }
        return config;
    }

    ClientPersistence persistence_;
    sendspin::SendspinClient client_;
    sendspin::PlayerRole* player_{nullptr};
    int64_t played_until_us_{0};  ///< sync task thread only
    QTimer pump_;
};
