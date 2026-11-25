#pragma once

#include "audio/outputs.h"
#include "audio/sendspin_output.h"
#include "cast/cast_engine.h"
#include "client/client.h"
#include "mpris.h"
#include "notifier.h"
#include "paired_servers.h"
#include "persistence.h"
#include "picker/picker.h"
#include "picker/picker_window.h"
#include "settings_model.h"

#include <QAction>
#include <QIcon>
#include <QMenu>
#include <QObject>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSystemTrayIcon>

#include <filesystem>
#include <memory>
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
    SettingsModel* settings() {
        return &this->settings_;
    }
    /// @brief The Settings window; nullptr until it was first shown
    QQuickWindow* settings_window() const;

    /// @brief The tray icon's name: the mark with its waves while Sendspin plays, alone otherwise
    static QString tray_icon_name(bool sendspin_plays);
    /// @brief The icon of that name, as the tray takes it
    static QIcon tray_icon(const QString& name);

public slots:
    /// @brief Opens the picker: a launch, or a second one pinging this instance
    /// @param activationToken that launch's XDG activation token; empty for none
    void showPicker(const QString& activationToken = {});
    /// @brief Opens the Settings window, or raises the one that is open
    void showSettings();

private:
    enum class Section { Current = -1, ThisPc, Speakers, MusicAssistant };
    /// @param activation_token the XDG activation token of the click that asks for it, if any
    void show_settings_(Section section, const QString& activation_token = {});
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

    PairedServers paired_servers_;
    SettingsModel settings_;
    /// The Settings window, made when it is first shown
    std::unique_ptr<QQmlApplicationEngine> window_;
};
