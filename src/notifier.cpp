#include "notifier.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QGuiApplication>
#include <QVariantMap>

namespace {

QDBusMessage call(const QString& method) {
    return QDBusMessage::createMethodCall(QStringLiteral("org.freedesktop.Notifications"),
                                          QStringLiteral("/org/freedesktop/Notifications"),
                                          QStringLiteral("org.freedesktop.Notifications"), method);
}

void close_notification(quint32 id) {
    QDBusMessage message = call(QStringLiteral("CloseNotification"));
    message << id;
    QDBusConnection::sessionBus().send(message);
}

}  // namespace

Notifier::Notifier(QObject* parent) : QObject(parent) {
    QDBusConnection::sessionBus().connect(QStringLiteral("org.freedesktop.Notifications"),
                                          QStringLiteral("/org/freedesktop/Notifications"),
                                          QStringLiteral("org.freedesktop.Notifications"),
                                          QStringLiteral("ActionInvoked"), this,
                                          SLOT(onActionInvoked(quint32, QString)));
}

void Notifier::onActionInvoked(quint32 id, const QString& key) {
    // The signal is broadcast for every app's notifications
    if (id != 0 && id == this->id_) {
        if (key == QLatin1String("default")) {
            emit this->clicked();
        } else {
            emit this->actionInvoked();
        }
    }
}

void Notifier::show(const QString& summary, const QString& body, bool persistent,
                    const QString& action) {
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }
    // "default" is the click on the notification itself; servers that show it use the label
    QStringList actions{QStringLiteral("default"), tr("Settings")};
    if (!action.isEmpty()) {
        actions << QStringLiteral("act") << action;
    }
    QDBusMessage message = call(QStringLiteral("Notify"));
    // app_name, replaces_id, app_icon, summary, body, actions, hints, expire_timeout (0: never,
    // -1: the server's default)
    message << QGuiApplication::applicationDisplayName() << this->id_ << QString() << summary
            << body << actions << QVariantMap() << (persistent ? 0 : -1);

    const quint32 generation = ++this->generation_;
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation](QDBusPendingCallWatcher* finished) {
                finished->deleteLater();
                const QDBusPendingReply<quint32> reply = *finished;
                if (reply.isError()) {
                    return;
                }
                if (generation == this->generation_) {
                    this->id_ = reply.value();
                } else if (this->id_ == 0) {
                    // Closed, or replaced, before its id was known
                    close_notification(reply.value());
                }
            });
}

void Notifier::close() {
    ++this->generation_;
    if (this->id_ != 0) {
        close_notification(this->id_);
        this->id_ = 0;
    }
}
