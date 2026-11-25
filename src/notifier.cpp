#include "notifier.h"

#include "autostart.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QGuiApplication>
#include <QVariantMap>

namespace {

/// The portal knows a notification by a name of the app's own; there is one at a time
constexpr QLatin1StringView PORTAL_ID{"pairing"};
constexpr QLatin1StringView KEY_DEFAULT{"default"};
constexpr QLatin1StringView KEY_BUTTON{"act"};
/// About what notification servers give one that asks for their default
constexpr int EXPIRY_MS = 10'000;

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

QDBusMessage portal_call(const QString& portal, const QString& method) {
    return QDBusMessage::createMethodCall(portal, QStringLiteral("/org/freedesktop/portal/desktop"),
                                          QStringLiteral("org.freedesktop.portal.Notification"),
                                          method);
}

/// The click's XDG activation token, which version 2 of the portal sends in a dict of
/// platform data among the action's parameters
QString activation_token(const QVariantList& parameter) {
    for (const QVariant& value : parameter) {
        if (!value.canConvert<QDBusArgument>()) {
            continue;
        }
        const auto argument = value.value<QDBusArgument>();
        if (argument.currentSignature() == QLatin1StringView("a{sv}")) {
            const QVariantMap data = qdbus_cast<QVariantMap>(argument);
            if (data.contains(QStringLiteral("activation-token"))) {
                return data.value(QStringLiteral("activation-token")).toString();
            }
        }
    }
    return {};
}

}  // namespace

Notifier::Notifier(QObject* parent)
    : Notifier(
          autostart::sandboxed() ? QStringLiteral("org.freedesktop.portal.Desktop") : QString(),
          parent) {}

Notifier::Notifier(const QString& portal, QObject* parent) : QObject(parent), portal_(portal) {
    if (!portal.isEmpty()) {
        this->expiry_.setSingleShot(true);
        this->expiry_.setInterval(EXPIRY_MS);
        connect(&this->expiry_, &QTimer::timeout, this, &Notifier::remove_from_portal);
        // The buttons, as aa{sv}
        qDBusRegisterMetaType<QList<QVariantMap>>();
        QDBusConnection::sessionBus().connect(
            portal, QStringLiteral("/org/freedesktop/portal/desktop"),
            QStringLiteral("org.freedesktop.portal.Notification"), QStringLiteral("ActionInvoked"),
            this, SLOT(onPortalActionInvoked(QString, QString, QVariantList)));
        return;
    }
    QDBusConnection::sessionBus().connect(QStringLiteral("org.freedesktop.Notifications"),
                                          QStringLiteral("/org/freedesktop/Notifications"),
                                          QStringLiteral("org.freedesktop.Notifications"),
                                          QStringLiteral("ActionInvoked"), this,
                                          SLOT(onActionInvoked(quint32, QString)));
}

void Notifier::onActionInvoked(quint32 id, const QString& key) {
    // The signal is broadcast for every app's notifications
    if (id != 0 && id == this->id_) {
        this->invoked(key);
    }
}

void Notifier::onPortalActionInvoked(const QString& id, const QString& action,
                                     const QVariantList& parameter) {
    if (id == PORTAL_ID) {
        this->invoked(action, activation_token(parameter));
    }
}

void Notifier::invoked(const QString& key, const QString& activation_token) {
    if (key == KEY_DEFAULT) {
        emit this->clicked(activation_token);
    } else {
        emit this->actionInvoked();
    }
}

void Notifier::show_on_portal(const QString& summary, const QString& body, bool persistent,
                              const QString& action) {
    // No timeout to ask for: the desktop keeps a notification until it is removed, so a
    // passing one is removed from here. Priority is only a hint of which is which
    QVariantMap notification{
        {QStringLiteral("title"), summary},
        {QStringLiteral("priority"),
         persistent ? QStringLiteral("high") : QStringLiteral("normal")},
        {QStringLiteral("default-action"), QString(KEY_DEFAULT)},
    };
    if (!body.isEmpty()) {
        notification.insert(QStringLiteral("body"), body);
    }
    if (!action.isEmpty()) {
        const QList<QVariantMap> buttons{{
            {QStringLiteral("label"), action},
            {QStringLiteral("action"), QString(KEY_BUTTON)},
        }};
        notification.insert(QStringLiteral("buttons"), QVariant::fromValue(buttons));
    }
    QDBusMessage message = portal_call(this->portal_, QStringLiteral("AddNotification"));
    message << QString(PORTAL_ID) << notification;
    QDBusConnection::sessionBus().send(message);
    if (persistent) {
        this->expiry_.stop();
    } else {
        this->expiry_.start();
    }
}

void Notifier::remove_from_portal() {
    this->expiry_.stop();
    QDBusMessage message = portal_call(this->portal_, QStringLiteral("RemoveNotification"));
    message << QString(PORTAL_ID);
    QDBusConnection::sessionBus().send(message);
}

void Notifier::show(const QString& summary, const QString& body, bool persistent,
                    const QString& action) {
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }
    if (!this->portal_.isEmpty()) {
        this->show_on_portal(summary, body, persistent, action);
        return;
    }
    // "default" is the click on the notification itself; servers that show it use the label
    QStringList actions{QString(KEY_DEFAULT), tr("Settings")};
    if (!action.isEmpty()) {
        actions << QString(KEY_BUTTON) << action;
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
    if (!this->portal_.isEmpty()) {
        this->remove_from_portal();
        return;
    }
    ++this->generation_;
    if (this->id_ != 0) {
        close_notification(this->id_);
        this->id_ = 0;
    }
}
