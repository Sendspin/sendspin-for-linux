#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

/// @brief One desktop notification at a time: through org.freedesktop.Notifications, or under
/// Flatpak through the notification portal. Does nothing without a session bus or a service
/// to show it.
class Notifier : public QObject {
    Q_OBJECT

public:
    explicit Notifier(QObject* parent = nullptr);
    /// @param portal the portal's bus name, a test's stand-in; empty for
    /// org.freedesktop.Notifications
    explicit Notifier(const QString& portal, QObject* parent = nullptr);

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
    void onPortalActionInvoked(const QString& id, const QString& action,
                               const QVariantList& parameter);

private:
    void invoked(const QString& key);
    void show_on_portal(const QString& summary, const QString& body, bool persistent,
                        const QString& action);

    QString portal_;
    quint32 id_{0};
    /// Counts show() and close() calls, so a late reply to an old show() is recognised
    quint32 generation_{0};
};
