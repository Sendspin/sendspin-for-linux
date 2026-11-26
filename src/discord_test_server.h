#pragma once

#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QtEndian>

/// @brief A Discord client for tests: its socket, what it was sent, and the READY it answers a
/// handshake with
class DiscordTestServer {
public:
    struct Frame {
        qint32 opcode;
        QJsonObject payload;
    };

    DiscordTestServer() {
        QObject::connect(&this->server_, &QLocalServer::newConnection, &this->server_, [this] {
            this->connections++;
            this->client_ = this->server_.nextPendingConnection();
            QObject::connect(this->client_, &QLocalSocket::readyRead, &this->server_,
                             [this] { this->read_(); });
        });
    }

    /// @param path the socket, as `discord_socket_paths()` names one
    bool listen(const QString& path) {
        return QDir().mkpath(QFileInfo(path).path()) && this->server_.listen(path);
    }
    /// @brief Discord quits: its connection and its socket go
    void close() {
        delete this->client_;
        this->server_.close();
    }
    bool connected() const {
        return this->client_ != nullptr && this->client_->state() == QLocalSocket::ConnectedState;
    }
    void send(qint32 opcode, const QJsonObject& payload) {
        const QByteArray json = QJsonDocument(payload).toJson(QJsonDocument::Compact);
        char header[8];
        qToLittleEndian<qint32>(opcode, header);
        qToLittleEndian<qint32>(static_cast<qint32>(json.size()), header + 4);
        this->client_->write(header, 8);
        this->client_->write(json);
    }
    /// @brief Bytes as they are, for what is no frame, or part of one
    void write_raw(const QByteArray& bytes) {
        this->client_->write(bytes);
        this->client_->flush();
    }
    static QByteArray frame(qint32 opcode, const QByteArray& payload) {
        QByteArray bytes(8, '\0');
        qToLittleEndian<qint32>(opcode, bytes.data());
        qToLittleEndian<qint32>(static_cast<qint32>(payload.size()), bytes.data() + 4);
        return bytes + payload;
    }
    /// @brief The activity of each SET_ACTIVITY so far; null for one that cleared it
    QList<QJsonValue> activities() const {
        QList<QJsonValue> activities;
        for (const Frame& frame : this->frames) {
            if (frame.payload.value("cmd") == QLatin1String("SET_ACTIVITY")) {
                activities << frame.payload.value("args").toObject().value("activity");
            }
        }
        return activities;
    }

    /// Every frame received, over every connection
    QList<Frame> frames;
    int connections{0};
    /// What a handshake is answered with: Discord's READY, or the CLOSE of an unknown application
    bool accepts{true};
    /// A handshake gets no answer at all
    bool mute{false};

private:
    void read_() {
        while (this->client_->bytesAvailable() >= 8) {
            char header[8];
            this->client_->peek(header, 8);
            const auto length = qFromLittleEndian<qint32>(header + 4);
            if (this->client_->bytesAvailable() < 8 + length) {
                return;
            }
            this->client_->skip(8);
            const Frame frame{qFromLittleEndian<qint32>(header),
                              QJsonDocument::fromJson(this->client_->read(length)).object()};
            this->frames << frame;
            if (frame.opcode != 0 || this->mute) {
                continue;
            }
            if (this->accepts) {
                this->send(1, {{"cmd", "DISPATCH"}, {"evt", "READY"}, {"data", QJsonObject()}});
            } else {
                this->send(2, {{"code", 4000}, {"message", "Invalid Client ID"}});
                this->client_->disconnectFromServer();
            }
        }
    }

    QLocalServer server_;
    QPointer<QLocalSocket> client_;
};
