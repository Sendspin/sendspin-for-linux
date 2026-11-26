#include "pairing_clipboard.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>

namespace pairing_clipboard {

void copy(const QString& secret) {
    auto* data = new QMimeData;
    data->setText(secret);
    // Klipper and the managers that follow it keep no history of a copy marked so
    data->setData(QStringLiteral("x-kde-passwordManagerHint"), "secret");
    QGuiApplication::clipboard()->setMimeData(data);
}

bool holds(const QString& code) {
    // On Wayland a refused or replaced copy reads as another's text, or as none without the focus
    return !code.isEmpty() && QGuiApplication::clipboard()->text() == code;
}

void clear(const QString& code) {
    if (holds(code)) {
        QGuiApplication::clipboard()->clear();
    }
}

}  // namespace pairing_clipboard
