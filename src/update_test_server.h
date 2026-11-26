#pragma once

#include "update_installer.h"

#include <QByteArray>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>

/// @brief Stands in for GitHub in the updater's tests: one release on the loopback interface,
/// its "latest release" answer at /latest and its files beside it.
///
/// The files are `update_test_data/`: a stand-in for release 9.0.0's Debian 13 package, its
/// SHA256SUMS, and their signature by a key made for this and thrown away, whose public half is
/// `key.asc`. New ones are a `gpg --detach-sign --armor SHA256SUMS` with any new key.
class ReleaseServer : public QObject {
public:
    static constexpr const char* VERSION = "9.0.0";
    static constexpr const char* PACKAGE = "sendspin-for-linux-9.0.0-debian13-amd64.deb";

    static QString data(const QString& name) {
        return QStringLiteral(SENDSPIN_SOURCE_DIR "/src/update_test_data/") + name;
    }

    ReleaseServer() {
        this->server_.listen(QHostAddress::LocalHost);
        for (const char* name : {PACKAGE, "SHA256SUMS", "SHA256SUMS.asc"}) {
            QFile file(data(QLatin1String(name)));
            if (file.open(QIODevice::ReadOnly)) {
                this->files[QLatin1String(name)] = file.readAll();
            }
        }
        connect(&this->server_, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket* socket = this->server_.nextPendingConnection()) {
                connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
                    if (socket->peek(1 << 16).contains("\r\n\r\n")) {
                        this->answer_(socket);
                    }
                });
            }
        });
    }

    QUrl url(const QString& path = QStringLiteral("latest")) const {
        return QUrl(
            QStringLiteral("http://127.0.0.1:%1/%2").arg(this->server_.serverPort()).arg(path));
    }

    /// What /latest answers
    QByteArray latest() const {
        QJsonArray assets;
        for (auto file = this->files.begin(); file != this->files.end(); ++file) {
            assets.append(QJsonObject{{"name", file.key()},
                                      {"browser_download_url", this->url(file.key()).toString()}});
        }
        for (auto file = this->elsewhere.begin(); file != this->elsewhere.end(); ++file) {
            assets.append(QJsonObject{{"name", file.key()},
                                      {"browser_download_url", file.value().toString()}});
        }
        return QJsonDocument(QJsonObject{{"tag_name", this->tag},
                                         {"html_url", this->url("page").toString()},
                                         {"assets", assets}})
            .toJson();
    }

    /// Each file's content by its name, as the release has them: a test may change or drop one
    QMap<QString, QByteArray> files;
    /// Files the release says are at some other address
    QMap<QString, QUrl> elsewhere;
    QString tag{QStringLiteral("v9.0.0")};
    /// How often /latest was asked for
    int checks{0};

private:
    void answer_(QTcpSocket* socket) {
        const QString path = QString::fromLatin1(socket->readAll().split(' ').value(1)).mid(1);
        QByteArray body;
        bool found = true;
        if (path == u"latest") {
            ++this->checks;
            body = this->latest();
        } else if (this->files.contains(path)) {
            body = this->files.value(path);
        } else {
            found = false;
        }
        socket->write(found ? "HTTP/1.1 200 OK\r\n" : "HTTP/1.1 404 Not Found\r\n");
        socket->write("Connection: close\r\nContent-Length: " + QByteArray::number(body.size()) +
                      "\r\n\r\n" + body);
        socket->disconnectFromHost();
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
    }

    QTcpServer server_;
};

inline const char* const LOCAL_POLICY = "sendspin-for-linux:\n"
                                        "  Installed: 1.0.0\n"
                                        "  Candidate: 1.0.0\n"
                                        "  Version table:\n"
                                        " *** 1.0.0 100\n"
                                        "        100 /var/lib/dpkg/status\n";
inline const char* const REPOSITORY_POLICY =
    "sendspin-for-linux:\n"
    "  Installed: 1.0.0\n"
    "  Candidate: 9.0.0\n"
    "  Version table:\n"
    "     9.0.0 500\n"
    "        500 https://sendspin.github.io/sendspin-for-linux/apt debian13/main amd64 Packages\n"
    " *** 1.0.0 500\n"
    "        500 https://sendspin.github.io/sendspin-for-linux/apt debian13/main amd64 Packages\n"
    "        100 /var/lib/dpkg/status\n";

/// A Debian 13 machine with the package installed from a file, the signing key in place, and a
/// pkexec that is a script of the test's: it has no password to ask for
struct UpdateMachine {
    explicit UpdateMachine(const QByteArray& pkexec = "exit 0", const char* policy = LOCAL_POLICY) {
        write("policy", policy);
        // What it was asked to run, for the test to read
        write("pkexec",
              "#!/bin/sh\necho \"$@\" >" + this->path("ran").toUtf8() + "\n" + pkexec + "\n");
        QFile(this->path("pkexec"))
            .setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
        this->system = {.helper = QStringLiteral(SENDSPIN_SOURCE_DIR "/data/install-update"),
                        .pkexec = this->path("pkexec"),
                        .key = ReleaseServer::data("key.asc"),
                        .os_id = QStringLiteral("debian"),
                        .os_version = QStringLiteral("13"),
                        .cpu = QStringLiteral("x86_64"),
                        .sandboxed = false,
                        .image_based = false,
                        .origin_command = {QStringLiteral("cat"), this->path("policy")}};
    }

    QString path(const QString& name) const {
        return this->dir.filePath(name);
    }
    void write(const QString& name, const QByteArray& content) const {
        QFile file(this->path(name));
        if (file.open(QIODevice::WriteOnly)) {
            file.write(content);
        }
    }
    /// What pkexec was asked to run; empty when it never was
    QStringList ran() const {
        QFile file(this->path("ran"));
        return file.open(QIODevice::ReadOnly)
                   ? QString::fromUtf8(file.readAll()).trimmed().split(u' ')
                   : QStringList();
    }

    QTemporaryDir dir;
    UpdateInstaller::System system;
};
