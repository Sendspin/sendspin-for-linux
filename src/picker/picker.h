#pragma once

#include "audio/outputs.h"
#include "cast/cast_engine.h"
#include "cast/speaker_menu.h"
#include "client/client.h"

#include <QLoggingCategory>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>

Q_DECLARE_LOGGING_CATEGORY(lc_picker)

/// @brief What the picker and the tray show, and what a click there does: the view over the cast
/// engine's speakers, this PC's own output and the server (docs/SPEC.md, "The picker"). The rules
/// are speaker_menu's; this gathers what they are asked about and carries out what they answer.
class Picker : public QObject {
    Q_OBJECT
    Q_PROPERTY(Dot dot READ dot NOTIFY changed)
    Q_PROPERTY(QString dotText READ dot_text NOTIFY changed)
    Q_PROPERTY(QString notice READ notice NOTIFY changed)
    Q_PROPERTY(bool hasOutputs READ has_outputs NOTIFY changed)
    Q_PROPERTY(bool volumeShown READ volume_shown NOTIFY changed)
    Q_PROPERTY(bool looking READ looking NOTIFY changed)
    Q_PROPERTY(QString localId READ local_id CONSTANT)
    Q_PROPERTY(QString localName READ local_name NOTIFY changed)
    Q_PROPERTY(Check localCheck READ local_check NOTIFY changed)
    Q_PROPERTY(bool localClickable READ local_clickable NOTIFY changed)
    Q_PROPERTY(QString localStatusText READ local_status_text NOTIFY changed)
    Q_PROPERTY(bool localWarning READ local_warning NOTIFY changed)
    Q_PROPERTY(bool serverShown READ server_shown NOTIFY changed)
    Q_PROPERTY(QString serverName READ server_name NOTIFY changed)
    Q_PROPERTY(QString serverCaption READ server_caption NOTIFY changed)
    Q_PROPERTY(QString serverStatusText READ server_status_text NOTIFY changed)
    Q_PROPERTY(bool serverHighlighted READ server_highlighted NOTIFY changed)
    Q_PROPERTY(bool serverChosen READ server_chosen NOTIFY changed)
    Q_PROPERTY(bool serverChoosable READ server_choosable NOTIFY changed)
    Q_PROPERTY(bool sendspinPlays READ sendspin_plays NOTIFY changed)
    Q_PROPERTY(QString statusLine READ status_line NOTIFY changed)

public:
    // speaker_menu's, for QML: the values are kept the same in picker.cpp
    enum class Check { NoCheck, Empty, Filled, Grey, Spinner };
    Q_ENUM(Check)
    enum class Dot { Plays, InUse, NotInUse };
    Q_ENUM(Dot)
    enum class Click { NoClick, Add, Remove };
    Q_ENUM(Click)

    /// @brief What the server row knows
    struct Server {
        QString name{};
        bool connected{false};
        bool paired{false};
        /// The pairing code showing, digits to type in; empty for none
        QString code{};
        /// The pairing code is a QR code, drawn in Settings and never spelled out
        bool qr_code{false};
        bool chosen{false};
        /// It has the PC as its input
        bool streaming{false};
    };

    /// @param outputs this PC's own output and the system's default
    Picker(CastEngine& cast, Client& client, Outputs& outputs, QObject* parent = nullptr);

    Dot dot() const;
    QString dot_text() const {
        return dot_text(this->dot());
    }
    /// @brief What stands in the way of everything; empty for nothing
    QString notice() const;
    /// @brief False without PipeWire: there are no rows to show
    bool has_outputs() const {
        return this->outputs_.connected();
    }
    /// @brief A speaker is in the group: the group's volume has something to set
    bool volume_shown() const;
    /// @brief Nothing is found or chosen yet
    bool looking() const;

    QString local_id() const;
    QString local_name() const;
    Check local_check() const {
        return static_cast<Check>(this->local_row_().check);
    }
    bool local_clickable() const {
        return this->local_row_().click != speaker_menu::Click::NONE;
    }
    QString local_status_text() const {
        return SpeakerModel::status_text(this->local_row_().status);
    }
    bool local_warning() const {
        return this->local_row_().warning;
    }

    bool server_shown() const {
        return !this->server_().name.isEmpty();
    }
    QString server_name() const {
        return this->server_().name;
    }
    QString server_caption() const {
        return server_caption(this->server_(), this->pc_name_);
    }
    QString server_status_text() const;
    bool server_highlighted() const;
    bool server_chosen() const {
        return this->server_chosen_;
    }
    bool server_choosable() const;

    /// @brief The PC's sound goes out through Sendspin: the tray icon has its waves
    bool sendspin_plays() const;
    /// @brief The tray's status line
    QString status_line() const;

    speaker_menu::Route route() const {
        return this->route_;
    }

    static QString dot_text(Dot dot);
    /// @brief A pairing code as it is shown: "123-456"
    static QString grouped_code(const QString& code);
    static QString server_caption(const Server& server, const QString& pc_name);
    /// @param playing_on the speakers that play the PC's sound
    /// @param sending_to the server that takes it; empty for none
    /// @param here the track a server plays to this PC; empty for none
    static QString status_line(const QStringList& playing_on, const QString& sending_to,
                               const Track& here);

public slots:
    // id is a speaker's SpeakerModel::IdRole, or localId

    /// @brief A row's check: adds it to the group or takes it out
    void toggle(const QString& id);
    /// @brief A row's name: play there only
    void playOnly(const QString& id);
    /// @brief The server row: the group dissolves and the server gets the PC's sound
    void chooseServer();

signals:
    void changed();

private:
    speaker_menu::State state_() const;
    speaker_menu::Row local_row_() const;
    Server server_() const;
    void apply_(const speaker_menu::Plan& plan);
    void refresh_();
    void save_();

    CastEngine& cast_;
    Client& client_;
    Outputs& outputs_;
    const QString pc_name_;
    QTimer timer_;
    // What the timer last saw of the source
    bool source_streaming_{false};
    bool source_signal_{false};

    /// This PC's own output is in the group
    bool local_ticked_{true};
    bool server_chosen_{false};
    /// The server this PC last paired with, which stays listed while it is away
    QString paired_server_;
    speaker_menu::Route route_{speaker_menu::Route::DIRECT};
    /// A choice is being carried out: the engine's changes on the way are not the speakers' own
    bool applying_{false};
};
