#include "cast/cast_engine.h"

#include "device_name.h"

#include <QLoggingCategory>
#include <QNetworkInterface>

#include <algorithm>
#include <cstdlib>
#include <ctime>
#include <utility>

Q_LOGGING_CATEGORY(lc_cast, "sendspin.cast")

using sendspin::server::ApprovalScope;
using sendspin::server::ClientDisconnect;
using sendspin::server::ClientInfo;
using sendspin::server::CodeFormat;
using sendspin::server::ErrorKind;
using sendspin::server::GroupId;
using sendspin::server::GroupInfo;
using sendspin::server::PairingOptions;
using sendspin::server::PairingResult;
using sendspin::server::PairMethod;
using sendspin::server::SendspinServer;
using sendspin::server::ServerError;
using sendspin::server::StartResult;
using speaker_menu::Connection;
using speaker_menu::Step;

namespace {

constexpr int REFRESH_MS = 250;
/// How long a step may take to show before it is tried again
constexpr int64_t STEP_NS = 2'000'000'000;
/// A latency change smaller than this isn't worth telling the graph
constexpr int64_t LATENCY_STEP_NS = 1'000'000;
const char* const PLAYER_ROLE = "player@v1";

/// The clock of CapturedBlock::capture_time_ns
int64_t raw_now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (static_cast<int64_t>(ts.tv_sec) * 1'000'000'000) + ts.tv_nsec;
}

QString settings_path(const std::filesystem::path& data_dir) {
    return QString::fromStdString((data_dir / "cast.ini").string());
}

bool contains(const std::vector<std::string>& ids, const std::string& id) {
    return std::ranges::find(ids, id) != ids.end();
}

}  // namespace

/// Runs on the library's dispatch thread: copies each event and queues it to the engine's.
class CastEngine::Listener final : public sendspin::server::ServerListener {
public:
    explicit Listener(CastEngine* engine) : engine_(engine) {}

    void on_client_seen(const ClientInfo& /*client*/) override {
        this->post([](CastEngine& engine) {
            // Browsing works again; the library only logs that
            engine.set_discovery_available_(true);
            engine.refresh_();
        });
    }
    void on_client_connected(const ClientInfo& client) override {
        this->post([client](CastEngine& engine) { engine.on_connected_(client); });
    }
    void on_client_disconnected(const ClientInfo& client,
                                const ClientDisconnect& disconnect) override {
        this->post([client, disconnect](CastEngine& engine) {
            engine.on_disconnected_(client, disconnect);
        });
    }
    void on_approval_needed(const ClientInfo& /*client*/) override {
        this->post([](CastEngine& engine) { engine.refresh_(); });
    }
    void on_pairing_required(const ClientInfo& /*client*/) override {
        this->post([](CastEngine& engine) { engine.refresh_(); });
    }
    void on_credential_mismatch(const ClientInfo& /*client*/) override {
        this->post([](CastEngine& engine) { engine.refresh_(); });
    }
    void on_pairing_pending(const ClientInfo& client, std::string_view message) override {
        this->post(
            [id = QString::fromStdString(client.client_id),
             text = QString::fromUtf8(message.data(), static_cast<qsizetype>(message.size()))](
                CastEngine& engine) { emit engine.pairingPending(id, text); });
    }
    void on_pairing_code_needed(const ClientInfo& client, PairMethod method,
                                CodeFormat format) override {
        this->post([id = QString::fromStdString(client.client_id), method, format](
                       CastEngine& engine) { emit engine.pairingCodeNeeded(id, method, format); });
    }
    void on_pairing_result(const ClientInfo& client, PairingResult result) override {
        this->post(
            [client, result](CastEngine& engine) { engine.on_pairing_result_(client, result); });
    }
    void on_error(const ServerError& error) override {
        this->post([error](CastEngine& engine) { engine.on_error_(error); });
    }
    void on_group_changed(const GroupInfo& group) override {
        this->post([group](CastEngine& engine) { engine.on_group_changed_(group); });
    }

private:
    template <typename Fn>
    void post(Fn fn) {
        CastEngine* engine = this->engine_;
        // Dropped if the engine's server is gone by the time it runs
        QMetaObject::invokeMethod(
            engine,
            [engine, fn = std::move(fn)] {
                if (engine->server_) {
                    fn(*engine);
                }
            },
            Qt::QueuedConnection);
    }

    CastEngine* engine_;
};

bool is_own_client(const sendspin::server::DiscoveredClient& found, uint16_t own_port,
                   const QList<QHostAddress>& own_addresses) {
    if (own_port == 0 || found.port != own_port) {
        return false;
    }
    return std::ranges::any_of(found.addresses, [&own_addresses](const std::string& text) {
        // A link-local address comes with its zone, which the comparison would trip over
        QHostAddress address(QString::fromStdString(text));
        address.setScopeId({});
        return std::ranges::any_of(own_addresses, [&address](QHostAddress own) {
            own.setScopeId({});
            return own == address;
        });
    });
}

/// The library's own discovery, minus this app's client. Called on the discovery thread.
class CastEngine::Discovery final : public sendspin::server::DiscoveryProvider,
                                    private sendspin::server::DiscoverySink {
public:
    Discovery(std::shared_ptr<sendspin::server::DiscoveryProvider> platform, uint16_t own_port)
        : platform_(std::move(platform)), own_port_(own_port) {}

    bool start(const sendspin::server::DiscoveryRequest& request,
               sendspin::server::DiscoverySink& sink) override {
        this->sink_ = &sink;
        return this->platform_->start(request, *this);
    }
    void stop() override {
        this->platform_->stop();
    }

private:
    void on_client_found(const sendspin::server::DiscoveredClient& client) override {
        if (!is_own_client(client, this->own_port_, QNetworkInterface::allAddresses())) {
            this->sink_->on_client_found(client);
        }
    }
    void on_client_lost(const std::string& instance) override {
        this->sink_->on_client_lost(instance);
    }
    void on_unavailable() override {
        this->sink_->on_unavailable();
    }
    void on_available() override {
        this->sink_->on_available();
    }

    std::shared_ptr<sendspin::server::DiscoveryProvider> platform_;
    uint16_t own_port_;
    sendspin::server::DiscoverySink* sink_{nullptr};
};

QString CastEngine::Speaker::key() const {
    return QString::fromStdString(this->client_id.empty() ? this->url : this->client_id);
}

CastEngine::CastEngine(const std::filesystem::path& data_dir, SendspinOutput& output,
                       QObject* parent)
    : QObject(parent),
      output_(output),
      persistence_(data_dir),
      settings_(settings_path(data_dir), QSettings::IniFormat),
      listener_(std::make_unique<Listener>(this)) {
    this->load_settings_();
    this->publish_rows_();
    this->timer_.setInterval(REFRESH_MS);
    connect(&this->timer_, &QTimer::timeout, this, &CastEngine::refresh_);
}

CastEngine::~CastEngine() {
    this->stop();
}

bool CastEngine::start(uint16_t port, bool discovery) {
    this->stop();

    sendspin::server::ServerConfig config;
    config.name = device_name().toStdString();
    config.listen_port = port;
    config.advertise = discovery;
    config.browse = discovery;
    if (std::shared_ptr<sendspin::server::DiscoveryProvider> platform =
            sendspin::server::platform_discovery();
        discovery && platform) {
        config.discovery = std::make_shared<Discovery>(std::move(platform), this->own_client_port_);
    }
    // Follows the category, so QT_LOGGING_RULES turns the library's log up and down too
    config.log_level = lc_cast().isInfoEnabled() ? sendspin::server::LogLevel::INFO
                                                 : sendspin::server::LogLevel::WARN;
    // The library never puts a key, a PSK or a client_id in a line
    config.log = [](sendspin::server::LogLevel level, std::string_view line) {
        const QByteArray text(line.data(), static_cast<qsizetype>(line.size()));
        if (level == sendspin::server::LogLevel::INFO) {
            qCInfo(lc_cast, "%s", text.constData());
        } else {
            qCWarning(lc_cast, "%s", text.constData());
        }
    };

    auto server = std::make_unique<SendspinServer>();
    StartResult result = server->start(config, &this->persistence_, this->listener_.get());
    if (result == StartResult::BIND_FAILED && port != 0) {
        qCWarning(lc_cast, "port %u is taken; letting the system pick one", port);
        config.listen_port = 0;
        result = server->start(config, &this->persistence_, this->listener_.get());
    }
    if (result != StartResult::OK) {
        qCWarning(lc_cast, "the server did not start: %d", static_cast<int>(result));
        return false;
    }
    this->server_ = std::move(server);
    qCInfo(lc_cast, "serving on port %u", this->server_->listen_port());

    // Speakers mDNS can't find
    const QStringList urls = this->settings_.value("speaker_urls").toStringList();
    for (const QString& url : urls) {
        if (!this->server_->connect_url(url.toStdString(), {.retry_until_connected = true})) {
            qCWarning(lc_cast, "not a speaker URL: %s", qUtf8Printable(url));
        }
    }

    this->timer_.start();
    this->refresh_();
    return true;
}

void CastEngine::stop() {
    this->timer_.stop();
    this->release_cast_();
    // Joins the dispatch thread, so no callback runs past here
    this->server_.reset();
    for (Speaker& speaker : this->speakers_) {
        speaker.listed = false;
        speaker.facts.connected = false;
        speaker.group.reset();
    }
}

uint16_t CastEngine::port() const {
    return this->server_ ? this->server_->listen_port() : 0;
}

// ==============================================================================
// The user's choices
// ==============================================================================

void CastEngine::add(const QString& id) {
    Speaker* speaker = this->find_(id);
    if (speaker == nullptr || !this->server_) {
        return;
    }
    if (!speaker->presents_as.isEmpty()) {
        // The new identity takes the place of the one it presented itself as
        if (Speaker* old = this->find_(speaker->presents_as)) {
            old->facts.ticked = false;
        }
        speaker->presents_as.clear();
    }
    this->set_ticked_(*speaker, true);
    speaker->left_as.reset();
    speaker->busy_until_ns = 0;
    if (!speaker->facts.connected) {
        // Dials for playback, which may take it from another server: the user asked for it
        const bool dialing = speaker->client_id.empty()
                                 ? this->server_->connect_url(speaker->url)
                                 : this->server_->connect(speaker->client_id);
        if (!dialing) {
            qCInfo(lc_cast, "%s can't be dialed now", qUtf8Printable(speaker->name));
        }
    } else {
        const Connection connection = speaker_menu::connection(speaker->facts);
        if (connection == Connection::NEEDS_PAIRING ||
            connection == Connection::NEEDS_PAIRING_AGAIN) {
            this->pair(id);
        }
    }
    this->refresh_();
}

void CastEngine::remove(const QString& id) {
    Speaker* speaker = this->find_(id);
    if (speaker == nullptr || !this->server_) {
        return;
    }
    if (!speaker->presents_as.isEmpty()) {
        // The row stood for the speaker it presents itself as: that one leaves the group
        speaker = this->find_(speaker->presents_as);
        if (speaker == nullptr) {
            return;
        }
    }
    this->set_ticked_(*speaker, false);
    speaker->busy_until_ns = 0;
    const std::optional<GroupId> lobby = this->server_->default_group();
    if (!speaker->facts.connected && !speaker->client_id.empty() && this->cast_exists_ && lobby) {
        // Gives up the place the library keeps for it in the cast group while it is away
        (void)this->server_->move_client(speaker->client_id, *lobby);
    }
    this->refresh_();
}

void CastEngine::setGroupVolume(int volume) {
    volume = std::clamp(volume, 0, static_cast<int>(sendspin::server::MAX_VOLUME));
    if (this->volume_ == volume) {
        return;
    }
    this->volume_ = volume;
    this->settings_.setValue("group_volume", volume);
    if (this->server_ && this->cast_exists_) {
        (void)this->server_->set_group_volume(*this->cast_group_, static_cast<uint32_t>(volume));
    }
    emit this->groupVolumeChanged(volume);
}

void CastEngine::setGroupMuted(bool muted) {
    if (this->muted_ == muted) {
        return;
    }
    this->muted_ = muted;
    this->settings_.setValue("group_muted", muted);
    if (this->server_ && this->cast_exists_) {
        (void)this->server_->set_group_mute(*this->cast_group_, muted);
    }
    emit this->groupMutedChanged(muted);
}

bool CastEngine::pair(const QString& id) {
    const Speaker* speaker = this->find_(id);
    if (speaker == nullptr || !this->server_ || speaker->client_id.empty()) {
        return false;
    }
    PairingOptions options;
    if (speaker->offers.dynamic_digits) {
        options = {PairMethod::DYNAMIC_PAIRING_CODE, CodeFormat::DIGITS};
    } else if (speaker->offers.static_pairing_code) {
        options = {PairMethod::STATIC_PAIRING_CODE, {}};
    } else if (speaker->offers.pairing_psk) {
        options = {PairMethod::PAIRING_PSK, {}};
    } else {
        return false;
    }
    return this->server_->begin_pairing(speaker->client_id, options);
}

bool CastEngine::submitPairingCode(const QString& id, const QString& code) {
    return this->server_ &&
           this->server_->submit_pairing_code(id.toStdString(), code.toStdString());
}

bool CastEngine::submitPairingToken(const QString& id, const QString& token) {
    return this->server_ &&
           this->server_->submit_pairing_token(id.toStdString(), token.trimmed().toStdString());
}

void CastEngine::cancelPairing(const QString& id) {
    if (this->server_) {
        this->server_->cancel_pairing(id.toStdString());
    }
}

void CastEngine::forget(const QString& id) {
    if (!this->server_) {
        return;
    }
    (void)this->server_->unpair(id.toStdString());
    this->remove(id);
}

// ==============================================================================
// The library's events
// ==============================================================================

void CastEngine::on_connected_(const ClientInfo& info) {
    this->sync_clients_();
    if (Speaker* speaker = this->find_client_(info.client_id)) {
        speaker->left_as.reset();
    }
    this->refresh_();
}

void CastEngine::on_disconnected_(const ClientInfo& info, const ClientDisconnect& disconnect) {
    if (Speaker* speaker = this->find_client_(info.client_id)) {
        speaker->left_as = speaker_menu::after_disconnect(disconnect);
        speaker->group.reset();
        speaker->busy_until_ns = 0;
        speaker->cant_play = false;
        // A speaker reached by its URL can't announce that it's back, and the library stops
        // dialing one that said goodbye. Gone for a while is not a wish to be left alone.
        if (speaker->left_as == Connection::UNAVAILABLE &&
            this->settings_.value("speaker_urls")
                .toStringList()
                .contains(QString::fromStdString(speaker->url))) {
            (void)this->server_->connect_url(speaker->url, {.retry_until_connected = true});
        }
    }
    this->refresh_();
}

void CastEngine::on_error_(const ServerError& error) {
    if (error.kind == ErrorKind::DISCOVERY_UNAVAILABLE) {
        this->set_discovery_available_(false);
    }
    const std::optional<Connection> connection = speaker_menu::after_error(error);
    if (connection) {
        this->sync_clients_();
        const auto it = std::ranges::find_if(this->speakers_, [&error](const Speaker& speaker) {
            return error.client_id.empty() ? !error.url.empty() && speaker.url == error.url
                                           : speaker.client_id == error.client_id;
        });
        if (it != this->speakers_.end()) {
            if (*connection == Connection::CANT_PLAY) {
                it->cant_play = true;
            } else {
                it->left_as = connection;
            }
        }
    }
    this->refresh_();
}

void CastEngine::on_group_changed_(const GroupInfo& group) {
    // A member the engine has not met yet is in clients() already
    this->sync_clients_();
    const int64_t now_ns = raw_now_ns();
    const bool is_cast = this->cast_group_ == group.id;
    for (Speaker& speaker : this->speakers_) {
        if (speaker.client_id.empty()) {
            continue;
        }
        const bool member = contains(group.client_ids, speaker.client_id);
        const bool was_member = speaker.group == group.id;
        if (member == was_member) {
            continue;
        }
        const bool ours = now_ns < speaker.busy_until_ns;
        speaker.busy_until_ns = 0;
        if (member) {
            speaker.group = group.id;
        } else {
            speaker.group.reset();
        }
        // The speaker's own `switch` button took it into or out of the cast group: the person
        // at the speaker chose. A speaker that went away was taken out of its group before.
        if (is_cast && !ours && speaker.facts.ticked != member) {
            qCInfo(lc_cast, "%s %s the group by itself", qUtf8Printable(speaker.name),
                   member ? "joined" : "left");
            this->set_ticked_(speaker, member);
        }
    }
    this->refresh_();
}

void CastEngine::on_pairing_result_(const ClientInfo& info, PairingResult result) {
    if (result == PairingResult::PAIRED) {
        if (Speaker* speaker = this->find_client_(info.client_id)) {
            // It is activated again as a paired client; its move waits for that
            speaker->busy_until_ns = raw_now_ns() + STEP_NS;
        }
    }
    emit this->pairingFinished(QString::fromStdString(info.client_id), result);
    this->refresh_();
}

// ==============================================================================
// Keeping the server as the choices say
// ==============================================================================

void CastEngine::refresh_() {
    if (!this->server_) {
        this->publish_rows_();
        return;
    }
    const int64_t now_ns = raw_now_ns();
    this->sync_clients_();

    const std::vector<GroupInfo> groups = this->server_->groups();
    const bool exists =
        this->cast_group_ && std::ranges::any_of(groups, [this](const GroupInfo& group) {
            return group.id == *this->cast_group_;
        });
    if (this->cast_group_ && !exists && (this->cast_exists_ || now_ns >= this->cast_deadline_ns_)) {
        // The server dropped it, or never made it
        this->release_cast_();
    }
    this->cast_exists_ = this->cast_group_ && exists;

    this->detect_identity_changes_();
    this->take_steps_(now_ns);
    this->update_input_(now_ns);
    this->publish_rows_();
}

void CastEngine::sync_clients_() {
    for (Speaker& speaker : this->speakers_) {
        speaker.listed = false;
        speaker.facts.connected = false;
    }
    for (const ClientInfo& info : this->server_->clients()) {
        Speaker& speaker = this->speaker_for_(info);
        speaker.listed = true;
        if (!info.name.empty()) {
            speaker.name = QString::fromStdString(info.name);
        }
        if (!info.url.empty()) {
            speaker.url = info.url;
        }
        if (info.client_id.empty()) {
            continue;
        }
        speaker.facts.connected = info.connected;
        speaker.facts.paired = info.paired;
        speaker.facts.approved = info.approval.has_value();
        speaker.facts.unpaired_access = info.unpaired_access;
        speaker.facts.credential_mismatch = info.credential_mismatch;
        speaker.offers = info.pairing_offers;
        speaker.can_pair = info.pairing_offers != sendspin::server::PairingOffers{};
        speaker.has_player = contains(info.roles, PLAYER_ROLE);
    }
    for (Speaker& speaker : this->speakers_) {
        if (!speaker.facts.connected) {
            speaker.group.reset();
            speaker.cant_play = false;
        }
        speaker.facts.in_cast = this->cast_group_ && speaker.group == this->cast_group_;
    }
}

void CastEngine::detect_identity_changes_() {
    for (Speaker& speaker : this->speakers_) {
        speaker.presents_as.clear();
        const speaker_menu::Facts& facts = speaker.facts;
        if (!facts.connected || facts.ticked || facts.paired || facts.approved) {
            continue;
        }
        // A client_id the app hasn't seen, under an added speaker's name or URL
        const auto it = std::ranges::find_if(this->speakers_, [&speaker](const Speaker& added) {
            return added.facts.ticked && !added.facts.connected &&
                   ((!added.url.empty() && added.url == speaker.url) || added.name == speaker.name);
        });
        if (it != this->speakers_.end()) {
            speaker.presents_as = it->key();
        }
    }
}

void CastEngine::take_steps_(int64_t now_ns) {
    const std::optional<GroupId> lobby = this->server_->default_group();
    for (Speaker& speaker : this->speakers_) {
        if (speaker.client_id.empty() || now_ns < speaker.busy_until_ns) {
            continue;
        }
        bool taken = true;
        switch (speaker_menu::next_step(speaker.facts)) {
            case Step::APPROVE:
                // Never the source role
                taken = this->server_->approve(speaker.client_id, ApprovalScope::PLAYBACK);
                break;
            case Step::MOVE_TO_CAST:
                if (!this->cast_group_) {
                    this->cast_group_ =
                        this->server_->create_group(device_name().toStdString(), speaker.client_id);
                    this->cast_deadline_ns_ = now_ns + STEP_NS;
                    taken = this->cast_group_.has_value();
                } else if (this->cast_exists_) {
                    taken = this->server_->move_client(speaker.client_id, *this->cast_group_);
                } else {
                    // The group is on its way; the others follow its first member
                    taken = false;
                }
                break;
            case Step::MOVE_TO_LOBBY:
                taken = lobby && this->server_->move_client(speaker.client_id, *lobby);
                break;
            case Step::REVOKE:
                taken = this->server_->revoke_approval(speaker.client_id);
                break;
            case Step::NONE:
                taken = false;
                break;
        }
        if (taken) {
            speaker.busy_until_ns = now_ns + STEP_NS;
        }
    }
}

void CastEngine::update_input_(int64_t now_ns) {
    const bool any_added = std::ranges::any_of(
        this->speakers_, [](const Speaker& speaker) { return speaker.facts.ticked; });
    if (!any_added) {
        // The group goes with its last member and its input
        this->release_cast_();
    } else if (this->feed_.poll(now_ns) == CastFeed::State::CLOSED) {
        this->feed_.detach();
    }
    if (this->cast_exists_ && !this->feed_.input()) {
        sendspin::server::InputOptions options;
        options.format = {SendspinOutput::RATE, static_cast<uint8_t>(SendspinOutput::CHANNELS),
                          sendspin::server::SampleFormat::F32};
        options.kind = sendspin::server::InputKind::LIVE;
        options.write_jitter_us = WRITE_JITTER_US;
        sendspin::server::OpenedInput opened =
            this->server_->open_input(*this->cast_group_, options);
        if (opened.result == sendspin::server::OpenInputResult::OK) {
            this->feed_.attach(std::move(opened.input));
            if (this->volume_) {
                (void)this->server_->set_group_volume(*this->cast_group_,
                                                      static_cast<uint32_t>(*this->volume_));
            }
            if (this->muted_) {
                (void)this->server_->set_group_mute(*this->cast_group_, true);
            }
        } else {
            qCWarning(lc_cast, "the group's input did not open: %d",
                      static_cast<int>(opened.result));
        }
    }

    // The speaker buffer is the library's; the output reports it as its latency
    const int64_t latency_ns =
        this->feed_.input() ? std::max<int64_t>(this->feed_.input()->latency_us(), 0) * 1000 : 0;
    if (std::abs(latency_ns - this->latency_ns_) >= LATENCY_STEP_NS) {
        this->latency_ns_ = latency_ns;
        this->output_.set_latency(latency_ns);
    }

    const bool speaker_plays = std::ranges::any_of(this->speakers_, [](const Speaker& speaker) {
        return speaker.facts.in_cast && speaker.has_player;
    });
    const bool playing = speaker_menu::sendspin_plays(
        this->feed_.has_sound(now_ns), speaker_menu::Route::SPEAKERS, speaker_plays, false);
    if (playing != this->playing_) {
        this->playing_ = playing;
        emit this->playingChanged(playing);
    }
}

void CastEngine::release_cast_() {
    this->feed_.detach();
    this->cast_group_.reset();
    this->cast_exists_ = false;
    for (Speaker& speaker : this->speakers_) {
        speaker.facts.in_cast = false;
    }
}

void CastEngine::publish_rows_() {
    // What you didn't choose leaves once it's gone
    std::erase_if(this->speakers_, [](const Speaker& speaker) {
        return !speaker_menu::is_listed(speaker.facts.ticked, speaker.listed);
    });

    const int added = static_cast<int>(std::ranges::count_if(
        this->speakers_, [](const Speaker& speaker) { return speaker.facts.ticked; }));
    std::vector<SpeakerRow> rows;
    for (const Speaker& speaker : this->speakers_) {
        // One row for a speaker and the unknown device that presents itself as it
        const bool replaced = std::ranges::any_of(
            this->speakers_,
            [&speaker](const Speaker& other) { return other.presents_as == speaker.key(); });
        if (replaced) {
            continue;
        }
        speaker_menu::Output output;
        output.in_group = speaker.facts.ticked || !speaker.presents_as.isEmpty();
        output.plays = speaker.facts.connected && speaker.facts.in_cast && speaker.has_player &&
                       !speaker.cant_play;
        output.is_discovered = speaker.listed;
        output.connection = this->connection_of_(speaker);

        SpeakerRow row;
        row.id = speaker.key();
        row.name = speaker.name;
        row.in_group = output.in_group;
        row.plays = output.plays;
        row.paired = speaker.facts.paired;
        row.can_pair = speaker.can_pair;
        row.row = speaker_menu::row(output, {.plays_groupable = true, .group_count = added});
        rows.push_back(std::move(row));
    }
    std::ranges::sort(rows, [](const SpeakerRow& a, const SpeakerRow& b) {
        const int by_name = QString::localeAwareCompare(a.name, b.name);
        return by_name != 0 ? by_name < 0 : a.id < b.id;
    });
    this->model_.set_rows(std::move(rows));
}

Connection CastEngine::connection_of_(const Speaker& speaker) const {
    if (!speaker.presents_as.isEmpty()) {
        return Connection::IDENTITY_CHANGED;
    }
    if (!speaker.facts.connected) {
        // The library dials a speaker it found; until that fails there is nothing to say
        return speaker.left_as.value_or(speaker.facts.ticked ? Connection::CONNECTING
                                                             : Connection::WAITING);
    }
    return speaker.cant_play ? Connection::CANT_PLAY : speaker_menu::connection(speaker.facts);
}

CastEngine::Speaker& CastEngine::speaker_for_(const ClientInfo& info) {
    const auto by_id = std::ranges::find_if(this->speakers_, [&info](const Speaker& speaker) {
        return !info.client_id.empty() && speaker.client_id == info.client_id;
    });
    if (by_id != this->speakers_.end()) {
        return *by_id;
    }
    // Found by browsing and not connected yet, it is known by its URL alone: the row it had
    // before it connected, or an added speaker's
    const auto by_url = std::ranges::find_if(this->speakers_, [&info](const Speaker& speaker) {
        return !info.url.empty() && speaker.url == info.url &&
               (speaker.client_id.empty() || info.client_id.empty());
    });
    if (by_url != this->speakers_.end()) {
        if (by_url->client_id.empty() && !info.client_id.empty()) {
            by_url->client_id = info.client_id;
            if (by_url->facts.ticked) {
                this->save_ticked_();
            }
        }
        return *by_url;
    }
    Speaker speaker;
    speaker.client_id = info.client_id;
    speaker.url = info.url;
    speaker.name = QString::fromStdString(info.name);
    return this->speakers_.emplace_back(std::move(speaker));
}

CastEngine::Speaker* CastEngine::find_(const QString& key) {
    const auto it = std::ranges::find_if(
        this->speakers_, [&key](const Speaker& speaker) { return speaker.key() == key; });
    return it == this->speakers_.end() ? nullptr : &*it;
}

CastEngine::Speaker* CastEngine::find_client_(const std::string& client_id) {
    const auto it = std::ranges::find_if(this->speakers_, [&client_id](const Speaker& speaker) {
        return !client_id.empty() && speaker.client_id == client_id;
    });
    return it == this->speakers_.end() ? nullptr : &*it;
}

void CastEngine::set_ticked_(Speaker& speaker, bool ticked) {
    if (speaker.facts.ticked != ticked) {
        speaker.facts.ticked = ticked;
        this->save_ticked_();
    }
}

void CastEngine::set_discovery_available_(bool available) {
    if (this->discovery_available_ != available) {
        this->discovery_available_ = available;
        emit this->discoveryAvailableChanged(available);
    }
}

// ==============================================================================
// Settings
// ==============================================================================

void CastEngine::load_settings_() {
    if (this->settings_.contains("group_volume")) {
        this->volume_ = std::clamp(this->settings_.value("group_volume").toInt(), 0,
                                   static_cast<int>(sendspin::server::MAX_VOLUME));
    }
    this->muted_ = this->settings_.value("group_muted", false).toBool();

    const int count = this->settings_.beginReadArray("added");
    for (int i = 0; i < count; i++) {
        this->settings_.setArrayIndex(i);
        Speaker speaker;
        speaker.client_id = this->settings_.value("client_id").toString().toStdString();
        speaker.url = this->settings_.value("url").toString().toStdString();
        // A speaker keeps the name from its hello while it's away
        speaker.name = this->settings_.value("name").toString();
        speaker.facts.ticked = true;
        if (!speaker.client_id.empty()) {
            this->speakers_.push_back(std::move(speaker));
        }
    }
    this->settings_.endArray();
}

void CastEngine::save_ticked_() {
    this->settings_.remove("added");
    this->settings_.beginWriteArray("added");
    int index = 0;
    for (const Speaker& speaker : this->speakers_) {
        // Kept by client_id; one added before it ever connected is kept once it has
        if (!speaker.facts.ticked || speaker.client_id.empty()) {
            continue;
        }
        this->settings_.setArrayIndex(index++);
        this->settings_.setValue("client_id", QString::fromStdString(speaker.client_id));
        this->settings_.setValue("url", QString::fromStdString(speaker.url));
        this->settings_.setValue("name", speaker.name);
    }
    this->settings_.endArray();
}
