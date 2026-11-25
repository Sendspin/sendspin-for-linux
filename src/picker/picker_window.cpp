#include "picker/picker_window.h"

#include <QCursor>
#include <QGuiApplication>
#include <QQuickStyle>
#include <QScreen>

#include <algorithm>
#include <utility>

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
    if (QQuickStyle::name().isEmpty() && qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
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

QPoint PickerWindow::beside(const QPoint& pointer, const QSize& size, const QRect& available) {
    // Under a panel at the top, over one at the bottom
    const bool above = pointer.y() > available.center().y();
    const QPoint wanted(pointer.x() - (size.width() / 2),
                        above ? pointer.y() - size.height() : pointer.y());
    return {std::clamp(wanted.x(), available.left(),
                       std::max(available.left(), available.right() + 1 - size.width())),
            std::clamp(wanted.y(), available.top(),
                       std::max(available.top(), available.bottom() + 1 - size.height()))};
}

void PickerWindow::show(Place place, const QString& activation_token) {
    if (!this->load_()) {
        return;
    }
    const bool at_pointer =
        place == Place::POINTER && QGuiApplication::platformName() == QLatin1String("xcb");
    const QScreen* screen = at_pointer ? QGuiApplication::screenAt(QCursor::pos()) : nullptr;
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    if (screen != nullptr) {
        const QRect area = screen->availableGeometry();
        const QSize size = this->window_->size();
        // Wayland ignores it
        this->window_->setPosition(at_pointer ? beside(QCursor::pos(), size, area)
                                              : area.center() -
                                                    QPoint(size.width() / 2, size.height() / 2));
    }
    if (!activation_token.isEmpty()) {
        // Where Qt's Wayland plugin looks for the token when a window asks for the focus
        qputenv("XDG_ACTIVATION_TOKEN", activation_token.toLatin1());
    }
    this->window_->show();
    this->window_->raise();
    this->window_->requestActivate();
}

void PickerWindow::toggle(Place place) {
    if (this->window_ && this->window_->isVisible()) {
        this->window_->hide();
    } else if (!this->hidden_.isValid() || this->hidden_.elapsed() > REOPEN_GUARD_MS) {
        this->show(place);
    }
}
