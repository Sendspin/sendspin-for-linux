#include "autostart.h"

#include "app_id.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>

namespace autostart {

QString path() {
    return QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) +
           QStringLiteral("/autostart/") + QLatin1String(APP_ID) + QStringLiteral(".desktop");
}

bool available() {
    return !QFileInfo::exists(QStringLiteral("/.flatpak-info"));
}

bool enabled() {
    if (!QFileInfo::exists(path())) {
        return false;
    }
    // Desktops switch an entry off by marking it, rather than removing it
    const QSettings entry(path(), QSettings::IniFormat);
    return !entry.value("Desktop Entry/Hidden", false).toBool() &&
           entry.value("Desktop Entry/X-GNOME-Autostart-enabled", true).toBool();
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

namespace {

QByteArray entry_for(const QString& program) {
    const QString exec =
        exec_value(program.isEmpty() ? QCoreApplication::applicationFilePath() : program);
    return "[Desktop Entry]\nType=Application\nName=Sendspin\nExec=" + exec.toUtf8() +
           "\nTerminal=false\nX-GNOME-Autostart-enabled=true\n";
}

}  // namespace

bool set_enabled(bool enabled, const QString& program) {
    if (!enabled) {
        return !QFileInfo::exists(path()) || QFile::remove(path());
    }
    if (!available() || !QDir().mkpath(QFileInfo(path()).path())) {
        return false;
    }
    QSaveFile file(path());
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    file.write(entry_for(program));
    return file.commit();
}

void refresh(const QString& program) {
    if (!available() || !enabled()) {
        return;
    }
    QFile file(path());
    if (file.open(QIODevice::ReadOnly) && file.readAll() != entry_for(program)) {
        file.close();
        set_enabled(true, program);
    }
}

}  // namespace autostart
