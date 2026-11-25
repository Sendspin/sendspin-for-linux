#include "app.h"

#include "app_id.h"
#include "device_name.h"

#include <QApplication>
#include <QIcon>
#include <QLoggingCategory>
#include <QSettings>
#include <QStandardPaths>
#include <QStyle>

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
      cast_(data_dir, output_) {
    qCInfo(lc_app, "PipeWire %s", pw_get_library_version());
    this->mpris_.set_enabled(settings().value(SETTING_MPRIS, true).toBool());

    this->status_ = this->menu_.addAction(QString());
    this->status_->setEnabled(false);
    this->menu_.addSeparator();
    this->menu_.addAction(tr("Settings…"))->setEnabled(false);
    this->menu_.addAction(tr("Quit"), qApp, &QCoreApplication::quit);
    this->update_status_();

    this->tray_.setIcon(QIcon::fromTheme(QIcon::ThemeIcon::MultimediaPlayer,
                                         qApp->style()->standardIcon(QStyle::SP_MediaVolume)));
    this->tray_.setToolTip(tr("Sendspin"));
    this->tray_.setContextMenu(&this->menu_);
    this->tray_.show();

    connect(&this->client_, &Client::stateChanged, this, &App::update_status_);
    connect(&this->client_, &Client::serverNameChanged, this, &App::update_status_);
    connect(&this->client_.player(), &Player::streamingChanged, this, &App::update_status_);
    connect(&this->client_.now_playing(), &NowPlaying::changed, this, &App::update_status_);
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

    // Both get every block. Which of the two the sound goes to is the picker's choice, still to
    // come: the cast takes it while a speaker is added, the server while it has the PC as input.
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

QString App::status_text(Client::State state, const QString& server_name, const Track& playing) {
    switch (state) {
        case Client::State::Stopped:
            break;
        case Client::State::Waiting:
            return tr("Waiting for a server");
        case Client::State::Connected:
            if (!playing.title.isEmpty()) {
                return playing.artist.isEmpty() ? playing.title
                                                : tr("%1 — %2").arg(playing.title, playing.artist);
            }
            return tr("Connected to %1").arg(server_name);
        case Client::State::Pairing:
            return server_name.isEmpty() ? tr("Pairing…") : tr("Pairing with %1…").arg(server_name);
    }
    return tr("Not connected");
}

QString App::grouped_code(const QString& code) {
    return code.size() == 6 ? code.first(3) + QLatin1Char('-') + code.last(3) : code;
}

void App::update_status_() {
    this->status_->setText(status_text(
        this->client_.state(), this->client_.server_name(),
        this->client_.player().streaming() ? this->client_.now_playing().track() : Track{}));
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
    this->notifier_.show(tr("Pairing code: %1").arg(grouped_code(code)),
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

void App::onSecondLaunch() {
    // Opens the Settings window once there is one
    qCInfo(lc_app, "second launch");
    this->tray_.showMessage(tr("Sendspin"), tr("Sendspin is already running."));
}
