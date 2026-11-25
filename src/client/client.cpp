#include "client.h"

#include "audio/player_formats.h"

#include <QCryptographicHash>
#include <QLoggingCategory>
#include <QTcpServer>

#include <chrono>
#include <utility>

Q_LOGGING_CATEGORY(lc_client, "sendspin.client")

namespace {

// How often the library's callbacks are delivered; its connections do not wait for this
constexpr auto LOOP_INTERVAL = std::chrono::milliseconds(20);

sendspin::SendspinClientConfig library_config(const Client::Config& config) {
    sendspin::SendspinClientConfig result;
    result.name = config.name.toStdString();
    result.product_name = "Sendspin for Linux";
    result.server_port = config.port;
    // The code is shown on screen and typed into the server. No QR code: the person pairing
    // sits at this PC. Offering a dynamic code rules out a static one.
    result.pairing_code_out_channels = {sendspin::SendspinPairingCodeChannel::DISPLAY};
    result.pairing_code_formats = {sendspin::SendspinPairingCodeFormat::DIGITS};
    return result;
}

}  // namespace

uint16_t Client::free_port(uint16_t preferred) {
    QTcpServer probe;
    if (preferred != 0 && probe.listen(QHostAddress::Any, preferred)) {
        return preferred;
    }
    return probe.listen(QHostAddress::Any, 0) ? probe.serverPort() : preferred;
}

Client::Client(const Config& config, sendspin::SendspinPersistenceProvider* persistence,
               QObject* parent)
    : QObject(parent),
      name_(config.name),
      port_(config.port),
      advertise_(config.advertise),
      client_(library_config(config)),
      source_(this->client_),
      player_(this->client_,
              config.player_formats.empty() ? player_formats({}, {}) : config.player_formats,
              config.player_volume, config.player_muted),
      now_playing_(this->client_) {
    this->client_.set_listener(this);
    this->client_.set_network_provider(this);
    this->client_.set_persistence_provider(persistence);
    // The library does not keep this; before start(), so the first hello carries it
    this->client_.set_unpaired_access_enabled(config.unpaired_access);

    this->loop_timer_.setInterval(LOOP_INTERVAL);
    connect(&this->loop_timer_, &QTimer::timeout, this, &Client::tick_);
}

Client::~Client() {
    // Whoever listens is being torn down with this
    this->blockSignals(true);
    this->stop();
}

bool Client::start() {
    if (this->client_.is_started()) {
        return true;
    }
    if (!this->client_.start()) {
        qCWarning(lc_client, "the Sendspin client did not start");
        return false;
    }
    qCInfo(lc_client, "listening on port %u as %s", this->port_, qUtf8Printable(this->client_id()));
    if (this->advertise_) {
        // A host name of the client's own, stable for as long as its identity is
        const QByteArray digest =
            QCryptographicHash::hash(this->client_id().toLatin1(), QCryptographicHash::Sha256);
        this->advertiser_.start(this->name_, "sendspin-" + digest.first(4).toHex(), this->port_);
    }
    this->loop_timer_.start();
    this->sync_();
    return true;
}

void Client::stop() {
    if (!this->client_.is_started()) {
        return;
    }
    this->loop_timer_.stop();
    this->advertiser_.stop();
    // Withdraws a code still showing, through the listener
    this->client_.stop();
    this->player_.stop();
    this->now_playing_.poll();
    this->pairing_ = false;
    this->sync_();
}

QString Client::pairing_token() const {
    const auto token = this->client_.client_pairing_token();
    return token ? QString::fromLatin1(token->data(), static_cast<qsizetype>(token->size()))
                 : QString();
}

QString Client::client_id() const {
    return QString::fromStdString(this->client_.client_id());
}

void Client::allow_pairing() {
    this->client_.confirm_pairing_window();
    this->window_wanted_ = false;
    this->sync_();
}

void Client::tick_() {
    this->client_.loop();
    this->source_.poll();
    this->player_.poll();
    this->now_playing_.poll();
    this->sync_();
}

/// Signals what changed since the last call. Outside the library's calls, so slots are free to
/// call back in.
void Client::sync_() {
    const bool started = this->client_.is_started();
    const bool connected = started && this->client_.is_connected();
    QString server_name;
    if (connected) {
        if (const auto server = this->client_.server_information()) {
            server_name =
                QString::fromUtf8(server->name.data(), static_cast<qsizetype>(server->name.size()));
        } else {
            // The name trails the connection by a tick
            server_name = this->server_name_;
        }
    }

    // A pairing a server abandons by dropping the connection reports no end
    if (this->pairing_) {
        if (connected) {
            this->pairing_connected_ = true;
        } else if (this->pairing_connected_) {
            this->pairing_ = false;
            this->ended_event_ = true;
        }
    }

    State state = State::Stopped;
    if (started) {
        state = this->pairing_ ? State::Pairing : connected ? State::Connected : State::Waiting;
    }

    if (server_name != this->server_name_) {
        this->server_name_ = server_name;
        emit this->serverNameChanged(server_name);
    }
    if (state != this->state_) {
        this->state_ = state;
        emit this->stateChanged(state);
    }
    if (this->code_ != this->pairing_code_) {
        this->pairing_code_ = this->code_;
        emit this->pairingCodeChanged(this->pairing_code_);
    }
    // The library keeps its window open until the attempt ends; the question is answered once
    // the code shows
    const bool approval_needed = this->window_wanted_ && this->code_.isEmpty();
    if (approval_needed != this->approval_needed_) {
        this->approval_needed_ = approval_needed;
        emit this->pairingApprovalNeededChanged(this->approval_needed_);
    }
    if (std::exchange(this->paired_event_, false)) {
        emit this->paired(this->server_name_);
    }
    if (std::exchange(this->ended_event_, false)) {
        emit this->pairingEnded();
    }
}

void Client::on_pairing_attempt(std::string_view /*server_id*/) {
    this->pairing_ = true;
    this->pairing_connected_ = false;
}

void Client::on_paired(std::string_view /*server_id*/) {
    this->pairing_ = false;
    this->paired_event_ = true;
}

void Client::on_pairing_aborted(std::string_view /*server_id*/,
                                sendspin::SendspinPairAbortReason reason) {
    qCInfo(lc_client, "pairing ended, reason %d", static_cast<int>(reason));
    this->pairing_ = false;
    this->ended_event_ = true;
}

void Client::on_pairing_code(std::string_view code,
                             sendspin::SendspinPairingCodeFormat /*format*/) {
    // Only DIGITS is offered
    this->code_ = QString::fromLatin1(code.data(), static_cast<qsizetype>(code.size()));
}

void Client::on_clear_pairing_code() {
    this->code_.clear();
    // The end of a code attempt, also when the server just moves on with a server/activate,
    // which reports neither paired nor aborted
    this->pairing_ = false;
}

void Client::on_open_pairing_window() {
    this->window_wanted_ = true;
}

void Client::on_close_pairing_window() {
    this->window_wanted_ = false;
    // As above, for an attempt that never got to show its code
    if (this->code_.isEmpty()) {
        this->pairing_ = false;
    }
}
