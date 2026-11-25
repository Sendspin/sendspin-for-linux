#include "picker/picker.h"

#include "device_name.h"

#include <QLoggingCategory>
#include <QSettings>

#include <algorithm>
#include <utility>

Q_LOGGING_CATEGORY(lc_picker, "sendspin.picker")

using speaker_menu::Action;
using speaker_menu::Route;

namespace {

constexpr int REFRESH_MS = 500;
/// This PC's own output among the speakers' ids: those are a key's encoding or a URL, never this
const char* const LOCAL = "local";

constexpr const char* SETTING_LOCAL = "picker/local";
constexpr const char* SETTING_SERVER_CHOSEN = "picker/server_chosen";
constexpr const char* SETTING_PAIRED_SERVER = "picker/paired_server";

QSettings settings() {
    return QSettings(QStringLiteral("sendspin"), QStringLiteral("sendspin"));
}

template <typename Ours, typename Theirs>
constexpr bool same(Ours ours, Theirs theirs) {
    return static_cast<int>(ours) == static_cast<int>(theirs);
}
static_assert(same(Picker::Check::NoCheck, speaker_menu::Check::NONE) &&
              same(Picker::Check::Empty, speaker_menu::Check::EMPTY) &&
              same(Picker::Check::Filled, speaker_menu::Check::FILLED) &&
              same(Picker::Check::Grey, speaker_menu::Check::GREY) &&
              same(Picker::Check::Spinner, speaker_menu::Check::SPINNER));
static_assert(same(Picker::Click::NoClick, speaker_menu::Click::NONE) &&
              same(Picker::Click::Add, speaker_menu::Click::ADD) &&
              same(Picker::Click::Remove, speaker_menu::Click::REMOVE));
static_assert(same(Picker::Dot::Plays, speaker_menu::Dot::PLAYS) &&
              same(Picker::Dot::InUse, speaker_menu::Dot::IN_USE) &&
              same(Picker::Dot::NotInUse, speaker_menu::Dot::NOT_IN_USE));

}  // namespace

Picker::Picker(CastEngine& cast, Client& client, Outputs& outputs, QObject* parent)
    : QObject(parent), cast_(cast), client_(client), outputs_(outputs), pc_name_(device_name()) {
    const QSettings stored = settings();
    this->local_ticked_ = stored.value(SETTING_LOCAL, true).toBool();
    this->server_chosen_ = stored.value(SETTING_SERVER_CHOSEN, false).toBool();
    this->paired_server_ = stored.value(SETTING_PAIRED_SERVER).toString();

    const SpeakerModel* speakers = this->cast_.speakers();
    connect(speakers, &QAbstractItemModel::modelReset, this, &Picker::refresh_);
    connect(speakers, &QAbstractItemModel::dataChanged, this, &Picker::refresh_);
    connect(&this->cast_, &CastEngine::playingChanged, this, &Picker::refresh_);
    connect(&this->cast_, &CastEngine::discoveryAvailableChanged, this, &Picker::refresh_);
    connect(&this->client_, &Client::stateChanged, this, &Picker::refresh_);
    connect(&this->client_, &Client::serverNameChanged, this, &Picker::refresh_);
    connect(&this->client_, &Client::pairingCodeChanged, this, &Picker::refresh_);
    connect(&this->client_, &Client::paired, this, &Picker::refresh_);
    connect(&this->client_.player(), &Player::streamingChanged, this, &Picker::refresh_);
    connect(&this->client_.now_playing(), &NowPlaying::changed, this, &Picker::refresh_);
    connect(&this->outputs_, &Outputs::changed, this, &Picker::refresh_);
    // The source's stream and signal are only there to be asked
    this->timer_.setInterval(REFRESH_MS);
    connect(&this->timer_, &QTimer::timeout, this, [this] {
        const SourceFeed& source = this->client_.source();
        if (std::exchange(this->source_streaming_, source.streaming()) != source.streaming() ||
            std::exchange(this->source_signal_, source.signal()) != source.signal()) {
            this->refresh_();
        }
    });
    this->timer_.start();
    // What was chosen before this start is no change of route
    this->route_ = speaker_menu::route(this->state_());
    this->refresh_();
}

// ==============================================================================
// What shows
// ==============================================================================

speaker_menu::State Picker::state_() const {
    speaker_menu::State state;
    state.local = LOCAL;
    for (const SpeakerRow& row : this->cast_.speakers()->rows()) {
        if (row.in_group) {
            state.group.insert(row.id.toStdString());
        }
        if (row.can_play) {
            state.can_play.insert(row.id.toStdString());
        }
    }
    if (this->local_ticked_) {
        state.group.insert(LOCAL);
    }
    if (this->outputs_.local()) {
        state.can_play.insert(LOCAL);
    }
    return state;
}

speaker_menu::Row Picker::local_row_() const {
    const bool found = this->outputs_.local().has_value();
    speaker_menu::Output output;
    output.is_local = true;
    output.in_group = this->local_ticked_;
    // Beside speakers it plays once it plays along with them
    output.plays = this->local_ticked_ && found;
    output.is_discovered = found;
    return speaker_menu::row(output,
                             {.plays_groupable = this->route_ != Route::SERVER,
                              .group_count = static_cast<int>(this->state_().group.size())});
}

Picker::Server Picker::server_() const {
    const Client::State state = this->client_.state();
    Server server;
    server.connected = state == Client::State::Connected || state == Client::State::Pairing;
    server.paired = server.connected && this->client_.is_paired();
    server.code = this->client_.pairing_code();
    server.chosen = this->server_chosen_;
    server.streaming = this->client_.source().streaming();
    if (server.connected) {
        server.name = this->client_.server_name();
        if (server.name.isEmpty()) {
            server.name = tr("A server");
        }
    } else {
        server.name = this->paired_server_;
    }
    return server;
}

Picker::Dot Picker::dot() const {
    // An app routed to the Sendspin output by hand makes it an output in use too, so the dot
    // never contradicts the tray icon
    const bool plays = this->sendspin_plays();
    return static_cast<Dot>(speaker_menu::dot(this->outputs_.own_is_default() || plays, plays));
}

QString Picker::dot_text(Dot dot) {
    switch (dot) {
        case Dot::Plays:
            return tr("Sendspin is playing");
        case Dot::InUse:
            return tr("Sendspin is the output, and nothing is playing");
        case Dot::NotInUse:
            return tr("This PC plays directly");
    }
    return {};
}

QString Picker::notice() const {
    if (!this->outputs_.connected()) {
        return tr("PipeWire can't be reached. Sendspin needs it for sound.");
    }
    if (!this->cast_.discovery_available()) {
        return tr("Speakers can't be found: avahi-daemon isn't running.");
    }
    return {};
}

bool Picker::volume_shown() const {
    return std::ranges::any_of(this->cast_.speakers()->rows(), &SpeakerRow::in_group);
}

bool Picker::looking() const {
    return this->cast_.speakers()->rows().empty() && !this->server_shown();
}

QString Picker::local_id() const {
    return QLatin1String(LOCAL);
}

QString Picker::local_name() const {
    const std::optional<OutputSink> local = this->outputs_.local();
    return local ? local->description : tr("This PC");
}

QString Picker::server_status_text() const {
    const Server server = this->server_();
    return SpeakerModel::status_text(
        speaker_menu::row(speaker_menu::server(server.chosen, server.connected), {}).status);
}

bool Picker::server_highlighted() const {
    const Server server = this->server_();
    speaker_menu::Output output = speaker_menu::server(server.chosen, server.connected);
    // Its icon says the stream is open, which is more than connected
    output.plays = output.plays && server.streaming;
    return speaker_menu::row(output, {}).highlighted;
}

bool Picker::server_choosable() const {
    const Server server = this->server_();
    return server.paired && !server.chosen;
}

QString Picker::grouped_code(const QString& code) {
    return code.size() == 6 ? code.first(3) + QLatin1Char('-') + code.last(3) : code;
}

QString Picker::server_caption(const Server& server, const QString& pc_name) {
    if (!server.connected) {
        return tr("It reconnects by itself when it's back on the network.");
    }
    if (!server.code.isEmpty()) {
        return tr("Enter %1 in %2.").arg(grouped_code(server.code), server.name);
    }
    if (!server.paired) {
        return tr("Pair this PC in %1, then enter the code that appears here.").arg(server.name);
    }
    if (!server.chosen) {
        return {};
    }
    return server.streaming ? tr("Choose speakers, groups and volume in %1.").arg(server.name)
                            : tr("Select %1 as an input source in %2 to start playing.")
                                  .arg(pc_name, server.name);
}

bool Picker::sendspin_plays() const {
    const SourceFeed& source = this->client_.source();
    return this->cast_.playing() ||
           speaker_menu::sendspin_plays(source.signal(), this->route_, false, source.streaming());
}

QString Picker::status_line() const {
    QStringList playing_on;
    if (this->cast_.playing()) {
        for (const SpeakerRow& row : this->cast_.speakers()->rows()) {
            if (row.plays) {
                playing_on << row.name;
            }
        }
    }
    const bool sending = this->route_ == Route::SERVER && this->sendspin_plays();
    return status_line(
        playing_on, sending ? this->server_().name : QString(),
        this->client_.player().streaming() ? this->client_.now_playing().track() : Track{});
}

QString Picker::status_line(const QStringList& playing_on, const QString& sending_to,
                            const Track& here) {
    if (!playing_on.isEmpty()) {
        return playing_on.size() == 1 ? tr("Playing on %1").arg(playing_on.first())
                                      : tr("Playing on %1 and %n more", nullptr,
                                           static_cast<int>(playing_on.size()) - 1)
                                            .arg(playing_on.first());
    }
    if (!sending_to.isEmpty()) {
        return tr("Sending to %1").arg(sending_to);
    }
    if (!here.title.isEmpty()) {
        return here.artist.isEmpty() ? here.title : tr("%1 — %2").arg(here.title, here.artist);
    }
    return tr("Not in use");
}

// ==============================================================================
// Choices
// ==============================================================================

void Picker::toggle(const QString& id) {
    const speaker_menu::State state = this->state_();
    const bool in_group = state.group.contains(id.toStdString());
    const std::optional<speaker_menu::Plan> plan = speaker_menu::plan(
        {in_group ? Action::Kind::REMOVE : Action::Kind::ADD, id.toStdString()}, state);
    if (plan) {
        this->apply_(*plan);
    }
}

void Picker::playOnly(const QString& id) {
    const auto& rows = this->cast_.speakers()->rows();
    const auto row = std::ranges::find(rows, id, &SpeakerRow::id);
    const speaker_menu::NameClick click =
        row != rows.end() ? speaker_menu::name_click(row->row) : speaker_menu::NameClick::PLAY_ONLY;
    if (click == speaker_menu::NameClick::ACCEPT_IDENTITY) {
        this->cast_.add(id);
        return;
    }
    const std::optional<speaker_menu::Plan> plan =
        speaker_menu::plan({Action::Kind::PLAY_ONLY, id.toStdString()}, this->state_());
    if (plan) {
        this->apply_(*plan);
    }
    if (click == speaker_menu::NameClick::PLAY_ONLY_AND_DIAL) {
        this->cast_.add(id);
    }
}

void Picker::chooseServer() {
    if (!this->server_choosable()) {
        return;
    }
    const std::optional<speaker_menu::Plan> plan =
        speaker_menu::plan({Action::Kind::CHOOSE_SERVER, {}}, this->state_());
    if (plan) {
        this->apply_(*plan);
    }
}

void Picker::apply_(const speaker_menu::Plan& plan) {
    this->applying_ = true;
    // By value: the engine republishes its rows with each call
    const std::vector<SpeakerRow> rows = this->cast_.speakers()->rows();
    for (const SpeakerRow& row : rows) {
        const bool wanted = plan.group.contains(row.id.toStdString());
        if (wanted && !row.in_group) {
            this->cast_.add(row.id);
        } else if (!wanted && row.in_group) {
            this->cast_.remove(row.id);
        }
    }
    this->local_ticked_ = plan.group.contains(LOCAL);
    this->server_chosen_ = plan.route == Route::SERVER;
    this->save_();
    this->applying_ = false;
    this->refresh_();
}

void Picker::refresh_() {
    if (this->applying_) {
        return;
    }
    const speaker_menu::State state = this->state_();
    if (!state.group.empty() && this->server_chosen_) {
        // A speaker joined by its own button: the server is no longer chosen
        this->server_chosen_ = false;
        this->save_();
    } else if (state.group.empty() && !this->server_chosen_) {
        // The last speaker left by its own button, which is a choice like any made here: what
        // follows it is the rules' to say
        if (const auto plan = speaker_menu::plan({Action::Kind::PLAY_ONLY, state.local}, state)) {
            this->apply_(*plan);
            return;
        }
    }
    const bool was_sendspin = this->route_ != Route::DIRECT;
    this->route_ = speaker_menu::route(this->state_());
    // The system's output follows the choice, made here or with a speaker's own button, and only
    // when the sound moves to Sendspin or back: a default set by hand since is left alone
    // (docs/SPEC.md, "Sendspin or direct follows what's ticked")
    const bool is_sendspin = this->route_ != Route::DIRECT;
    if (is_sendspin != was_sendspin && !this->outputs_.set_own_default(is_sendspin) &&
        this->outputs_.connected()) {
        qCWarning(lc_picker, "the system's default output could not be changed");
    }
    // The server gets the PC's sound only while it is chosen
    this->client_.source().set_silenced(this->route_ != Route::SERVER);
    // Its own republished rows must not come back in here
    this->applying_ = true;
    this->cast_.set_plays_groupable(this->route_ != Route::SERVER);
    this->applying_ = false;

    const Server server = this->server_();
    if (server.paired && server.name != this->paired_server_) {
        this->paired_server_ = server.name;
        settings().setValue(SETTING_PAIRED_SERVER, server.name);
    }
    emit this->changed();
}

void Picker::save_() {
    QSettings stored = settings();
    stored.setValue(SETTING_LOCAL, this->local_ticked_);
    stored.setValue(SETTING_SERVER_CHOSEN, this->server_chosen_);
}
