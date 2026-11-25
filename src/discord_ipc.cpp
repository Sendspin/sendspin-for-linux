#include "discord_ipc.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLoggingCategory>
#include <QUuid>
#include <QtEndian>

#include <algorithm>
#include <utility>

Q_LOGGING_CATEGORY(lc_discord, "sendspin.discord")

namespace {

// A frame is an opcode and a length, both little-endian, and that much JSON
constexpr qint32 OP_HANDSHAKE = 0;
constexpr qint32 OP_FRAME = 1;
constexpr qint32 OP_CLOSE = 2;
constexpr qint32 OP_PING = 3;
constexpr qint32 OP_PONG = 4;
constexpr qint64 HEADER_BYTES = 8;
/// Discord's own frames are a few hundred bytes; more is not Discord
constexpr qint32 MAX_PAYLOAD_BYTES = 64 * 1024;
constexpr int SOCKETS_PER_DIR = 10;

}  // namespace

QStringList discord_socket_paths(const QString& runtime_dir) {
    QStringList paths;
    for (const char* dir : {"", "/app/com.discordapp.Discord", "/snap.discord"}) {
        for (int i = 0; i < SOCKETS_PER_DIR; i++) {
            paths << runtime_dir + QLatin1String(dir) + QStringLiteral("/discord-ipc-%1").arg(i);
        }
    }
    return paths;
}

DiscordIpc::DiscordIpc(QString client_id, QString runtime_dir, Timing timing, QObject* parent)
    : QObject(parent),
      client_id_(std::move(client_id)),
      runtime_dir_(std::move(runtime_dir)),
      timing_(timing),
      next_retry_(timing.retry) {
    this->retry_.setSingleShot(true);
    this->pace_.setSingleShot(true);
    this->handshake_.setSingleShot(true);
    this->handshake_.setInterval(timing.retry);
    connect(&this->handshake_, &QTimer::timeout, &this->socket_, &QLocalSocket::abort);
    this->unconnected_.setSingleShot(true);
    this->unconnected_.setInterval(0);
    connect(&this->retry_, &QTimer::timeout, this, &DiscordIpc::connect_first_);
    connect(&this->pace_, &QTimer::timeout, this, &DiscordIpc::flush_);
    connect(&this->unconnected_, &QTimer::timeout, this, &DiscordIpc::on_unconnected_);
    connect(&this->socket_, &QLocalSocket::connected, this, [this] {
        const QJsonObject hello{{QStringLiteral("v"), 1},
                                {QStringLiteral("client_id"), this->client_id_}};
        this->send_(OP_HANDSHAKE, QJsonDocument(hello).toJson(QJsonDocument::Compact));
        this->handshake_.start();
    });
    connect(&this->socket_, &QLocalSocket::readyRead, this, &DiscordIpc::on_ready_read_);
    connect(&this->socket_, &QLocalSocket::stateChanged, this,
            [this](QLocalSocket::LocalSocketState state) {
                if (state == QLocalSocket::UnconnectedState) {
                    this->unconnected_.start();
                }
            });
}

DiscordIpc::~DiscordIpc() {
    // Closing says nothing to what is already going
    this->socket_.disconnect(this);
}

void DiscordIpc::set_activity(const QJsonObject& activity) {
    this->wanted_ = activity;
    if (this->ready_) {
        this->flush_();
    } else if (activity.isEmpty()) {
        this->retry_.stop();
    } else if (this->socket_.state() == QLocalSocket::UnconnectedState &&
               !this->retry_.isActive() && !this->unconnected_.isActive()) {
        this->connect_first_();
    }
}

void DiscordIpc::connect_first_() {
    this->candidates_.clear();
    for (const QString& path : discord_socket_paths(this->runtime_dir_)) {
        if (QFileInfo::exists(path)) {
            this->candidates_ << path;
        }
    }
    this->connect_next_();
}

void DiscordIpc::connect_next_() {
    if (this->candidates_.isEmpty()) {
        // No Discord is the usual case: nothing to say about it
        this->retry_.start(this->next_retry_);
        this->next_retry_ = std::min(this->next_retry_ * 2, MAX_RETRY);
        return;
    }
    this->socket_.connectToServer(this->candidates_.takeFirst());
}

void DiscordIpc::on_unconnected_() {
    if (this->socket_.state() != QLocalSocket::UnconnectedState) {
        return;
    }
    const bool was_ready = std::exchange(this->ready_, false);
    this->handshake_.stop();
    this->sent_ = {};
    this->pace_.stop();
    if (this->wanted_.isEmpty()) {
        return;
    }
    if (was_ready) {
        qCInfo(lc_discord, "Discord went away");
        // It may be restarting: look again now, then with the usual patience
        this->connect_first_();
    } else {
        this->connect_next_();
    }
}

void DiscordIpc::on_ready_read_() {
    while (this->socket_.bytesAvailable() >= HEADER_BYTES) {
        char header[HEADER_BYTES];
        this->socket_.peek(header, HEADER_BYTES);
        const auto opcode = qFromLittleEndian<qint32>(header);
        const auto length = qFromLittleEndian<qint32>(header + 4);
        if (length < 0 || length > MAX_PAYLOAD_BYTES) {
            qCDebug(lc_discord, "a frame of %d bytes: not Discord", length);
            this->socket_.abort();
            return;
        }
        if (this->socket_.bytesAvailable() < HEADER_BYTES + length) {
            return;
        }
        this->socket_.skip(HEADER_BYTES);
        this->on_frame_(opcode, this->socket_.read(length));
    }
}

void DiscordIpc::on_frame_(qint32 opcode, const QByteArray& payload) {
    const QJsonObject message = QJsonDocument::fromJson(payload).object();
    switch (opcode) {
        case OP_FRAME: {
            const QString event = message.value(QStringLiteral("evt")).toString();
            if (event == QLatin1String("READY")) {
                qCInfo(lc_discord, "connected to Discord");
                this->ready_ = true;
                this->handshake_.stop();
                this->next_retry_ = this->timing_.retry;
                this->flush_();
            } else if (event == QLatin1String("ERROR")) {
                qCDebug(lc_discord, "Discord refused %s: %s",
                        qUtf8Printable(message.value(QStringLiteral("cmd")).toString()),
                        qUtf8Printable(message.value(QStringLiteral("data"))
                                           .toObject()
                                           .value(QStringLiteral("message"))
                                           .toString()));
            }
            break;
        }
        case OP_CLOSE:
            // An application ID it does not know, for one
            qCDebug(lc_discord, "Discord closed the connection: %s",
                    qUtf8Printable(message.value(QStringLiteral("message")).toString()));
            this->socket_.abort();
            break;
        case OP_PING:
            this->send_(OP_PONG, payload);
            break;
        default:
            break;
    }
}

void DiscordIpc::send_(qint32 opcode, const QByteArray& payload) {
    char header[HEADER_BYTES];
    qToLittleEndian<qint32>(opcode, header);
    qToLittleEndian<qint32>(static_cast<qint32>(payload.size()), header + 4);
    this->socket_.write(header, HEADER_BYTES);
    this->socket_.write(payload);
}

void DiscordIpc::flush_() {
    if (!this->ready_ || this->pace_.isActive() || this->wanted_ == this->sent_) {
        return;
    }
    const QJsonObject args{
        {QStringLiteral("pid"), QCoreApplication::applicationPid()},
        {QStringLiteral("activity"),
         this->wanted_.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(this->wanted_)}};
    const QJsonObject command{
        {QStringLiteral("cmd"), QStringLiteral("SET_ACTIVITY")},
        {QStringLiteral("args"), args},
        {QStringLiteral("nonce"), QUuid::createUuid().toString(QUuid::WithoutBraces)}};
    this->send_(OP_FRAME, QJsonDocument(command).toJson(QJsonDocument::Compact));
    this->sent_ = this->wanted_;
    this->pace_.start(this->timing_.pace);
}
