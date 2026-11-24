#pragma once

#include <QLocalServer>
#include <QLockFile>
#include <QObject>
#include <QString>

/// @brief One instance per session: a lock file and a local socket in the session's runtime
/// directory. A lock rather than a D-Bus name, which would need the app ID that is still open.
class SingleInstance : public QObject {
    Q_OBJECT

public:
    /// @param dir the session's runtime directory
    explicit SingleInstance(const QString& dir, QObject* parent = nullptr);

    /// @brief Takes the instance lock, or pings the instance that holds it
    /// @return true when this process is the instance and should keep running
    bool claim();

signals:
    /// @brief Another launch found this instance running
    void secondLaunch();

private:
    bool ping() const;

    QString socket_path_;
    QLockFile lock_;
    QLocalServer server_;
};
