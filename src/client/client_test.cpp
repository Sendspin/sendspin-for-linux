#include "client.h"

#include "persistence.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <sendspin/server/server.h>

#include <unistd.h>

#include <cmath>
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
    bool code_needed{false};
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
        this->code_needed = true;
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
        QVERIFY(this->server_.begin_pairing(id, {}));
        QTRY_COMPARE(client.pairing_code().size(), 6);
        QCOMPARE(client.state(), Client::State::Pairing);
        QCOMPARE(codes.size(), 1);
        QTRY_VERIFY(this->events_.get(&ServerEvents::code_needed));
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
