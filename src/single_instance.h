#pragma once

#include <QByteArray>
#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QString>

/// @brief One instance per session: a lock file and a local socket in the session's runtime
/// directory. Not a D-Bus name: the headless mode claims it too, and needs no session bus.
class SingleInstance : public QObject {
    Q_OBJECT

public:
    /// @param dir the session's runtime directory
    explicit SingleInstance(const QString& dir, QObject* parent = nullptr);

    /// @brief Takes the instance lock, or pings the instance that holds it
    /// @param activation_token This launch's XDG_ACTIVATION_TOKEN, handed to the instance so its
    /// window may take the focus on Wayland; empty for none
    /// @return true when this process is the instance and should keep running
    bool claim(const QByteArray& activation_token = {});

signals:
    /// @brief Another launch found this instance running
    /// @param activationToken that launch's, empty when it had none
    void secondLaunch(const QString& activationToken);

private:
    bool ping(const QByteArray& activation_token) const;

    QString socket_path_;
    QLockFile lock_;
    QLocalServer server_;
};
