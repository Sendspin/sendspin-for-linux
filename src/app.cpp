#include "app.h"

#include "app_id.h"
#include "autostart.h"

#include <QApplication>
#include <QIcon>
#include <QLoggingCategory>
#include <QProcess>
#include <QQuickImageProvider>
#include <QQuickStyle>
#include <QSettings>
#include <QStandardPaths>

#include <pipewire/pipewire.h>

#include <algorithm>
#include <utility>

Q_LOGGING_CATEGORY(lc_app, "sendspin.app")

namespace {

// The player's own, kept as a server sets them; the rest are SettingsModel's
constexpr const char* SETTING_VOLUME = "player/volume";  ///< 0-100; 100 unless set
constexpr const char* SETTING_MUTED = "player/muted";

QSettings conf() {
    return QSettings(QStringLiteral("sendspin"), QStringLiteral("sendspin"));
}

/// The QR code a pairing server asked for, as `image://qr/…`: the code stays out of QML
class QrProvider final : public QQuickImageProvider {
public:
    explicit QrProvider(const SettingsModel& settings)
        : QQuickImageProvider(QQuickImageProvider::Image), settings_(settings) {}

    QImage requestImage(const QString& /*id*/, QSize* size, const QSize& requested) override {
        constexpr int MODULE_PX = 6;
        const QImage code = this->settings_.pairing_qr_image();
        if (code.isNull()) {
            return {};
        }
        // Whole pixels a module, so it stays sharp
        const int scale =
            requested.isValid() ? qMax(1, requested.width() / code.width()) : MODULE_PX;
        const QImage image = code.scaled(code.size() * scale);
        *size = image.size();
        return image;
    }

private:
    const SettingsModel& settings_;
};

/// Flatpak's place for files other apps are pointed at
QString art_dir() {
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) +
           QStringLiteral("/app/") + QLatin1String(APP_ID);
}

}  // namespace

App::App(const std::filesystem::path& data_dir, QObject* parent)
    : QObject(parent),
      program_(QCoreApplication::applicationFilePath()),
      client_persistence_(data_dir),
      client_({.name = SettingsModel::stored_device_name(),
               .port = Client::free_port(),
               .unpaired_access = SettingsModel::stored_unpaired_access(),
               .player_formats = this->probe_formats_(),
               .player_volume = static_cast<uint8_t>(
                   std::clamp(conf().value(SETTING_VOLUME, 100).toInt(), 0, 100)),
               .player_muted = conf().value(SETTING_MUTED, false).toBool()},
              &this->client_persistence_),
      mpris_(this->client_, art_dir()),
      cast_(data_dir, output_),
      picker_(cast_, client_, outputs_),
      picker_window_(picker_, cast_),
      paired_servers_(data_dir),
      // No version, in a test: no checks
      updates_(QVersionNumber::fromString(QCoreApplication::applicationVersion()),
               UpdateChecker::releases_url()),
      installer_(UpdateInstaller::this_system()),
      settings_(client_, cast_, outputs_, mpris_, paired_servers_, updates_, installer_) {
    qCInfo(lc_app, "PipeWire %s", pw_get_library_version());
    this->mpris_.set_enabled(SettingsModel::stored_media_controls());

    this->status_ = this->menu_.addAction(QString());
    this->status_->setEnabled(false);
    this->menu_.addSeparator();
    this->update_ =
        this->menu_.addAction(QString(), this, [this] { this->show_settings_(Section::ThisPc); });
    this->update_->setVisible(false);
    this->menu_.addAction(tr("Settings…"), this, &App::showSettings);
    this->menu_.addAction(tr("Quit"), qApp, &QCoreApplication::quit);
    this->update_tray_();

    this->tray_.setToolTip(tr("Sendspin"));
    this->tray_.setContextMenu(&this->menu_);
    this->tray_.show();

    connect(&this->picker_, &Picker::changed, this, &App::update_tray_);
    connect(&this->picker_window_, &PickerWindow::settingsRequested, this, &App::showSettings);
    connect(&this->tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger) {
                    this->picker_window_.toggle(PickerWindow::Place::POINTER);
                }
            });
    connect(&this->client_.player(), &Player::volumeChanged, this, [](int volume, bool muted) {
        QSettings kept = conf();
        kept.setValue(SETTING_VOLUME, volume);
        kept.setValue(SETTING_MUTED, muted);
    });
    connect(&this->outputs_, &Outputs::targetChanged, this,
            [this] { this->client_.player().stream().set_target(this->outputs_.target()); });
    this->client_.player().stream().set_target(this->outputs_.target());
    connect(&this->client_, &Client::pairingCodeChanged, this, &App::on_pairing_code_);
    connect(&this->client_, &Client::pairingApprovalNeededChanged, this,
            &App::on_pairing_approval_needed_);
    // Every notification of this one is about a server pairing
    connect(&this->notifier_, &Notifier::clicked, this, [this](const QString& activationToken) {
        this->show_settings_(Section::MusicAssistant, activationToken);
    });
    connect(&this->notifier_, &Notifier::actionInvoked, this, [this] {
        if (this->notice_ == Notice::Approval) {
            this->client_.allow_pairing();
        }
    });
    connect(&this->client_, &Client::paired, this, [this](const QString& server_name) {
        this->show_result_(server_name.isEmpty() ? tr("Paired")
                                                 : tr("Paired with %1").arg(server_name));
    });
    connect(&this->client_, &Client::pairingEnded, this,
            [this] { this->show_result_(tr("Pairing ended")); });

    const auto announce = [this] {
        const NowPlaying& now_playing = this->client_.now_playing();
        this->announcer_.update(now_playing.playing(), now_playing.track(), now_playing.artwork());
    };
    connect(&this->client_.now_playing(), &NowPlaying::changed, this, announce);
    connect(&this->announcer_, &TrackAnnouncer::announced, &this->track_notifier_,
            &Notifier::show_track);
    connect(&this->track_notifier_, &Notifier::clicked, this, &App::showPicker);
    connect(&this->settings_, &SettingsModel::trackNotificationsChanged, this, [this] {
        const bool enabled = this->settings_.track_notifications();
        this->announcer_.set_enabled(enabled);
        if (!enabled) {
            this->track_notifier_.close();
        }
    });
    this->announcer_.set_enabled(SettingsModel::stored_track_notifications());

    connect(&this->settings_, &SettingsModel::updateChanged, this, &App::show_update_);
    connect(&this->installer_, &UpdateInstaller::installed, this, &App::restart_);
    this->updates_.start();

    // Both get every block. Which of the two the sound goes to is the picker's choice: the cast
    // takes it while a speaker is added, and the picker silences the source unless the server is
    // chosen.
    this->output_.set_consumer([this](const CapturedBlock& block) {
        this->client_.source().write(block);
        this->cast_.feed().take(block);
    });
}

App::~App() {
    // The capture thread writes into the client and the cast
    this->output_.stop();
}

void App::start() {
    QString error;
    if (!this->output_.start(&error)) {
        qCWarning(lc_app, "no Sendspin output: %s", qUtf8Printable(error));
    }
    // What the entry starts may have changed since it was written
    autostart::refresh({}, &this->settings_, [this] { emit this->settings_.openAtLoginChanged(); });
    // First: its client_id comes with its start
    this->client_.start();
    // Our server never connects to our own client
    this->cast_.set_own_client(this->client_.port(), this->client_.client_id().toStdString());
    if (!this->cast_.start()) {
        qCWarning(lc_app, "casting to speakers is unavailable");
    }
}

std::vector<sendspin::AudioSupportedFormatObject> App::probe_formats_() {
    if (!this->outputs_.start()) {
        qCWarning(lc_app, "PipeWire's outputs cannot be listed");
    }
    this->outputs_.set_chosen(SettingsModel::stored_output());
    const auto target = this->outputs_.target();
    auto formats = player_formats(target ? this->outputs_.formats(*target) : SinkFormats{},
                                  this->outputs_.clock());
    QStringList listed;
    for (const auto& format : formats) {
        if (format.codec == sendspin::SendspinCodecFormat::FLAC) {
            listed << QStringLiteral("%1/%2").arg(format.sample_rate).arg(format.bit_depth);
        }
    }
    qCInfo(lc_app, "the player takes %s, as FLAC or PCM", qUtf8Printable(listed.join(u' ')));
    return formats;
}

QString App::tray_icon_name(bool sendspin_plays) {
    return sendspin_plays ? QStringLiteral("sendspin-waves-symbolic")
                          : QStringLiteral("sendspin-symbolic");
}

QIcon App::tray_icon(const QString& name) {
    // Installed, the icon theme has it: the tray then gets its name, and the panel looks it up
    // and draws it in its own colour. Uninstalled there is only the picture to hand over.
    return QIcon::fromTheme(name, QIcon(QStringLiteral(":/icons/%1.svg").arg(name)));
}

void App::update_tray_() {
    this->status_->setText(this->picker_.status_line());
    const QString name = tray_icon_name(this->picker_.sendspin_plays());
    if (name != this->tray_icon_) {
        this->tray_icon_ = name;
        this->tray_.setIcon(tray_icon(name));
    }
}

void App::show_update_() {
    const std::optional<Release> release = this->updates_.update();
    this->update_->setVisible(release.has_value());
    if (!release) {
        return;
    }
    // Who installs it is the package manager's to say, and only matters once there is one
    this->installer_.probe();
    const bool under_way = this->installer_.state() != UpdateInstaller::State::IDLE;
    this->update_->setText(under_way
                               ? this->settings_.update_status()
                               : tr("Sendspin %1 is available…").arg(release->version.toString()));
}

void App::restart_() {
    // The new one waits for this one to go: while it runs, a launch only opens its picker
    QProcess::startDetached(
        QStringLiteral("/bin/sh"),
        {QStringLiteral("-c"),
         QStringLiteral(
             "while kill -0 \"$1\" 2>/dev/null; do sleep 0.2; done; exec \"$0\" --background"),
         this->program_, QString::number(QCoreApplication::applicationPid())});
    QCoreApplication::quit();
}

void App::on_pairing_code_(const QString& code) {
    if (code.isEmpty()) {
        // The result may already have taken the code's place
        if (this->notice_ == Notice::Code) {
            this->notice_ = Notice::None;
            this->notifier_.close();
        }
        return;
    }
    this->notice_ = Notice::Code;
    const QString server_name = this->client_.server_name();
    if (this->client_.pairing_code_is_qr()) {
        // Nothing to read out: the code is on show in Settings
        this->show_settings_(Section::MusicAssistant);
        this->notifier_.show(
            tr("Scan the code to pair this PC"),
            server_name.isEmpty()
                ? tr("It is in Sendspin's settings.")
                : tr("%1 asked for it. It is in Sendspin's settings.").arg(server_name),
            true);
        return;
    }
    this->notifier_.show(tr("Pairing code: %1").arg(Picker::grouped_code(code)),
                         server_name.isEmpty()
                             ? tr("Enter it in the server to pair this PC.")
                             : tr("Enter it in %1 to pair this PC.").arg(server_name),
                         true);
}

void App::on_pairing_approval_needed_(bool needed) {
    if (needed) {
        // The library holds pairing back after too many wrong codes, until someone here allows it
        this->notice_ = Notice::Approval;
        this->notifier_.show(tr("A server wants to pair with this PC"),
                             tr("Pairing was paused after too many wrong codes."), true,
                             tr("Allow pairing"));
    } else if (this->notice_ == Notice::Approval) {
        this->notice_ = Notice::None;
        this->notifier_.close();
    }
}

void App::show_result_(const QString& summary) {
    this->notice_ = Notice::None;
    this->notifier_.show(summary);
}

void App::showPicker(const QString& activationToken) {
    this->picker_window_.show(PickerWindow::Place::CENTRE, activationToken);
}

QQuickWindow* App::settings_window() const {
    if (!this->window_ || this->window_->rootObjects().isEmpty()) {
        return nullptr;
    }
    return qobject_cast<QQuickWindow*>(this->window_->rootObjects().first());
}

void App::showSettings() {
    this->show_settings_(Section::Current);
}

void App::show_settings_(Section section, const QString& activation_token) {
    if (!this->window_) {
        // As the picker does, whichever opens first: Fusion takes the platform's palette
        if (QQuickStyle::name().isEmpty() &&
            qEnvironmentVariableIsEmpty("QT_QUICK_CONTROLS_STYLE")) {
            QQuickStyle::setStyle(QStringLiteral("Fusion"));
        }
        qmlRegisterUncreatableType<SettingsModel>("Sendspin", 1, 0, "SettingsModel",
                                                  QStringLiteral("made by the app"));
        this->window_ = std::make_unique<QQmlApplicationEngine>();
        // The engine owns the provider
        this->window_->addImageProvider(QStringLiteral("qr"), new QrProvider(this->settings_));
        this->window_->setInitialProperties(
            {{QStringLiteral("settings"), QVariant::fromValue(&this->settings_)}});
        this->window_->load(QUrl(QStringLiteral("qrc:/qml/Settings.qml")));
    }
    QQuickWindow* window = this->settings_window();
    if (window == nullptr) {
        qCWarning(lc_app, "the Settings window did not load");
        return;
    }
    if (section != Section::Current) {
        window->setProperty("section", static_cast<int>(section));
    }
    if (!activation_token.isEmpty()) {
        // Where Qt's Wayland plugin looks for the token when a window asks for the focus
        qputenv("XDG_ACTIVATION_TOKEN", activation_token.toLatin1());
    }
    window->show();
    window->raise();
    window->requestActivate();
}
