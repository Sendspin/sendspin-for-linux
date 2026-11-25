#include "settings_model.h"

#include "autostart.h"
#include "device_name.h"
#include "pairing_clipboard.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QImage>
#include <QLocale>
#include <QSettings>
#include <qrcodegen.hpp>

#include <algorithm>
#include <stdexcept>

using sendspin::server::CodeFormat;
using sendspin::server::PairingResult;
using sendspin::server::PairMethod;

namespace {

constexpr const char* KEY_NAME = "client/name";      ///< empty or unset for the hostname
constexpr const char* KEY_OUTPUT = "player/output";  ///< a node.name; unset for the default
constexpr const char* KEY_MPRIS = "mpris/enabled";   ///< on unless set false
constexpr const char* KEY_TRACKS = "notifications/now_playing";  ///< off unless set true
constexpr const char* KEY_DISCORD = "discord/rich_presence";     ///< off unless set true
constexpr const char* KEY_UNPAIRED = "client/unpaired_access";   ///< on unless set false

QSettings store() {
    return QSettings(QStringLiteral("sendspin"), QStringLiteral("sendspin"));
}

QString why_it_failed(PairingResult result) {
    switch (result) {
        case PairingResult::CODE_MISMATCH:
            return SettingsModel::tr("That code didn't match the speaker's.");
        case PairingResult::TOKEN_MISMATCH:
            return SettingsModel::tr("That token isn't this speaker's.");
        case PairingResult::METHOD_NOT_SUPPORTED:
            return SettingsModel::tr("The speaker refused this way of pairing.");
        case PairingResult::CLIENT_TIMED_OUT:
        case PairingResult::TIMED_OUT:
            return SettingsModel::tr("Pairing timed out.");
        case PairingResult::CLIENT_CANCELLED:
            return SettingsModel::tr("Pairing was cancelled at the speaker.");
        case PairingResult::CONCURRENT_ATTEMPT:
            return SettingsModel::tr("The speaker is busy pairing with another server.");
        case PairingResult::CONNECTION_LOST:
            return SettingsModel::tr("The connection to the speaker was lost.");
        default:
            return SettingsModel::tr("Pairing failed.");
    }
}

}  // namespace

QString SettingsModel::stored_device_name() {
    const QString name = store().value(KEY_NAME).toString().trimmed();
    return name.isEmpty() ? ::device_name() : name;
}

QByteArray SettingsModel::stored_output() {
    return store().value(KEY_OUTPUT).toByteArray();
}

bool SettingsModel::stored_unpaired_access() {
    return store().value(KEY_UNPAIRED, true).toBool();
}

bool SettingsModel::stored_media_controls() {
    return store().value(KEY_MPRIS, true).toBool();
}

bool SettingsModel::stored_track_notifications() {
    return store().value(KEY_TRACKS, false).toBool();
}

bool SettingsModel::stored_discord_rich_presence() {
    return store().value(KEY_DISCORD, false).toBool();
}

SettingsModel::SettingsModel(Client& client, CastEngine& cast, Outputs& outputs, Mpris& mpris,
                             DiscordPresence& discord, PairedServers& servers,
                             UpdateChecker& updates, UpdateInstaller& installer, QObject* parent)
    : QObject(parent),
      client_(client),
      cast_(cast),
      outputs_(outputs),
      mpris_(mpris),
      discord_(discord),
      servers_(servers),
      updates_(updates),
      installer_(installer),
      running_name_(client.name()) {
    connect(&updates, &UpdateChecker::changed, this, &SettingsModel::updateChanged);
    connect(&installer, &UpdateInstaller::changed, this, &SettingsModel::updateChanged);
    connect(&outputs, &Outputs::changed, this, &SettingsModel::outputsChanged);

    const SpeakerModel* speakers = cast.speakers();
    connect(speakers, &QAbstractItemModel::modelReset, this, &SettingsModel::speakerCountsChanged);
    connect(speakers, &QAbstractItemModel::dataChanged, this, &SettingsModel::speakerCountsChanged);

    connect(&cast, &CastEngine::pairingCodeNeeded, this, &SettingsModel::on_code_needed_);
    connect(&cast, &CastEngine::pairingPending, this,
            [this](const QString& id, const QString& message) {
                if (id != this->pairing_id_) {
                    return;
                }
                // A code already asked for can be entered meanwhile; the server holds it
                const bool entering =
                    this->step_ == PairingStep::EnterCode || this->step_ == PairingStep::EnterToken;
                this->set_step_(entering ? this->step_ : PairingStep::Waiting,
                                message.isEmpty()
                                    ? tr("The speaker is waiting for its own button to be pressed.")
                                    : message);
            });
    connect(&cast, &CastEngine::pairingFinished, this, &SettingsModel::on_pairing_finished_);

    connect(&client, &Client::stateChanged, this, &SettingsModel::serverChanged);
    connect(&client, &Client::serverNameChanged, this, &SettingsModel::serverChanged);
    connect(&client, &Client::pairingCodeChanged, this, [this] {
        this->qr_serial_++;
        emit this->serverChanged();
    });
    connect(&client, &Client::pairingApprovalNeededChanged, this, &SettingsModel::serverChanged);
    connect(&client, &Client::paired, this, [this](const QString& name, const QString& id) {
        this->servers_.add(id, name, QDateTime::currentDateTime());
        emit this->pairedServersChanged();
    });
}

// ==============================================================================
// This PC
// ==============================================================================

QString SettingsModel::device_name() const {
    return stored_device_name();
}

QString SettingsModel::default_device_name() const {
    return ::device_name();
}

bool SettingsModel::device_name_waits() const {
    return this->device_name() != this->running_name_;
}

void SettingsModel::setDeviceName(const QString& name) {
    const QString wanted = name.trimmed();
    if ((wanted.isEmpty() ? this->default_device_name() : wanted) == this->device_name()) {
        return;
    }
    QSettings settings = store();
    if (wanted.isEmpty() || wanted == this->default_device_name()) {
        // Not kept, so it follows a hostname that changes
        settings.remove(KEY_NAME);
    } else {
        settings.setValue(KEY_NAME, wanted);
    }
    emit this->deviceNameChanged();
}

QVariantList SettingsModel::outputs() const {
    QVariantList list{QVariantMap{{"name", QString()}, {"description", tr("System default")}}};
    const QString chosen = this->output();
    bool listed = chosen.isEmpty();
    for (const OutputSink& sink : this->outputs_.choices()) {
        const QString name = QString::fromUtf8(sink.name);
        listed = listed || name == chosen;
        list.append(QVariantMap{{"name", name}, {"description", sink.description}});
    }
    // Never the Sendspin output, even written into the file by hand
    if (!listed && !this->outputs_.is_own(chosen.toUtf8())) {
        list.append(
            QVariantMap{{"name", chosen}, {"description", tr("%1 (not connected)").arg(chosen)}});
    }
    return list;
}

QString SettingsModel::output() const {
    return QString::fromUtf8(stored_output());
}

void SettingsModel::setOutput(const QString& node_name) {
    if (node_name == this->output()) {
        return;
    }
    QSettings settings = store();
    if (node_name.isEmpty()) {
        settings.remove(KEY_OUTPUT);
    } else {
        settings.setValue(KEY_OUTPUT, node_name);
    }
    // Tells of the outputs too: an unplugged choice is listed only while it is the choice
    this->outputs_.set_chosen(node_name.toUtf8());
    emit this->outputChanged();
}

bool SettingsModel::open_at_login() const {
    return autostart::enabled();
}

void SettingsModel::setOpenAtLogin(bool open) {
    if (open != autostart::enabled()) {
        // Told even of a refusal, which puts the switch back
        autostart::set_enabled(open, {}, this, [this] { emit this->openAtLoginChanged(); });
    }
}

// ==============================================================================
// Speakers
// ==============================================================================

SpeakerModel* SettingsModel::speakers() const {
    return this->cast_.speakers();
}

int SettingsModel::paired_speaker_count() const {
    return static_cast<int>(
        std::ranges::count(this->cast_.speakers()->rows(), true, &SpeakerRow::paired));
}

int SettingsModel::unpaired_speaker_count() const {
    return static_cast<int>(this->cast_.speakers()->rows().size()) - this->paired_speaker_count();
}

void SettingsModel::set_step_(PairingStep step, const QString& message) {
    this->step_ = step;
    this->pairing_message_ = message;
    if (step == PairingStep::Idle) {
        this->pairing_id_.clear();
        this->pairing_name_.clear();
    }
    emit this->pairingChanged();
}

void SettingsModel::pair(const QString& id) {
    if (!this->pairing_id_.isEmpty()) {
        this->cancelPairing();
    }
    this->pairing_id_ = id;
    for (const SpeakerRow& row : this->cast_.speakers()->rows()) {
        if (row.id == id) {
            this->pairing_name_ = row.name;
        }
    }
    if (this->cast_.pair(id)) {
        this->set_step_(PairingStep::Starting);
    } else {
        this->set_step_(PairingStep::Failed, tr("This speaker can't be paired right now."));
    }
}

void SettingsModel::on_code_needed_(const QString& id, PairMethod method, CodeFormat format) {
    if (id != this->pairing_id_) {
        return;
    }
    // Asked again after an entry: that one was wrong
    const bool again = this->step_ == PairingStep::Checking;
    this->pairing_digits_ = method == PairMethod::STATIC_PAIRING_CODE ? 8 : 6;
    this->set_step_(format == CodeFormat::TOKEN ? PairingStep::EnterToken : PairingStep::EnterCode,
                    again ? tr("That didn't match. Try again.") : QString());
}

void SettingsModel::submitPairingCode(const QString& code) {
    if (this->step_ != PairingStep::EnterCode) {
        return;
    }
    if (this->cast_.submitPairingCode(this->pairing_id_, code)) {
        this->set_step_(PairingStep::Checking);
    } else {
        this->set_step_(PairingStep::Failed, tr("The speaker is no longer there."));
    }
}

void SettingsModel::submitPairingToken(const QString& token) {
    if (this->step_ != PairingStep::EnterToken) {
        return;
    }
    if (this->cast_.submitPairingToken(this->pairing_id_, token)) {
        this->set_step_(PairingStep::Checking);
    } else {
        this->set_step_(PairingStep::EnterToken, tr("That isn't this speaker's token."));
    }
}

void SettingsModel::cancelPairing() {
    if (this->pairing_id_.isEmpty()) {
        return;
    }
    const QString id = this->pairing_id_;
    const bool under_way = this->step_ != PairingStep::Failed;
    // First, so the result of the cancel is not shown as a failure
    this->set_step_(PairingStep::Idle);
    if (under_way) {
        this->cast_.cancelPairing(id);
    }
}

void SettingsModel::on_pairing_finished_(const QString& id, PairingResult result) {
    if (id != this->pairing_id_) {
        return;
    }
    if (result == PairingResult::CANCELLED && this->step_ == PairingStep::Starting) {
        // The end of an earlier pairing of this speaker, which starting this one cancelled
        return;
    }
    if (result == PairingResult::PAIRED || result == PairingResult::CANCELLED) {
        this->set_step_(PairingStep::Idle);
    } else {
        this->set_step_(PairingStep::Failed, why_it_failed(result));
    }
}

void SettingsModel::forget(const QString& id) {
    this->cast_.forget(id);
}

bool SettingsModel::pairByToken(const QString& token) {
    if (!this->pairing_id_.isEmpty()) {
        this->cancelPairing();
    }
    // The token names its speaker; the server takes it only for that one
    for (const SpeakerRow& row : this->cast_.speakers()->rows()) {
        if (!row.paired && this->cast_.submitPairingToken(row.id, token)) {
            this->pairing_id_ = row.id;
            this->pairing_name_ = row.name;
            this->set_step_(PairingStep::Checking);
            return true;
        }
    }
    return false;
}

// ==============================================================================
// Music Assistant
// ==============================================================================

QString SettingsModel::server_status() const {
    const QString name = this->client_.server_name();
    switch (this->client_.state()) {
        case Client::State::Connected:
            return tr("Connected to %1").arg(name);
        case Client::State::Pairing:
            return name.isEmpty() ? tr("A server is pairing with this PC")
                                  : tr("%1 is pairing with this PC").arg(name);
        default:
            return tr("No server is connected");
    }
}

QString SettingsModel::pairing_code() const {
    const QString code = this->client_.pairing_code();
    if (this->client_.pairing_code_is_qr()) {
        return {};
    }
    return code.size() == 6 ? code.first(3) + QLatin1Char('-') + code.last(3) : code;
}

QString SettingsModel::pairing_qr() const {
    // The image is asked for by a number, new with each code; the code stays out of the URL
    return this->client_.pairing_code_is_qr() && !this->client_.pairing_code().isEmpty()
               ? QStringLiteral("image://qr/%1").arg(this->qr_serial_)
               : QString();
}

QImage SettingsModel::qr_image(const QString& text) {
    constexpr int QUIET = 4;
    try {
        const auto code = qrcodegen::QrCode::encodeText(text.toUtf8().constData(),
                                                        qrcodegen::QrCode::Ecc::MEDIUM);
        const int size = code.getSize();
        QImage image(size + (2 * QUIET), size + (2 * QUIET), QImage::Format_RGB32);
        image.fill(Qt::white);
        for (int y = 0; y < size; y++) {
            for (int x = 0; x < size; x++) {
                if (code.getModule(x, y)) {
                    image.setPixel(x + QUIET, y + QUIET, qRgb(0, 0, 0));
                }
            }
        }
        return image;
    } catch (const std::length_error&) {
        // More than a QR code holds
        return {};
    }
}

QImage SettingsModel::pairing_qr_image() const {
    return this->pairing_qr().isEmpty() ? QImage() : qr_image(this->client_.pairing_code());
}

bool SettingsModel::pairing_approval_needed() const {
    return this->client_.pairing_approval_needed();
}

void SettingsModel::allowPairing() {
    this->client_.allow_pairing();
}

QVariantList SettingsModel::paired_servers() const {
    QVariantList list;
    for (const PairedServer& server : this->servers_.list()) {
        list.append(QVariantMap{
            {"name", server.name.isEmpty() ? tr("A server") : server.name},
            {"pairedAt", QLocale().toString(server.paired_at.date(), QLocale::ShortFormat)}});
    }
    return list;
}

bool SettingsModel::copyPairingCode() {
    const QString code = this->client_.pairing_code();
    if (code.isEmpty() || this->client_.pairing_code_is_qr()) {
        return false;
    }
    pairing_clipboard::copy(code);
    emit this->pairingCodeCopied();
    return true;
}

bool SettingsModel::copyPairingToken() {
    const QString token = this->client_.pairing_token();
    if (token.isEmpty()) {
        return false;
    }
    QGuiApplication::clipboard()->setText(token);
    return true;
}

bool SettingsModel::unpaired_access() const {
    return stored_unpaired_access();
}

void SettingsModel::setUnpairedAccess(bool allowed) {
    if (allowed == this->unpaired_access()) {
        return;
    }
    store().setValue(KEY_UNPAIRED, allowed);
    this->client_.set_unpaired_access(allowed);
    emit this->unpairedAccessChanged();
}

bool SettingsModel::media_controls() const {
    return stored_media_controls();
}

void SettingsModel::setMediaControls(bool enabled) {
    if (enabled == this->media_controls()) {
        return;
    }
    store().setValue(KEY_MPRIS, enabled);
    this->mpris_.set_enabled(enabled);
    emit this->mediaControlsChanged();
}

bool SettingsModel::track_notifications() const {
    return stored_track_notifications();
}

void SettingsModel::setTrackNotifications(bool enabled) {
    if (enabled == this->track_notifications()) {
        return;
    }
    store().setValue(KEY_TRACKS, enabled);
    emit this->trackNotificationsChanged();
}

bool SettingsModel::discord_available() const {
    return this->discord_.available();
}

bool SettingsModel::discord_rich_presence() const {
    return stored_discord_rich_presence();
}

void SettingsModel::setDiscordRichPresence(bool enabled) {
    if (enabled == this->discord_rich_presence()) {
        return;
    }
    store().setValue(KEY_DISCORD, enabled);
    this->discord_.set_enabled(enabled);
    emit this->discordRichPresenceChanged();
}

bool SettingsModel::check_for_updates() const {
    return this->updates_.enabled();
}

void SettingsModel::setCheckForUpdates(bool check) {
    this->updates_.set_enabled(check);
}

QString SettingsModel::update_status() const {
    using State = UpdateInstaller::State;
    using Failure = UpdateInstaller::Failure;
    const std::optional<Release> release = this->updates_.update();
    if (!release) {
        return {};
    }
    const QString version = release->version.toString();
    switch (this->installer_.state()) {
        case State::DOWNLOADING:
            return tr("Downloading Sendspin %1… %2%").arg(version).arg(this->installer_.percent());
        case State::VERIFYING:
            return tr("Checking the download…");
        case State::INSTALLING:
            return tr("Installing Sendspin %1…").arg(version);
        case State::INSTALLED:
            return tr("Sendspin %1 is installed. Restarting…").arg(version);
        case State::FAILED:
            switch (this->installer_.failure()) {
                case Failure::DOWNLOAD:
                    return tr("Sendspin %1 could not be downloaded.").arg(version);
                case Failure::SIGNATURE:
                    return tr("The download of Sendspin %1 isn't signed by Sendspin's key, so "
                              "it was not installed.")
                        .arg(version);
                default:
                    return tr("Sendspin %1 could not be installed.").arg(version);
            }
        case State::IDLE:
            break;
    }
    if (this->installer_.sandboxed()) {
        return tr("Sendspin %1 is available. Flatpak installs it.").arg(version);
    }
    if (this->installer_.origin() == update::Origin::REPOSITORY) {
        return tr("Sendspin %1 is available. Your system's updates install it.").arg(version);
    }
    return tr("Sendspin %1 is available.").arg(version);
}

SettingsModel::UpdateAction SettingsModel::update_action() const {
    const std::optional<Release> release = this->updates_.update();
    if (!release || this->installer_.busy() ||
        this->installer_.state() == UpdateInstaller::State::INSTALLED ||
        !this->installer_.probed()) {
        return UpdateAction::None;
    }
    if (this->installer_.can_install(*release)) {
        return UpdateAction::Install;
    }
    // The system's own updates need no page
    return this->installer_.origin() == update::Origin::REPOSITORY ? UpdateAction::None
                                                                   : UpdateAction::Open;
}

QUrl SettingsModel::update_page() const {
    const std::optional<Release> release = this->updates_.update();
    return release ? release->page : QUrl();
}

void SettingsModel::installUpdate() {
    if (const std::optional<Release> release = this->updates_.update()) {
        this->installer_.install(*release);
    }
}
