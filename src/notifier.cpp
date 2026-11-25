#include "notifier.h"

#include "autostart.h"

#include <QBuffer>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QVariantMap>

Q_LOGGING_CATEGORY(lc_notifier, "sendspin.notify")

namespace {

const QString PATH = QStringLiteral("/org/freedesktop/Notifications");
const QString INTERFACE = QStringLiteral("org.freedesktop.Notifications");
const QString PORTAL_PATH = QStringLiteral("/org/freedesktop/portal/desktop");
const QString PORTAL_INTERFACE = QStringLiteral("org.freedesktop.portal.Notification");
constexpr QLatin1StringView KEY_DEFAULT{"default"};
constexpr QLatin1StringView KEY_BUTTON{"act"};
/// About what notification servers give one that asks for their default
constexpr int EXPIRY_MS = 10'000;
/// A cover is drawn far smaller than it arrives, and goes over the bus as pixels
constexpr int IMAGE_PX = 256;

/// The `image-data` hint: width, height, rowstride, has alpha, bits per sample, channels, pixels
QVariant image_data(const QImage& image) {
    const QImage pixels = image.convertToFormat(QImage::Format_RGBA8888);
    QDBusArgument data;
    data.beginStructure();
    data << pixels.width() << pixels.height() << static_cast<int>(pixels.bytesPerLine()) << true
         << 8 << 4
         << QByteArray(reinterpret_cast<const char*>(pixels.constBits()), pixels.sizeInBytes());
    data.endStructure();
    return QVariant::fromValue(data);
}

/// The portal's `icon`, a serialized GBytesIcon. Cropped square, which every portal's check of
/// an icon takes
QVariant portal_icon(const QImage& image) {
    const int side = qMin(image.width(), image.height());
    const QImage square =
        image.copy((image.width() - side) / 2, (image.height() - side) / 2, side, side);
    QByteArray png;
    QBuffer buffer(&png);
    square.save(&buffer, "PNG");
    QDBusArgument icon;
    icon.beginStructure();
    icon << QStringLiteral("bytes") << QDBusVariant(png);
    icon.endStructure();
    return QVariant::fromValue(icon);
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

Notifier::Notifier(const QString& id, QObject* parent)
    : Notifier(id,
               autostart::sandboxed() ? QStringLiteral("org.freedesktop.portal.Desktop")
                                      : QStringLiteral("org.freedesktop.Notifications"),
               autostart::sandboxed(), parent) {}

Notifier::Notifier(const QString& id, const QString& service, bool portal, QObject* parent)
    : QObject(parent), name_(id), service_(service), portal_(portal) {
    if (portal) {
        this->expiry_.setSingleShot(true);
        this->expiry_.setInterval(EXPIRY_MS);
        connect(&this->expiry_, &QTimer::timeout, this, &Notifier::remove_from_portal);
        // The buttons, as aa{sv}
        qDBusRegisterMetaType<QList<QVariantMap>>();
        QDBusConnection::sessionBus().connect(
            service, PORTAL_PATH, PORTAL_INTERFACE, QStringLiteral("ActionInvoked"), this,
            SLOT(onPortalActionInvoked(QString, QString, QVariantList)));
        return;
    }
    QDBusConnection::sessionBus().connect(service, PATH, INTERFACE, QStringLiteral("ActionInvoked"),
                                          this, SLOT(onActionInvoked(quint32, QString)));
}

void Notifier::onActionInvoked(quint32 id, const QString& key) {
    // The signal is broadcast for every app's notifications
    if (id != 0 && id == this->id_) {
        this->invoked(key);
    }
}

void Notifier::onPortalActionInvoked(const QString& id, const QString& action,
                                     const QVariantList& parameter) {
    if (id == this->name_) {
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

void Notifier::send_to_portal_(const Notice& notice) {
    // No timeout to ask for: the desktop keeps a notification until it is removed, so a
    // passing one is removed from here, which also keeps it out of the list. Priority is only
    // a hint of which is which
    QVariantMap notification{
        {QStringLiteral("title"), notice.summary},
        {QStringLiteral("priority"),
         notice.persistent ? QStringLiteral("high") : QStringLiteral("normal")},
        {QStringLiteral("default-action"), QString(KEY_DEFAULT)},
    };
    if (!notice.body.isEmpty()) {
        notification.insert(QStringLiteral("body"), notice.body);
    }
    if (!notice.image.isNull()) {
        notification.insert(QStringLiteral("icon"), portal_icon(notice.image));
    }
    if (!notice.action.isEmpty()) {
        const QList<QVariantMap> buttons{{
            {QStringLiteral("label"), notice.action},
            {QStringLiteral("action"), QString(KEY_BUTTON)},
        }};
        notification.insert(QStringLiteral("buttons"), QVariant::fromValue(buttons));
    }
    QDBusMessage message = QDBusMessage::createMethodCall(
        this->service_, PORTAL_PATH, PORTAL_INTERFACE, QStringLiteral("AddNotification"));
    message << this->name_ << notification;
    QDBusConnection::sessionBus().send(message);
    if (notice.persistent) {
        this->expiry_.stop();
    } else {
        this->expiry_.start();
    }
}

void Notifier::remove_from_portal() {
    this->expiry_.stop();
    QDBusMessage message = QDBusMessage::createMethodCall(
        this->service_, PORTAL_PATH, PORTAL_INTERFACE, QStringLiteral("RemoveNotification"));
    message << this->name_;
    QDBusConnection::sessionBus().send(message);
}

void Notifier::show(const QString& summary, const QString& body, bool persistent,
                    const QString& action) {
    this->send_({.summary = summary,
                 .body = body,
                 .persistent = persistent,
                 .action = action,
                 .click = tr("Settings")});
}

void Notifier::show_track(const QString& summary, const QString& body, const QImage& cover) {
    // Not low urgency: GNOME shows no banner for that, only a line in its list
    this->send_({.summary = summary,
                 .body = body,
                 .click = tr("Open"),
                 .transient = true,
                 .image = cover.isNull() ? cover
                                         : cover.scaled(IMAGE_PX, IMAGE_PX, Qt::KeepAspectRatio,
                                                        Qt::SmoothTransformation)});
}

void Notifier::send_(const Notice& notice) {
    const QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        return;
    }
    if (this->portal_) {
        this->send_to_portal_(notice);
        return;
    }
    // "default" is the click on the notification itself; servers that show it use the label
    QStringList actions{QString(KEY_DEFAULT), notice.click};
    if (!notice.action.isEmpty()) {
        actions << QString(KEY_BUTTON) << notice.action;
    }
    QVariantMap hints;
    if (notice.transient) {
        hints.insert(QStringLiteral("transient"), true);
    }
    if (!notice.image.isNull()) {
        hints.insert(QStringLiteral("image-data"), image_data(notice.image));
    }
    QDBusMessage message =
        QDBusMessage::createMethodCall(this->service_, PATH, INTERFACE, QStringLiteral("Notify"));
    // app_name, replaces_id, app_icon, summary, body, actions, hints, expire_timeout (0: never,
    // -1: the server's default)
    message << QGuiApplication::applicationDisplayName() << this->id_ << QString() << notice.summary
            << notice.body << actions << hints << (notice.persistent ? 0 : -1);

    const quint32 generation = ++this->generation_;
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation](QDBusPendingCallWatcher* finished) {
                finished->deleteLater();
                const QDBusPendingReply<quint32> reply = *finished;
                if (reply.isError()) {
                    // No service to show it is a desktop without notifications, not a fault
                    qCDebug(lc_notifier, "not shown: %s", qUtf8Printable(reply.error().message()));
                    return;
                }
                if (generation == this->generation_) {
                    this->id_ = reply.value();
                } else if (this->id_ == 0) {
                    // Closed, or replaced, before its id was known
                    this->close_notification_(reply.value());
                }
            });
}

void Notifier::close_notification_(quint32 id) {
    QDBusMessage message = QDBusMessage::createMethodCall(this->service_, PATH, INTERFACE,
                                                          QStringLiteral("CloseNotification"));
    message << id;
    QDBusConnection::sessionBus().send(message);
}

void Notifier::close() {
    if (this->portal_) {
        this->remove_from_portal();
        return;
    }
    ++this->generation_;
    if (this->id_ != 0) {
        this->close_notification_(this->id_);
        this->id_ = 0;
    }
}
