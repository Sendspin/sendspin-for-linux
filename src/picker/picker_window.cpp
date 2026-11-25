#include "picker/picker_window.h"

#include <QGuiApplication>
#include <QLoggingCategory>
#include <QQuickStyle>
#include <QScreen>

#include <algorithm>
#include <utility>

Q_LOGGING_CATEGORY(lc_picker, "sendspin.picker")

namespace {

/// A click on the tray icon lands this soon after the focus it took away
constexpr qint64 REOPEN_GUARD_MS = 300;

}  // namespace

PickerWindow::PickerWindow(Picker& picker, CastEngine& cast, QObject* parent)
    : QObject(parent), picker_(picker), cast_(cast) {}

bool PickerWindow::load_() {
    if (this->window_) {
        return true;
    }
    if (std::exchange(this->tried_, true)) {
        // It failed before, and will again
        return false;
    }
    // Fusion takes its colours from the platform's palette; the default style has its own
    if (QQuickStyle::name().isEmpty()) {
        QQuickStyle::setStyle(QStringLiteral("Fusion"));
    }
    qmlRegisterUncreatableType<Picker>("Sendspin", 1, 0, "Picker", QStringLiteral("The app's own"));
    this->engine_.setInitialProperties(
        {{QStringLiteral("picker"), QVariant::fromValue(&this->picker_)},
         {QStringLiteral("cast"), QVariant::fromValue(&this->cast_)}});
    this->engine_.load(QUrl(QStringLiteral("qrc:/qml/Picker.qml")));
    this->window_ = this->engine_.rootObjects().isEmpty()
                        ? nullptr
                        : qobject_cast<QQuickWindow*>(this->engine_.rootObjects().first());
    if (!this->window_) {
        qCWarning(lc_picker, "the picker did not load");
        return false;
    }
    // By name: the signal is the QML's
    connect(this->window_, SIGNAL(settingsRequested()), this, SIGNAL(settingsRequested()));
    connect(this->window_, &QWindow::visibleChanged, this, [this](bool visible) {
        if (!visible) {
            this->hidden_.start();
        }
    });
    return true;
}

void PickerWindow::show(const QRect& beside) {
    if (!this->load_()) {
        return;
    }
    const QScreen* screen = beside.isValid() ? QGuiApplication::screenAt(beside.center()) : nullptr;
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen != nullptr) {
        const QRect area = screen->availableGeometry();
        const QSize size = this->window_->size();
        QPoint at = area.center() - QPoint(size.width() / 2, size.height() / 2);
        if (beside.isValid()) {
            // Under a panel at the top, over one at the bottom
            at.setX(beside.center().x() - (size.width() / 2));
            at.setY(beside.center().y() < area.center().y() ? beside.bottom()
                                                            : beside.top() - size.height());
        }
        at.setX(
            std::clamp(at.x(), area.left(), std::max(area.left(), area.right() - size.width())));
        at.setY(
            std::clamp(at.y(), area.top(), std::max(area.top(), area.bottom() - size.height())));
        // Wayland ignores it
        this->window_->setPosition(at);
    }
    this->window_->show();
    this->window_->raise();
    this->window_->requestActivate();
}

void PickerWindow::toggle(const QRect& beside) {
    if (this->window_ && this->window_->isVisible()) {
        this->window_->hide();
    } else if (!this->hidden_.isValid() || this->hidden_.elapsed() > REOPEN_GUARD_MS) {
        this->show(beside);
    }
}
