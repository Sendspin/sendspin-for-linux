#pragma once

#include <QObject>
#include <QString>

/// @brief One desktop notification at a time, through org.freedesktop.Notifications. Does
/// nothing without a session bus or a notification service.
class Notifier : public QObject {
    Q_OBJECT

public:
    explicit Notifier(QObject* parent = nullptr);

    /// @brief Shows a notification in place of the one showing
    /// @param persistent stays until close() or the user dismisses it
    /// @param action the label of a button on it; empty for none
    void show(const QString& summary, const QString& body = {}, bool persistent = false,
              const QString& action = {});
    /// @brief Takes down the one showing
    void close();

signals:
    /// The button of the notification showing was pressed
    void actionInvoked();
    /// The notification showing was clicked
    void clicked();

private slots:
    void onActionInvoked(quint32 id, const QString& key);

private:
    quint32 id_{0};
    /// Counts show() and close() calls, so a late reply to an old show() is recognised
    quint32 generation_{0};
};
