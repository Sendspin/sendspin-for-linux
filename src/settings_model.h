#pragma once

#include "audio/outputs.h"
#include "cast/cast_engine.h"
#include "client/client.h"
#include "discord_presence.h"
#include "mpris.h"
#include "paired_servers.h"
#include "update_checker.h"
#include "update_installer.h"

#include <QImage>
#include <QObject>
#include <QString>
#include <QUrl>
#include <QVariantList>

/// @brief What the Settings window shows and changes. Every change applies at once and is kept
/// in `~/.config/sendspin/sendspin.conf`; there is nothing to save.
class SettingsModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString deviceName READ device_name WRITE setDeviceName NOTIFY deviceNameChanged)
    Q_PROPERTY(QString defaultDeviceName READ default_device_name CONSTANT)
    Q_PROPERTY(bool deviceNameWaits READ device_name_waits NOTIFY deviceNameChanged)
    Q_PROPERTY(QVariantList outputs READ outputs NOTIFY outputsChanged)
    Q_PROPERTY(QString output READ output WRITE setOutput NOTIFY outputChanged)
    Q_PROPERTY(bool openAtLogin READ open_at_login WRITE setOpenAtLogin NOTIFY openAtLoginChanged)
    Q_PROPERTY(
        bool checkForUpdates READ check_for_updates WRITE setCheckForUpdates NOTIFY updateChanged)
    Q_PROPERTY(QString updateStatus READ update_status NOTIFY updateChanged)
    Q_PROPERTY(UpdateAction updateAction READ update_action NOTIFY updateChanged)
    Q_PROPERTY(QUrl updatePage READ update_page NOTIFY updateChanged)

    Q_PROPERTY(SpeakerModel* speakers READ speakers CONSTANT)
    Q_PROPERTY(int pairedSpeakerCount READ paired_speaker_count NOTIFY speakerCountsChanged)
    Q_PROPERTY(int unpairedSpeakerCount READ unpaired_speaker_count NOTIFY speakerCountsChanged)
    Q_PROPERTY(PairingStep pairingStep READ pairing_step NOTIFY pairingChanged)
    Q_PROPERTY(QString pairingName READ pairing_name NOTIFY pairingChanged)
    Q_PROPERTY(int pairingDigits READ pairing_digits NOTIFY pairingChanged)
    Q_PROPERTY(QString pairingMessage READ pairing_message NOTIFY pairingChanged)

    Q_PROPERTY(QString serverStatus READ server_status NOTIFY serverChanged)
    Q_PROPERTY(QString pairingCode READ pairing_code NOTIFY serverChanged)
    Q_PROPERTY(QString pairingQr READ pairing_qr NOTIFY serverChanged)
    Q_PROPERTY(bool pairingApprovalNeeded READ pairing_approval_needed NOTIFY serverChanged)
    Q_PROPERTY(QVariantList pairedServers READ paired_servers NOTIFY pairedServersChanged)
    Q_PROPERTY(bool unpairedAccess READ unpaired_access WRITE setUnpairedAccess NOTIFY
                   unpairedAccessChanged)
    Q_PROPERTY(
        bool mediaControls READ media_controls WRITE setMediaControls NOTIFY mediaControlsChanged)
    Q_PROPERTY(bool trackNotifications READ track_notifications WRITE setTrackNotifications NOTIFY
                   trackNotificationsChanged)
    Q_PROPERTY(bool discordAvailable READ discord_available CONSTANT)
    Q_PROPERTY(bool discordRichPresence READ discord_rich_presence WRITE setDiscordRichPresence
                   NOTIFY discordRichPresenceChanged)

public:
    /// Where pairing a speaker from Settings stands
    enum class PairingStep {
        Idle,
        Starting,    ///< the speaker was asked
        EnterCode,   ///< pairingDigits digits, from the speaker
        EnterToken,  ///< the speaker's "SP:0…" token
        Waiting,     ///< for the speaker's own button; pairingMessage says so
        Checking,    ///< what was entered is with the speaker
        Failed,      ///< pairingMessage says why
    };
    Q_ENUM(PairingStep)

    /// What there is to do about a newer release
    enum class UpdateAction {
        None,     ///< nothing newer, not known yet, on its way in, or the system's to update
        Install,  ///< installUpdate() takes it from here
        Open,     ///< nothing here installs it: updatePage has it
    };
    Q_ENUM(UpdateAction)

    // What was stored, for what starts before there is a model
    static QString stored_device_name();
    static QByteArray stored_output();
    static bool stored_unpaired_access();
    static bool stored_media_controls();
    static bool stored_track_notifications();
    static bool stored_discord_rich_presence();

    SettingsModel(Client& client, CastEngine& cast, Outputs& outputs, Mpris& mpris,
                  DiscordPresence& discord, PairedServers& servers, UpdateChecker& updates,
                  UpdateInstaller& installer, QObject* parent = nullptr);

    QString device_name() const;
    QString default_device_name() const;
    /// @brief The name was changed and servers still see the old one: the library takes its
    /// name when it starts (docs/SPEC.md, "Depends on")
    bool device_name_waits() const;
    /// @brief `{name, description}` for each choice; the first, with no name, follows the
    /// system default. A chosen output that is unplugged stays listed.
    QVariantList outputs() const;
    QString output() const;
    bool open_at_login() const;

    bool check_for_updates() const;
    /// @brief The newer release and who installs it, or how installing it goes; empty when
    /// there is nothing newer
    QString update_status() const;
    UpdateAction update_action() const;
    QUrl update_page() const;

    SpeakerModel* speakers() const;
    int paired_speaker_count() const;
    int unpaired_speaker_count() const;
    PairingStep pairing_step() const {
        return this->step_;
    }
    QString pairing_name() const {
        return this->pairing_name_;
    }
    int pairing_digits() const {
        return this->pairing_digits_;
    }
    QString pairing_message() const {
        return this->pairing_message_;
    }

    QString server_status() const;
    /// @brief The code to type into the server, "123-456"; empty when there is none or it is a
    /// QR code
    QString pairing_code() const;
    /// @brief The QR code a pairing server asked for, as an image URL; empty when there is none
    QString pairing_qr() const;
    bool pairing_approval_needed() const;
    /// @brief `{name, pairedAt}`, the latest first
    QVariantList paired_servers() const;
    bool unpaired_access() const;
    bool media_controls() const;
    bool track_notifications() const;
    /// @brief This build has a Discord application to show itself as; the switch is hidden
    /// without one
    bool discord_available() const;
    bool discord_rich_presence() const;

    /// @brief text as a QR code, one pixel a module, with its quiet zone
    static QImage qr_image(const QString& text);
    /// @brief What pairingQr names; null when no QR code is asked for
    QImage pairing_qr_image() const;

public slots:
    /// An empty name is the default again
    void setDeviceName(const QString& name);
    void setOutput(const QString& node_name);
    void setOpenAtLogin(bool open);
    void setCheckForUpdates(bool check);
    void installUpdate();
    void setUnpairedAccess(bool allowed);
    void setMediaControls(bool enabled);
    void setTrackNotifications(bool enabled);
    void setDiscordRichPresence(bool enabled);

    // id is a row's SpeakerModel::IdRole

    /// @brief Starts pairing a speaker by the best method it offers. It is not added to the
    /// group.
    void pair(const QString& id);
    void submitPairingCode(const QString& code);
    void submitPairingToken(const QString& token);
    /// @brief Ends the pairing under way, or dismisses how the last one ended
    void cancelPairing();
    void forget(const QString& id);
    /// @brief Pairs whichever speaker on the network a pasted token is for
    /// @return false when it is no speaker's here
    bool pairByToken(const QString& token);

    void allowPairing();
    /// @brief Puts the digits a pairing server asks for on the clipboard: the six alone, as the
    /// server's field takes them
    /// @return false when no digits are showing; a QR code's token is never copied
    bool copyPairingCode();
    /// @brief Puts this PC's pairing token on the clipboard, and nowhere else
    bool copyPairingToken();

signals:
    void deviceNameChanged();
    void outputsChanged();
    void outputChanged();
    void openAtLoginChanged();
    void updateChanged();
    void speakerCountsChanged();
    void pairingChanged();
    /// copyPairingCode() copied it, whoever asked
    void pairingCodeCopied();
    void serverChanged();
    void pairedServersChanged();
    void unpairedAccessChanged();
    void mediaControlsChanged();
    void trackNotificationsChanged();
    void discordRichPresenceChanged();

private:
    void set_step_(PairingStep step, const QString& message = {});
    void on_code_needed_(const QString& id, sendspin::server::PairMethod method,
                         sendspin::server::CodeFormat format);
    void on_pairing_finished_(const QString& id, sendspin::server::PairingResult result);

    Client& client_;
    CastEngine& cast_;
    Outputs& outputs_;
    Mpris& mpris_;
    DiscordPresence& discord_;
    PairedServers& servers_;
    UpdateChecker& updates_;
    UpdateInstaller& installer_;
    /// The name the client started with
    const QString running_name_;

    QString pairing_id_;
    QString pairing_name_;
    PairingStep step_{PairingStep::Idle};
    int pairing_digits_{6};
    QString pairing_message_;
    int qr_serial_{0};
};
