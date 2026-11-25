#pragma once

#include "cast/cast_engine.h"
#include "picker/picker.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPoint>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QRect>
#include <QSize>

/// @brief The picker's window: qml/Picker.qml over a Picker and the cast engine. Loaded when it
/// is first shown. It hides itself when it loses the focus, like a popup.
class PickerWindow : public QObject {
    Q_OBJECT

public:
    PickerWindow(Picker& picker, CastEngine& cast, QObject* parent = nullptr);

    /// Where the picker opens. The tray's own place is not to be had: Qt reports none for a
    /// StatusNotifierItem.
    enum class Place {
        /// In the middle of the screen; on Wayland, wherever the compositor puts it
        CENTRE,
        /// At the pointer, which is on the tray icon when a click there opens it. X11 only: a
        /// Wayland window can't place itself, and is centred.
        POINTER,
    };

    /// @brief Shows the picker and gives it the focus
    /// @param activation_token The XDG activation token of the launch that asks for it, which
    /// lets a Wayland compositor give it the focus; empty for none
    void show(Place place = Place::CENTRE, const QString& activation_token = {});
    /// @brief show(), or hides it when it is showing
    void toggle(Place place = Place::CENTRE);

    /// @brief Where a window of this size opens for a pointer at this spot: beside it, and never
    /// outside what the panels leave of the screen
    static QPoint beside(const QPoint& pointer, const QSize& size, const QRect& available);

    /// @brief nullptr until first shown, or when the QML did not load
    QQuickWindow* window() const {
        return this->window_;
    }

signals:
    /// The gear button
    void settingsRequested();

private:
    bool load_();

    Picker& picker_;
    CastEngine& cast_;
    QQmlApplicationEngine engine_;
    QPointer<QQuickWindow> window_;
    bool tried_{false};
    /// Since it last hid: the click on the tray icon that took its focus must not reopen it
    QElapsedTimer hidden_;
};
