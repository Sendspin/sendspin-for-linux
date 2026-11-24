#pragma once

#include "persistence.h"

#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>

#include <sendspin/client.h>
#include <sendspin/server/server.h>

#include <filesystem>

/// @brief The running app: the tray icon and the Sendspin server and client behind it. Nothing
/// is started yet; the server and client exist so both libraries are linked and constructed.
class App : public QObject {
    Q_OBJECT

public:
    explicit App(const std::filesystem::path& data_dir, QObject* parent = nullptr);

    QMenu* menu() {
        return &this->menu_;
    }

public slots:
    /// @brief A second launch pinged this instance
    void onSecondLaunch();

private:
    QMenu menu_;
    QSystemTrayIcon tray_;

    // The providers outlive the server and client that will use them
    ServerPersistence server_persistence_;
    ClientPersistence client_persistence_;
    sendspin::server::SendspinServer server_;
    sendspin::SendspinClient client_;
};
