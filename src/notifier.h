#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QTimer>
#include <QVariantList>

/// @brief One desktop notification at a time: through org.freedesktop.Notifications, or under
/// Flatpak through the notification portal. Does nothing without a session bus or a service
/// to show it. Two of these show two notifications, each replacing only its own.
class Notifier : public QObject {
    Q_OBJECT

public:
    /// @param id the notification's name on the portal, the app's own: one for each Notifier
    explicit Notifier(const QString& id, QObject* parent = nullptr);
    /// @param service the bus name of a test's stand-in
    /// @param portal it is the portal, not org.freedesktop.Notifications
    Notifier(const QString& id, const QString& service, bool portal, QObject* parent = nullptr);

    /// @brief Shows a notification in place of the one showing
    /// @param persistent stays until close() or the user dismisses it; otherwise it goes after a
    /// while
    /// @param action the label of a button on it; empty for none
    void show(const QString& summary, const QString& body = {}, bool persistent = false,
              const QString& action = {});
    /// @brief Shows a track in place of the one showing: it goes after a while, and stays out of
    /// the desktop's list of notifications
    /// @param cover shown beside the text; null for none
    void show_track(const QString& summary, const QString& body, const QImage& cover);
    void close();

signals:
    /// The button of the notification showing was pressed
    void actionInvoked();
    /// The notification showing was clicked
    /// @param activationToken the click's XDG activation token; empty where the desktop gave none
    void clicked(const QString& activationToken);

private slots:
    void onActionInvoked(quint32 id, const QString& key);
    void onPortalActionInvoked(const QString& id, const QString& action,
                               const QVariantList& parameter);

private:
    void invoked(const QString& key, const QString& activation_token = {});
    /// What show() and show_track() tell apart
    struct Notice {
        QString summary;
        QString body;
        bool persistent{false};
        QString action{};
        QString click{};  ///< the label of the click on the notification itself
        bool transient{false};
        QImage image{};
    };
    void send_(const Notice& notice);
    void send_to_portal_(const Notice& notice);
    void remove_from_portal();
    void close_notification_(quint32 id);

    const QString name_;
    const QString service_;
    const bool portal_;
    /// Takes a passing notification off the portal, which keeps one until told
    QTimer expiry_{this};
    quint32 id_{0};
    /// Counts show() and close() calls, so a late reply to an old show() is recognised
    quint32 generation_{0};
    /// The server lists `body-markup`, so a body's `<` and `&` are escaped. True until it
    /// answers: text shown as markup is the worse mistake
    bool markup_{true};
};
