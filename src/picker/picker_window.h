#pragma once

#include "cast/cast_engine.h"
#include "picker/picker.h"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QRect>

/// @brief The picker's window: qml/Picker.qml over a Picker and the cast engine. Loaded when it
/// is first shown. It hides itself when it loses the focus, like a popup.
class PickerWindow : public QObject {
    Q_OBJECT

public:
    PickerWindow(Picker& picker, CastEngine& cast, QObject* parent = nullptr);

    /// @brief Shows the picker and gives it the focus
    /// @param beside The tray icon's place on the screen, to open next to. Invalid where the
    /// platform doesn't tell (Wayland): the picker is centred, or goes where the compositor puts
    /// it.
    void show(const QRect& beside = {});
    /// @brief show(), or hides it when it is showing
    void toggle(const QRect& beside = {});

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
