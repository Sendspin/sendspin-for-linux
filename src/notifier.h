#pragma once

#include <QObject>
#include <QString>

/// @brief One desktop notification at a time, through org.freedesktop.Notifications. Does
/// nothing without a session bus or a notification service.
class Notifier : public QObject {
    Q_OBJECT

public:
    using QObject::QObject;

    /// @brief Shows a notification in place of the one showing
    /// @param persistent stays until close() or the user dismisses it
    void show(const QString& summary, const QString& body = {}, bool persistent = false);
    /// @brief Takes down the one showing
    void close();

private:
    quint32 id_{0};
    /// Counts show() and close() calls, so a late reply to an old show() is recognised
    quint32 generation_{0};
};
