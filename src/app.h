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
#include "track_announcer.h"

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
/// server plays here. And the updater: what checks for a newer release, and what installs it.
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
    TrackAnnouncer* announcer() {
        return &this->announcer_;
    }
    /// @brief What shows the track a server plays here
    Notifier* track_notifier() {
        return &this->track_notifier_;
    }
    /// @brief The Settings window; nullptr until it was first shown
    QQuickWindow* settings_window() const;

    /// @brief What the notification says under a pairing code
    /// @param copied the code is on the clipboard; otherwise Settings is where to copy it
    static QString code_notice_body(const QString& server_name, bool copied);
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
    /// @brief What a click on a pairing notification does: Settings on Music Assistant, and the
    /// keyboard on Copy while digits show
    void showPairing(const QString& activationToken = {});

private:
    enum class Section { Current = -1, ThisPc, Speakers, MusicAssistant };
    /// @param activation_token the XDG activation token of the click that asks for it, if any
    void show_settings_(Section section, const QString& activation_token = {});
    void update_tray_();
    void on_pairing_code_(const QString& code);
    void show_result_(const QString& summary);
    void on_pairing_approval_needed_(bool needed);
    void show_update_();
    /// Starts the version just installed in this one's place
    void restart_();
    /// Starts watching the outputs. What the player advertises: read once, here, as a role's
    /// formats cannot change while the client runs.
    std::vector<sendspin::AudioSupportedFormatObject> probe_formats_();

    /// Read before an update replaces the file
    const QString program_;
    QMenu menu_;
    QAction* status_{nullptr};
    /// The newer release, or how installing it goes; hidden without one
    QAction* update_{nullptr};
    QSystemTrayIcon tray_;
    /// The icon the tray has, so it is only set when it changes
    QString tray_icon_;
    /// The server pairing's
    Notifier notifier_{QStringLiteral("pairing")};
    /// The notification up, when it is one to take down again
    enum class Notice { None, Code, Approval };
    Notice notice_{Notice::None};
    /// The digits showing, to take off the clipboard when they go; never a QR code's token
    QString code_;

    // The provider outlives the client that uses it
    ClientPersistence client_persistence_;
    // Before the client, which is created with what the outputs play
    Outputs outputs_;
    Client client_;
    Mpris mpris_;
    /// The track a server plays here, for those who switched it on; beside the pairing's
    Notifier track_notifier_{QStringLiteral("now-playing")};
    TrackAnnouncer announcer_;
    CastEngine cast_;
    // After the client and the engine, so its capture thread is gone before what it writes to
    SendspinOutput output_;
    Picker picker_;
    PickerWindow picker_window_;

    PairedServers paired_servers_;
    UpdateChecker updates_;
    UpdateInstaller installer_;
    SettingsModel settings_;
    /// The Settings window, made when it is first shown
    std::unique_ptr<QQmlApplicationEngine> window_;
};
