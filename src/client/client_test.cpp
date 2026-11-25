#include "client.h"

#include "audio/volume_curve.h"
#include "persistence.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <sendspin/server/server.h>

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <vector>

namespace server = sendspin::server;

namespace {

/// What the server reported, for the test thread to poll. The server calls on its own thread.
class ServerEvents final : public server::ServerListener {
public:
    template <typename T>
    T get(T ServerEvents::* field) {
        const std::lock_guard lock(this->mutex_);
        return this->*field;
    }

    bool connected{false};
    bool paired_session{false};
    int code_requests{0};
    /// Source audio chunks received, and the furthest a chunk's timestamp was from its arrival
    int chunks{0};
    int64_t worst_chunk_age_us{0};

    /// ServerConfig::trace: called for every message, on the server's dispatch thread
    void trace(const server::TracedMessage& message) {
        // roles/source/v1.md "Source Audio Chunks (Binary)": the type, then the capture time of
        // the first sample in server time, big-endian microseconds
        constexpr uint8_t SOURCE_AUDIO = 12;
        if (message.direction != server::TraceDirection::INBOUND || !message.binary ||
            message.plaintext.size() <= 9 || message.plaintext[0] != SOURCE_AUDIO) {
            return;
        }
        const auto timestamp_us = qFromBigEndian<qint64>(message.plaintext.data() + 1);
        const std::lock_guard lock(this->mutex_);
        ++this->chunks;
        this->worst_chunk_age_us =
            std::max<int64_t>(this->worst_chunk_age_us, std::abs(message.stamp_us - timestamp_us));
    }
    std::vector<server::TransportCommand> commands;
    std::optional<server::PairingResult> pairing_result;
    std::optional<server::StreamFormat> source_format;
    std::optional<server::SourceSignal> source_signal;
    int errors{0};

private:
    void on_client_connected(const server::ClientInfo& client) override {
        const std::lock_guard lock(this->mutex_);
        this->connected = true;
        this->paired_session = client.paired;
    }
    void on_client_disconnected(const server::ClientInfo& /*client*/,
                                const server::ClientDisconnect& /*disconnect*/) override {
        const std::lock_guard lock(this->mutex_);
        this->connected = false;
        this->paired_session = false;
        this->source_format.reset();
    }
    void on_pairing_code_needed(const server::ClientInfo& /*client*/, server::PairMethod /*method*/,
                                server::CodeFormat /*format*/) override {
        const std::lock_guard lock(this->mutex_);
        ++this->code_requests;
    }
    void on_pairing_result(const server::ClientInfo& /*client*/,
                           server::PairingResult result) override {
        const std::lock_guard lock(this->mutex_);
        this->pairing_result = result;
    }
    void on_source_started(const server::ClientInfo& /*client*/, server::GroupId /*group*/,
                           const server::StreamFormat& format) override {
        const std::lock_guard lock(this->mutex_);
        this->source_format = format;
    }
    void on_source_stopped(const server::ClientInfo& /*client*/,
                           server::GroupId /*group*/) override {
        const std::lock_guard lock(this->mutex_);
        this->source_format.reset();
    }
    void on_transport_command(server::GroupId /*group*/, server::TransportCommand command,
                              const server::CommandArgs& /*args*/) override {
        const std::lock_guard lock(this->mutex_);
        this->commands.push_back(command);
    }
    void on_error(const server::ServerError& /*error*/) override {
        const std::lock_guard lock(this->mutex_);
        ++this->errors;
    }
    void on_source_signal(const server::ClientInfo& /*client*/,
                          server::SourceSignal signal) override {
        const std::lock_guard lock(this->mutex_);
        this->source_signal = signal;
    }

    std::mutex mutex_;
};

// Qt's own log lines, which QTest keeps off standard error
QByteArray qt_log;
QtMessageHandler previous_handler = nullptr;

void capture_message(QtMsgType type, const QMessageLogContext& context, const QString& message) {
    qt_log += message.toUtf8() + '\n';
    previous_handler(type, context, message);
}

/// 10 ms of a tone, or of silence, stamped now, as the Sendspin output delivers it
void feed(Client& client, bool sound) {
    static uint32_t position = 0;
    std::vector<float> samples(480 * SendspinOutput::CHANNELS, 0.0F);
    if (sound) {
        for (size_t frame = 0; frame < 480; ++frame, ++position) {
            const float value = 0.25F * std::sin(static_cast<float>(position) * 0.0576F);
            samples[frame * 2] = samples[frame * 2 + 1] = value;
        }
    }
    client.source().write({.samples = samples.data(),
                           .frames = 480,
                           .rate = SendspinOutput::RATE,
                           .capture_time_ns = sendspin::SendspinClient::now_us() * 1000,
                           .silent = !sound});
}

}  // namespace

/// The client against sendspin-server-cpp in this process, standing in for Music Assistant:
/// the server dials, pairs by the dynamic code and takes the source's stream.
class ClientTest : public QObject {
    Q_OBJECT

private:
    QTemporaryDir dir_;
    QFile stderr_capture_;
    int real_stderr_{-1};
    bool failed_{false};
    ServerEvents events_;
    std::optional<ServerPersistence> server_persistence_;
    server::SendspinServer server_;
    Client::Config config_;
    QString client_id_;
    QString token_;

    std::filesystem::path data_dir() const {
        return this->dir_.path().toStdString();
    }
    void dial() {
        const std::string url =
            "ws://127.0.0.1:" + std::to_string(this->config_.port) + "/sendspin";
        QVERIFY(this->server_.connect_url(url));
    }

private slots:
    void initTestCase() {
        // Everything either library and the app log, to check no secret is in it
        this->stderr_capture_.setFileName(this->dir_.filePath("stderr.log"));
        QVERIFY(this->stderr_capture_.open(QIODevice::ReadWrite));
        this->real_stderr_ = dup(STDERR_FILENO);
        dup2(this->stderr_capture_.handle(), STDERR_FILENO);
        sendspin::SendspinClient::set_log_level(sendspin::LogLevel::VERBOSE);
        previous_handler = qInstallMessageHandler(capture_message);

        this->config_ = {.name = "Test PC", .port = Client::free_port(0), .advertise = false};

        server::ServerConfig config;
        config.name = "Test Server";
        config.listen_port = 0;
        config.advertise = false;
        config.browse = false;
        config.trace = [this](const server::TracedMessage& message) {
            this->events_.trace(message);
        };
        this->server_persistence_.emplace(this->data_dir());
        QCOMPARE(this->server_.start(config, &*this->server_persistence_, &this->events_),
                 server::StartResult::OK);
    }

    void cleanup() {
        this->failed_ = this->failed_ || QTest::currentTestFailed();
    }

    void cleanupTestCase() {
        this->server_.stop();
        qInstallMessageHandler(previous_handler);
        if (this->real_stderr_ < 0) {
            return;
        }
        fflush(stderr);
        dup2(this->real_stderr_, STDERR_FILENO);
        ::close(this->real_stderr_);
        if (this->failed_) {
            // What the libraries said, which the capture kept from the test's output
            this->stderr_capture_.seek(0);
            fputs(this->stderr_capture_.readAll().constData(), stderr);
        }
    }

    void stoppedUntilStarted() {
        ClientPersistence persistence(this->data_dir());
        Client client(this->config_, &persistence);
        QCOMPARE(client.state(), Client::State::Stopped);
        QVERIFY(client.pairing_token().isEmpty());
        QVERIFY(client.client_id().isEmpty());
        QVERIFY(!QDir(this->dir_.filePath("client")).exists());
    }

    // The library gives a server 20 tries at the code, then holds pairing until someone at the
    // PC allows it
    void wrongCodesNeedApprovalToPairAgain() {
        ClientPersistence persistence(this->data_dir());
        Client client(this->config_, &persistence);
        QSignalSpy approvals(&client, &Client::pairingApprovalNeededChanged);
        QVERIFY(client.start());
        const std::string id = client.client_id().toStdString();
        this->dial();
        QTRY_COMPARE(client.state(), Client::State::Connected);

        QVERIFY(this->server_.begin_pairing(id, {}));
        QTRY_COMPARE(client.pairing_code().size(), 6);
        QString wrong = client.pairing_code();
        wrong[0] = wrong[0] == QLatin1Char('0') ? QLatin1Char('1') : QLatin1Char('0');
        int requests = 0;
        while (!client.pairing_code().isEmpty()) {
            QTRY_VERIFY(this->events_.get(&ServerEvents::code_requests) > requests ||
                        client.pairing_code().isEmpty());
            requests = this->events_.get(&ServerEvents::code_requests);
            QVERIFY(requests <= 25);
            (void)this->server_.submit_pairing_code(id, wrong.toStdString());
            QTest::qWait(20);
        }
        // The next attempt waits; the server may already have begun it
        if (!client.pairing_approval_needed()) {
            (void)this->server_.begin_pairing(id, {});
        }
        QTRY_VERIFY(client.pairing_approval_needed());
        QVERIFY(client.pairing_code().isEmpty());
        client.allow_pairing();
        QTRY_COMPARE(client.pairing_code().size(), 6);
        QTRY_VERIFY(!client.pairing_approval_needed());

        // A server that gives up takes the code and the state down with it
        QVERIFY(this->server_.cancel_pairing(id));
        QTRY_VERIFY(client.pairing_code().isEmpty());
        QTRY_COMPARE(client.state(), Client::State::Connected);
        QVERIFY(!client.is_paired());

        client.stop();
        QTRY_VERIFY(!this->events_.get(&ServerEvents::connected));
    }

    void pairsByTheDynamicCode() {
        ClientPersistence persistence(this->data_dir());
        Client client(this->config_, &persistence);
        QSignalSpy states(&client, &Client::stateChanged);
        QSignalSpy codes(&client, &Client::pairingCodeChanged);
        QSignalSpy paired(&client, &Client::paired);

        QVERIFY(client.start());
        QCOMPARE(client.state(), Client::State::Waiting);
        this->client_id_ = client.client_id();
        this->token_ = client.pairing_token();
        QCOMPARE(this->client_id_.size(), 43);
        QCOMPARE(this->token_.size(), qsizetype{sendspin::SendspinClient::PAIRING_TOKEN_LENGTH});
        QVERIFY(this->token_.startsWith("SP:0"));

        // The server finds the PC. Unpaired, it gets no roles: the source needs pairing.
        this->dial();
        QTRY_VERIFY(this->events_.get(&ServerEvents::connected));
        QTRY_COMPARE(client.state(), Client::State::Connected);
        QTRY_COMPARE(client.server_name(), "Test Server");
        QVERIFY(!client.is_paired());
        QVERIFY(!this->events_.get(&ServerEvents::paired_session));

        // The server starts pairing; the PC shows the code; the operator types it in
        const std::string id = this->client_id_.toStdString();
        const int requests = this->events_.get(&ServerEvents::code_requests);
        QVERIFY(this->server_.begin_pairing(id, {}));
        QTRY_COMPARE(client.pairing_code().size(), 6);
        QCOMPARE(client.state(), Client::State::Pairing);
        QCOMPARE(codes.size(), 1);
        QTRY_VERIFY(this->events_.get(&ServerEvents::code_requests) > requests);
        QVERIFY(this->server_.submit_pairing_code(id, client.pairing_code().toStdString()));

        QTRY_COMPARE(this->events_.get(&ServerEvents::pairing_result),
                     std::optional(server::PairingResult::PAIRED));
        QTRY_COMPARE(paired.size(), 1);
        QTRY_VERIFY(client.is_paired());
        QTRY_COMPARE(client.state(), Client::State::Connected);
        QVERIFY(client.pairing_code().isEmpty());
        QCOMPARE(codes.size(), 2);

        client.stop();
        QCOMPARE(client.state(), Client::State::Stopped);
        QTRY_VERIFY(!this->events_.get(&ServerEvents::connected));
    }

    // Runs after the pairing above, on the same stored identity and records
    void streamsAfterARestartWithoutPairingAgain() {
        QVERIFY(!this->client_id_.isEmpty());
        ClientPersistence persistence(this->data_dir());
        Client client(this->config_, &persistence);
        QSignalSpy codes(&client, &Client::pairingCodeChanged);

        QVERIFY(client.start());
        QCOMPARE(client.client_id(), this->client_id_);
        QCOMPARE(client.pairing_token(), this->token_);

        this->dial();
        QTRY_VERIFY(client.is_paired());
        QTRY_COMPARE(client.state(), Client::State::Connected);
        QTRY_VERIFY(this->events_.get(&ServerEvents::paired_session));
        QCOMPARE(codes.size(), 0);

        // Streaming is checked on this session and not right after pairing: there the server
        // starts the source without declaring `playback`, which roles/source/v1.md forbids and
        // the client rightly ignores (Sendspin/sendspin-server-cpp#107)
        const std::string id = this->client_id_.toStdString();
        // The server takes the PC as an input
        const auto group = this->server_.default_group();
        QVERIFY(group.has_value());
        const server::RoutedSource route = this->server_.route_source(id, *group);
        QCOMPARE(route.result, server::RouteSourceResult::OK);
        QTRY_VERIFY(client.source().streaming());
        QTRY_VERIFY(this->events_.get(&ServerEvents::source_format).has_value());
        QCOMPARE(*this->events_.get(&ServerEvents::source_format),
                 (server::StreamFormat{server::StreamCodec::FLAC, SendspinOutput::RATE, 2, 16}));

        // Sound on the output is a signal; silence, or sound sent elsewhere, is not
        QVERIFY(!client.source().signal());
        for (int i = 0; i < 50; ++i) {
            feed(client, true);
            QTest::qWait(10);
        }
        QVERIFY(client.source().signal());
        // The audio arrived, stamped when it was captured: half a second in 20 ms chunks, each
        // received within moments of its timestamp
        QTRY_VERIFY(this->events_.get(&ServerEvents::chunks) >= 20);
        QVERIFY(this->events_.get(&ServerEvents::worst_chunk_age_us) < 500'000);
        QTRY_COMPARE(this->events_.get(&ServerEvents::source_signal),
                     std::optional(server::SourceSignal::PRESENT));

        client.source().set_silenced(true);
        feed(client, true);
        QTRY_VERIFY(!client.source().signal());
        QTRY_COMPARE(this->events_.get(&ServerEvents::source_signal),
                     std::optional(server::SourceSignal::ABSENT));
        QVERIFY(client.source().streaming());

        // A stream the server could not decode would have been an error
        QCOMPARE(this->events_.get(&ServerEvents::errors), 0);

        client.stop();
        QVERIFY(!client.source().streaming());
    }

    void anUnpairedServerPlaysHereOnceApproved_data() {
        QTest::addColumn<bool>("unpaired_access");
        QTest::newRow("allowed") << true;
        QTest::newRow("pairing required") << false;
    }

    // A PC of its own identity, so the pairing above plays no part
    void anUnpairedServerPlaysHereOnceApproved() {
        QFETCH(const bool, unpaired_access);
        const QTemporaryDir dir;
        ClientPersistence persistence(dir.path().toStdString());
        Client::Config config = this->config_;
        config.unpaired_access = unpaired_access;
        Client client(config, &persistence);
        QVERIFY(client.start());
        this->dial();
        QTRY_COMPARE(client.state(), Client::State::Connected);
        QVERIFY(this->server_.approve(client.client_id().toStdString(),
                                      server::ApprovalScope::PLAYBACK));
        // On a new session: the server activates this one without `playback`
        // (Sendspin/sendspin-server-cpp#107)
        client.stop();
        QTRY_VERIFY(!this->events_.get(&ServerEvents::connected));
        QVERIFY(client.start());
        this->dial();

        const auto group = this->server_.default_group();
        QVERIFY(group.has_value());
        const server::OpenedInput opened =
            this->server_.open_input(*group, {.format = {48000, 2, server::SampleFormat::S16}});
        QCOMPARE(opened.result, server::OpenInputResult::OK);
        const std::vector<int16_t> silence(48000 * 2, 0);
        size_t written = 0;
        for (int i = 0; i < 100 && written < 48000 && !client.player().streaming(); ++i) {
            written += opened.input->write(silence.data(), 48000 - written).frames;
            QTest::qWait(20);
        }
        if (unpaired_access) {
            QTRY_VERIFY(client.player().streaming());
            // Playback only: the PC's sound is not the server's to take
            QVERIFY(!client.source().streaming());
        } else {
            QVERIFY(!client.player().streaming());
        }
        QVERIFY(!client.is_paired());

        opened.input->stop();
        client.stop();
        QTRY_VERIFY(!this->events_.get(&ServerEvents::connected));
    }

    // Also on the paired identity. No PipeWire: the playback stream has no target, so the
    // audio is taken and counted, which is all the player's side of this needs.
    void playsWhatTheServerSendsAndControlsItsGroup() {
        QVERIFY(!this->client_id_.isEmpty());
        ClientPersistence persistence(this->data_dir());
        Client client(this->config_, &persistence);
        QSignalSpy streaming(&client.player(), &Player::streamingChanged);
        QVERIFY(client.start());
        this->dial();
        QTRY_VERIFY(client.is_paired());
        QTRY_COMPARE(client.state(), Client::State::Connected);
        const auto group = this->server_.default_group();
        QVERIFY(group.has_value());

        // What the group is on, and what may be done about it
        QVERIFY(this->server_.set_controller_caps(
            *group, {.commands = {server::TransportCommand::PAUSE, server::TransportCommand::NEXT,
                                  server::TransportCommand::SEEK},
                     .seek_max_ms = 337'000}));
        server::Metadata metadata;
        metadata.title = "Blue in Green";
        metadata.artist = "Miles Davis";
        metadata.album = "Kind of Blue";
        metadata.track = 3;
        metadata.progress = {.track_progress_ms = 60'000, .track_duration_ms = 337'000};
        QVERIFY(this->server_.set_metadata(*group, metadata, this->server_.now_us()));
        NowPlaying& now_playing = client.now_playing();
        QTRY_COMPARE(now_playing.track().title, "Blue in Green");
        QCOMPARE(now_playing.track().artist, "Miles Davis");
        QCOMPARE(now_playing.track().album, "Kind of Blue");
        QCOMPARE(now_playing.track().number, 3);
        QCOMPARE(now_playing.track().duration_ms, 337'000);
        QTRY_VERIFY(now_playing.can(NowPlaying::Command::NEXT));
        QVERIFY(!now_playing.can(NowPlaying::Command::PREVIOUS));

        // The server plays: a second of a tone
        const server::OpenedInput opened =
            this->server_.open_input(*group, {.format = {48000, 2, server::SampleFormat::S16}});
        QCOMPARE(opened.result, server::OpenInputResult::OK);
        std::vector<int16_t> tone(48000 * 2);
        for (size_t frame = 0; frame < 48000; ++frame) {
            tone[frame * 2] = tone[(frame * 2) + 1] =
                static_cast<int16_t>(8000.0F * std::sin(static_cast<float>(frame) * 0.0576F));
        }
        size_t written = 0;
        QTRY_VERIFY(
            (written += opened.input->write(tone.data() + (written * 2), 48000 - written).frames) ==
            48000);
        QTRY_VERIFY(client.player().streaming());
        QTRY_VERIFY(now_playing.playing());
        QCOMPARE(streaming.size(), 1);
        QVERIFY(now_playing.position_ms() >= 60'000);

        // The position restated as it plays on is no seek; one moved is
        QSignalSpy seeks(&now_playing, &NowPlaying::seeked);
        metadata.progress->track_progress_ms = static_cast<uint32_t>(now_playing.position_ms());
        QVERIFY(this->server_.set_metadata(*group, metadata, this->server_.now_us()));
        metadata.album = "Kind of Blue (Legacy Edition)";
        QVERIFY(this->server_.set_metadata(*group, metadata, this->server_.now_us()));
        QTRY_COMPARE(now_playing.track().album, "Kind of Blue (Legacy Edition)");
        QCOMPARE(seeks.size(), 0);
        metadata.progress->track_progress_ms = 200'000;
        QVERIFY(this->server_.set_metadata(*group, metadata, this->server_.now_us()));
        QTRY_COMPARE(seeks.size(), 1);
        QVERIFY(now_playing.position_ms() >= 200'000);

        // The server sets the player's volume: the stream's, by the spec's curve
        PlaybackStream& stream = client.player().stream();
        QVERIFY(this->server_.set_group_volume(*group, 25));
        QTRY_COMPARE(stream.gain(), 0.125F);
        QTRY_COMPARE(now_playing.volume(), 25);
        QVERIFY(this->server_.set_group_mute(*group, true));
        QTRY_VERIFY(stream.muted());
        // The OS mixer moves the stream's: the player's, which the server hears of
        emit stream.gainChanged(volume_to_amplitude(64));
        QTRY_COMPARE(now_playing.volume(), 64);

        // Commands act on the group, and only those it offers are sent
        now_playing.send({.command = NowPlaying::Command::PREVIOUS});
        now_playing.send({.command = NowPlaying::Command::NEXT});
        now_playing.seek(999'000);
        QTRY_COMPARE(this->events_.get(&ServerEvents::commands),
                     (std::vector{server::TransportCommand::NEXT, server::TransportCommand::SEEK}));

        opened.input->stop();
        QTRY_VERIFY(!client.player().streaming());
        QTRY_VERIFY(!now_playing.playing());
        QCOMPARE(this->events_.get(&ServerEvents::errors), 0);

        client.stop();
        QTRY_VERIFY(!this->events_.get(&ServerEvents::connected));
    }

    // Last: reads what every slot above logged
    void logsNoSecret() {
        QVERIFY(!this->token_.isEmpty());
        fflush(stderr);
        this->stderr_capture_.seek(0);
        const QByteArray log = this->stderr_capture_.readAll() + qt_log;
        // Both captures work
        QVERIFY(log.contains("SsProto") || log.contains("sendspin"));
        QVERIFY(qt_log.contains(this->client_id_.toLatin1()));
        QVERIFY(!log.contains(this->token_.toLatin1()));
        // The token's secret half alone, however it is wrapped
        QVERIFY(!log.contains(this->token_.last(40).toLatin1()));
    }
};

QTEST_GUILESS_MAIN(ClientTest)
#include "client_test.moc"
