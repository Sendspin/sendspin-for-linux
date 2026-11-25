#pragma once

#include "audio/outputs.h"
#include "audio/sendspin_output.h"
#include "cast/cast_engine.h"
#include "client/client.h"
#include "mpris.h"
#include "notifier.h"
#include "persistence.h"
#include "picker/picker.h"
#include "picker/picker_window.h"

#include <QAction>
#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>

#include <filesystem>
#include <vector>

/// @brief The running app: the tray icon, the picker, the Sendspin output, the cast engine that
/// plays it on speakers, the Sendspin client that offers it to a server, and MPRIS for what a
/// server plays here.
class App : public QObject {
    Q_OBJECT

public:
    explicit App(const std::filesystem::path& data_dir, QObject* parent = nullptr);
    ~App() override;

    /// @brief Creates the Sendspin output and starts the cast engine and the client. Each may
    /// fail without the others: the app runs on with what works.
    void start();

    QMenu* menu() {
        return &this->menu_;
    }
    CastEngine* cast() {
        return &this->cast_;
    }
    PickerWindow* picker_window() {
        return &this->picker_window_;
    }

    /// @brief The tray icon's name: the mark with its waves while Sendspin plays, alone otherwise
    static QString tray_icon_name(bool sendspin_plays);
    /// @brief A pairing code as it is shown: "123-456"
    static QString grouped_code(const QString& code);

public slots:
    /// @brief Opens the picker: a launch, or a second one pinging this instance
    void showPicker();
    /// @brief Opens the Settings window, once there is one
    void showSettings();

private:
    void update_tray_();
    void on_pairing_code_(const QString& code);
    void show_result_(const QString& summary);
    void on_pairing_approval_needed_(bool needed);
    /// Starts watching the outputs. What the player advertises: read once, here, as a role's
    /// formats cannot change while the client runs.
    std::vector<sendspin::AudioSupportedFormatObject> probe_formats_();

    QMenu menu_;
    QAction* status_{nullptr};
    QSystemTrayIcon tray_;
    /// The icon the tray has, so it is only set when it changes
    QString tray_icon_;
    Notifier notifier_;
    /// The notification up, when it is one to take down again
    enum class Notice { None, Code, Approval };
    Notice notice_{Notice::None};

    // The provider outlives the client that uses it
    ClientPersistence client_persistence_;
    // Before the client, which is created with what the outputs play
    Outputs outputs_;
    Client client_;
    Mpris mpris_;
    CastEngine cast_;
    // After the client and the engine, so its capture thread is gone before what it writes to
    SendspinOutput output_;
    Picker picker_;
    PickerWindow picker_window_;
};
