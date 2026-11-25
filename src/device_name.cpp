#include "device_name.h"

#include <QFile>
#include <QSysInfo>

QString device_name(const QString& machine_info) {
    QFile file(machine_info);
    if (file.open(QIODevice::ReadOnly)) {
        // os-release syntax (machine-info(5)): KEY=value, the value optionally quoted
        const QByteArray key = "PRETTY_HOSTNAME=";
        for (const QByteArray& raw : file.readAll().split('\n')) {
            const QByteArray line = raw.trimmed();
            if (!line.startsWith(key)) {
                continue;
            }
            QByteArray value = line.sliced(key.size()).trimmed();
            const bool quoted = value.size() >= 2 &&
                                (value.front() == '"' || value.front() == '\'') &&
                                value.back() == value.front();
            if (quoted) {
                value = value.sliced(1, value.size() - 2);
                // Inside double quotes a backslash escapes the next character
                value.replace("\\\"", "\"")
                    .replace("\\$", "$")
                    .replace("\\`", "`")
                    .replace("\\\\", "\\");
            }
            if (!value.isEmpty()) {
                return QString::fromUtf8(value);
            }
        }
    }
    return QSysInfo::machineHostName();
}
