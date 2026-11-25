#pragma once

#include "audio/sendspin_output.h"
#include "client/client.h"
#include "notifier.h"
#include "persistence.h"

#include <QAction>
#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>

#include <sendspin/server/server.h>

#include <filesystem>

/// @brief The running app: the tray icon, the Sendspin output, and the Sendspin server and
/// client behind them. The server exists but is not started yet.
class App : public QObject {
    Q_OBJECT

public:
    explicit App(const std::filesystem::path& data_dir, QObject* parent = nullptr);
    ~App() override;

    /// @brief Creates the Sendspin output and starts the client. Either may fail without the
    /// other: the app runs on with what works.
    void start();

    QMenu* menu() {
        return &this->menu_;
    }

    /// @brief The tray's status line for a client state
    static QString status_text(Client::State state, const QString& server_name);
    /// @brief A pairing code as it is shown: "123-456"
    static QString grouped_code(const QString& code);

public slots:
    /// @brief A second launch pinged this instance
    void onSecondLaunch();

private:
    void update_status_();
    void on_pairing_code_(const QString& code);
    void show_result_(const QString& summary);
    void on_pairing_approval_needed_(bool needed);

    QMenu menu_;
    QAction* status_{nullptr};
    QSystemTrayIcon tray_;
    Notifier notifier_;
    /// The notification up, when it is one to take down again
    enum class Notice { None, Code, Approval };
    Notice notice_{Notice::None};

    // The providers outlive the server and client that use them
    ServerPersistence server_persistence_;
    ClientPersistence client_persistence_;
    sendspin::server::SendspinServer server_;
    Client client_;
    // After the client, so its capture thread is gone before the client it writes to
    SendspinOutput output_;
};
