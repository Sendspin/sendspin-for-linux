#include "single_instance.h"

#include <QLocalSocket>
#include <QLoggingCategory>
#include <QThread>

Q_LOGGING_CATEGORY(lc_instance, "sendspin.instance")

SingleInstance::SingleInstance(const QString& dir, QObject* parent)
    : QObject(parent),
      socket_path_(dir + QStringLiteral("/sendspin.sock")),
      lock_(dir + QStringLiteral("/sendspin.lock")) {
    // Never stale by age: the instance runs for the whole session. QLockFile still drops the lock
    // of a process that died.
    this->lock_.setStaleLockTime(0);
    this->server_.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&this->server_, &QLocalServer::newConnection, this, [this] {
        // The connection is the whole message
        while (QLocalSocket* socket = this->server_.nextPendingConnection()) {
            socket->deleteLater();
            emit this->secondLaunch();
        }
    });
}

bool SingleInstance::claim() {
    if (!this->lock_.tryLock()) {
        if (!this->ping()) {
            qCWarning(lc_instance, "another instance holds the lock but does not answer");
        }
        return false;
    }
    // Holding the lock, so any socket file is a crashed instance's
    QLocalServer::removeServer(this->socket_path_);
    if (!this->server_.listen(this->socket_path_)) {
        qCWarning(lc_instance, "cannot listen for later launches: %s",
                  qPrintable(this->server_.errorString()));
    }
    return true;
}

bool SingleInstance::ping() const {
    // The instance takes the lock a moment before it listens
    for (int attempt = 0; attempt < 20; ++attempt) {
        QLocalSocket socket;
        socket.connectToServer(this->socket_path_);
        if (socket.waitForConnected(100)) {
            return true;
        }
        QThread::msleep(100);
    }
    return false;
}
