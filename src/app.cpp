#include "app.h"

#include "app_id.h"
#include "device_name.h"

#include <QApplication>
#include <QIcon>
#include <QLoggingCategory>
#include <QSettings>
#include <QStandardPaths>

#include <pipewire/pipewire.h>

#include <utility>

Q_LOGGING_CATEGORY(lc_app, "sendspin.app")

namespace {

// Settings has no window yet; these are its keys
constexpr const char* SETTING_OUTPUT = "player/output";  ///< a node.name; empty for the default
constexpr const char* SETTING_MPRIS = "mpris/enabled";   ///< on unless set false
constexpr const char* SETTING_UNPAIRED = "client/unpaired_access";  ///< on unless set false

QSettings settings() {
    return QSettings(QStringLiteral("sendspin"), QStringLiteral("sendspin"));
}

/// Flatpak's place for files other apps are pointed at
QString art_dir() {
    return QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) +
           QStringLiteral("/app/") + QLatin1String(APP_ID);
}

}  // namespace

App::App(const std::filesystem::path& data_dir, QObject* parent)
    : QObject(parent),
      client_persistence_(data_dir),
      client_({.name = device_name(),
               .port = Client::free_port(),
               .unpaired_access = settings().value(SETTING_UNPAIRED, true).toBool(),
               .player_formats = this->probe_formats_()},
              &this->client_persistence_),
      mpris_(this->client_, art_dir()),
      cast_(data_dir, output_),
      picker_(cast_, client_, outputs_),
      picker_window_(picker_, cast_) {
    qCInfo(lc_app, "PipeWire %s", pw_get_library_version());
    this->mpris_.set_enabled(settings().value(SETTING_MPRIS, true).toBool());

    this->status_ = this->menu_.addAction(QString());
    this->status_->setEnabled(false);
    this->menu_.addSeparator();
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
    connect(&this->outputs_, &Outputs::targetChanged, this,
            [this] { this->client_.player().stream().set_target(this->outputs_.target()); });
    this->client_.player().stream().set_target(this->outputs_.target());
    connect(&this->client_, &Client::pairingCodeChanged, this, &App::on_pairing_code_);
    connect(&this->client_, &Client::pairingApprovalNeededChanged, this,
            &App::on_pairing_approval_needed_);
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
    this->outputs_.set_chosen(settings().value(SETTING_OUTPUT).toByteArray());
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

void App::showSettings() {
    // The Settings window is still to come
    qCInfo(lc_app, "Settings asked for");
}
