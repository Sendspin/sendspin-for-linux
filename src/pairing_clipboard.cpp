#include "pairing_clipboard.h"

#include <QClipboard>
#include <QGuiApplication>

namespace pairing_clipboard {

void copy(const QString& code) {
    QGuiApplication::clipboard()->setText(code);
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
