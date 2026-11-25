#include "autostart.h"

#include "app_id.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDir>
#include <QFileInfo>
#include <QRandomGenerator>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace autostart {

namespace {

constexpr const char* KEY_PORTAL = "autostart/portal";  ///< what the portal last granted

QString entry_name() {
    return QStringLiteral("autostart/") + QLatin1String(APP_ID) + QStringLiteral(".desktop");
}

/// Not hidden, by the spec's key or GNOME's
bool starts(const QString& entry_path) {
    const QSettings entry(entry_path, QSettings::IniFormat);
    return !entry.value("Desktop Entry/Hidden", false).toBool() &&
           entry.value("Desktop Entry/X-GNOME-Autostart-enabled", true).toBool();
}

/// The Background portal's entry for this app's Flatpak, which has this build's name for it
bool from_flatpak(const QString& entry_path) {
    QFile file(entry_path);
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }
    for (const QByteArray& line : file.readAll().split('\n')) {
        if (line.startsWith("X-Flatpak=") ||
            (line.startsWith("Exec=") && line.contains("flatpak run "))) {
            return true;
        }
    }
    return false;
}

void store_granted(bool granted) {
    QSettings(QStringLiteral("sendspin"), QStringLiteral("sendspin")).setValue(KEY_PORTAL, granted);
}

/// A package's entry is installed, and starts the app
bool packaged() {
    const QStringList found =
        QStandardPaths::locateAll(QStandardPaths::GenericConfigLocation, entry_name());
    for (const QString& entry_path : found) {
        if (entry_path != path()) {
            return starts(entry_path);
        }
    }
    return false;
}

QByteArray entry_for(const QString& program) {
    const QString exec =
        exec_value(program.isEmpty() ? QCoreApplication::applicationFilePath() : program);
    return "[Desktop Entry]\nType=Application\nName=Sendspin\nExec=" + exec.toUtf8() +
           " --background\nIcon=" + APP_ID + "\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
}

bool write(const QByteArray& entry) {
    if (!QDir().mkpath(QFileInfo(path()).path())) {
        return false;
    }
    QSaveFile file(path());
    return file.open(QIODevice::WriteOnly) && file.write(entry) == entry.size() && file.commit();
}

/// One RequestBackground call, until its Response
class PortalRequest : public QObject {
    Q_OBJECT

public:
    PortalRequest(std::function<void(bool)> done, QObject* parent)
        : QObject(parent), done_(std::move(done)) {}

public slots:
    void onResponse(uint response, const QVariantMap& results) {
        // 0 is success; cancelled or refused, nothing starts it
        this->done_(response == 0 && results.value(QStringLiteral("autostart")).toBool());
        this->deleteLater();
    }

private:
    std::function<void(bool)> done_;
};

}  // namespace

QString path() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + u'/' +
           entry_name();
}

bool sandboxed() {
    return QFileInfo::exists(QStringLiteral("/.flatpak-info"));
}

bool enabled() {
    if (sandboxed()) {
        return QSettings(QStringLiteral("sendspin"), QStringLiteral("sendspin"))
            .value(KEY_PORTAL, false)
            .toBool();
    }
    // The user's own entry decides, off included: desktops switch one off by marking it. The
    // Flatpak's there starts the Flatpak, and not this build
    return QFileInfo::exists(path()) ? !from_flatpak(path()) && starts(path()) : packaged();
}

QString exec_value(const QString& program) {
    // Desktop Entry Specification, "The Exec key": quoted, with these four escaped, and a
    // literal percent doubled
    QString quoted;
    for (const QChar c : program) {
        if (c == u'"' || c == u'`' || c == u'$' || c == u'\\') {
            quoted += u'\\';
        }
        quoted += c;
        if (c == u'%') {
            quoted += c;
        }
    }
    // The file's own string escaping, on top
    quoted.replace(u'\\', QStringLiteral("\\\\"));
    return u'"' + quoted + u'"';
}

void set_enabled(bool enabled, const QString& program, QObject* context,
                 const std::function<void()>& done) {
    if (sandboxed()) {
        ask_portal(enabled, QStringLiteral("org.freedesktop.portal.Desktop"), context,
                   [done](bool granted) {
                       store_granted(granted);
                       if (done) {
                           done();
                       }
                   });
        return;
    }
    if (!enabled && from_flatpak(path())) {
        // The Flatpak's entry is its own switch's to end; under it this build is off already
    } else if (enabled == packaged()) {
        // The package's entry, or none, already says it
        QFile::remove(path());
    } else if (enabled) {
        write(entry_for(program));
    } else {
        write("[Desktop Entry]\nType=Application\nName=Sendspin\nHidden=true\n");
    }
    if (done) {
        done();
    }
}

void refresh(const QString& program, QObject* context, const std::function<void()>& done) {
    if (sandboxed()) {
        // Asking again is the one way to learn that the desktop's settings took it back
        if (enabled()) {
            ask_portal(true, QStringLiteral("org.freedesktop.portal.Desktop"), context,
                       [done](bool granted) {
                           store_granted(granted);
                           if (done && !granted) {
                               done();
                           }
                       });
        }
        return;
    }
    if (!QFileInfo::exists(path()) || !starts(path()) || from_flatpak(path())) {
        return;
    }
    QFile file(path());
    if (file.open(QIODevice::ReadOnly) && file.readAll() != entry_for(program)) {
        file.close();
        write(entry_for(program));
    }
}

void ask_portal(bool enabled, const QString& service, QObject* context,
                const std::function<void(bool granted)>& done) {
    QDBusConnection bus = QDBusConnection::sessionBus();
    // The request's path is made of our bus name and a token of ours, so the answer can be
    // listened for before the call that it may overtake
    const QString token = QStringLiteral("sendspin%1").arg(QRandomGenerator::global()->generate());
    const QString sender = bus.baseService().mid(1).replace(u'.', u'_');
    const QString request_path =
        QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2").arg(sender, token);
    auto* request = new PortalRequest(done, context ? context : qApp);
    bus.connect(service, request_path, QStringLiteral("org.freedesktop.portal.Request"),
                QStringLiteral("Response"), request, SLOT(onResponse(uint, QVariantMap)));

    QDBusMessage call = QDBusMessage::createMethodCall(
        service, QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.Background"), QStringLiteral("RequestBackground"));
    const QVariantMap options{
        {QStringLiteral("handle_token"), token},
        {QStringLiteral("reason"),
         QCoreApplication::translate("autostart", "Sendspin starts with your session.")},
        {QStringLiteral("autostart"), enabled},
        {QStringLiteral("commandline"),
         QStringList{QStringLiteral("sendspin-for-linux"), QStringLiteral("--background")}},
    };
    // No parent window: the dialog, where a desktop shows one, stands alone
    call << QString() << options;
    auto* watcher = new QDBusPendingCallWatcher(bus.asyncCall(call), request);
    QObject::connect(watcher, &QDBusPendingCallWatcher::finished, request,
                     [request](QDBusPendingCallWatcher* finished) {
                         // No portal, or none with this interface: no Response will come
                         if (QDBusPendingReply<QDBusObjectPath>(*finished).isError()) {
                             request->onResponse(2, {});
                         }
                     });
}

}  // namespace autostart

#include "autostart.moc"
