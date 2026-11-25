#include "cast/cast_engine.h"

#include <QElapsedTimer>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <sendspin/client.h>
#include <sendspin/player_role.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <ctime>
#include <numbers>
#include <thread>
#include <vector>

using speaker_menu::Check;
using speaker_menu::Status;

namespace {

uint16_t free_port() {
    QTcpServer server;
    server.listen(QHostAddress::LocalHost, 0);
    return server.serverPort();
}

int64_t raw_now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (static_cast<int64_t>(ts.tv_sec) * 1'000'000'000) + ts.tv_nsec;
}

/// A Sendspin speaker: sendspin-cpp's client with a player role, listening on a local port and
/// counting what it is given to play.
class Peer : public sendspin::PlayerRoleListener,
             public sendspin::SendspinClientListener,
             public sendspin::SendspinNetworkProvider {
public:
    /// @param dir where it keeps its identity, so a second Peer there is the same speaker
    Peer(const QString& dir, uint16_t port, bool unpaired_access, std::string name = "Kitchen")
        : persistence_(dir.toStdString()), client_(config(port, std::move(name))) {
        this->client_.set_persistence_provider(&this->persistence_);
        this->client_.set_network_provider(this);
        this->client_.set_listener(this);
        this->client_.set_unpaired_access_enabled(unpaired_access);
        sendspin::PlayerRoleConfig player;
        player.audio_formats = {{sendspin::SendspinCodecFormat::PCM, 2, 48000, 16}};
        this->player_ = &this->client_.add_player(std::move(player));
        this->player_->set_listener(this);
        this->started = this->client_.start();
        QObject::connect(&this->pump_, &QTimer::timeout, [this] { this->client_.loop(); });
        this->pump_.start(2);
    }

    ~Peer() override {
        this->pump_.stop();
        this->client_.stop();
    }

    QString id() const {
        return QString::fromStdString(this->client_.client_id());
    }

    // An audio device with a short buffer: it takes audio no faster than it plays it, and says
    // when each block will have played. Sync task thread.
    size_t on_audio_output(std::span<const uint8_t> pcm, const sendspin::AudioFormat& format,
                           uint32_t /*timeout_ms*/) override {
        constexpr int64_t BUFFER_US = 20000;
        uint64_t loud = 0;
        for (size_t i = 0; i + 1 < pcm.size(); i += 2) {
            loud += (pcm[i] != 0 || pcm[i + 1] != 0) ? 1 : 0;
        }
        this->loud_samples += loud;

        const auto frames = static_cast<uint32_t>(
            pcm.size() / (static_cast<size_t>(format.channels) * (format.bits_per_sample / 8)));
        const int64_t now_us = sendspin::SendspinClient::now_us();
        this->played_until_us_ = std::max(this->played_until_us_, now_us) +
                                 (int64_t{frames} * 1'000'000 / format.sample_rate);
        this->player_->notify_audio_played(frames, this->played_until_us_);
        if (this->played_until_us_ - now_us > BUFFER_US) {
            std::this_thread::sleep_for(
                std::chrono::microseconds(this->played_until_us_ - now_us - BUFFER_US));
        }
        return pcm.size();
    }
    void on_stream_start() override {
        this->streams_started++;
    }
    void on_stream_end() override {
        this->streams_ended++;
    }
    void on_volume_changed(uint8_t value) override {
        this->volume = value;
    }
    void on_mute_changed(bool value) override {
        this->muted = value;
    }
    void on_pairing_code(std::string_view code, sendspin::SendspinPairingCodeFormat) override {
        this->pairing_code = QString::fromUtf8(code.data(), static_cast<qsizetype>(code.size()));
    }
    bool is_network_ready() override {
        return true;
    }

    bool started{false};
    std::atomic<uint64_t> loud_samples{0};
    int streams_started{0};
    int streams_ended{0};
    int volume{-1};
    bool muted{false};
    QString pairing_code;

private:
    static sendspin::SendspinClientConfig config(uint16_t port, std::string name) {
        sendspin::SendspinClientConfig config;
        config.name = std::move(name);
        config.server_port = port;
        config.pairing_code_out_channels = {sendspin::SendspinPairingCodeChannel::DISPLAY};
        config.pairing_code_formats = {sendspin::SendspinPairingCodeFormat::DIGITS};
        return config;
    }

    ClientPersistence persistence_;
    sendspin::SendspinClient client_;
    sendspin::PlayerRole* player_{nullptr};
    int64_t played_until_us_{0};  ///< sync task thread only
    QTimer pump_;
};

/// The app's side of a test: the engine with a speaker to dial, and no PipeWire or mDNS
struct Rig {
    explicit Rig(const QString& dir, uint16_t speaker_port) : dir(dir) {
        QSettings settings(dir + "/cast.ini", QSettings::IniFormat);
        settings.setValue(
            "speaker_urls",
            QStringList{QStringLiteral("ws://127.0.0.1:%1/sendspin").arg(speaker_port)});
        settings.sync();
        this->engine = std::make_unique<CastEngine>(dir.toStdString(), this->output);
    }

    /// The row of the speaker with this id; nullptr while it isn't listed
    const SpeakerRow* row(const QString& id) const {
        for (const SpeakerRow& row : this->engine->speakers()->rows()) {
            if (row.id == id) {
                return &row;
            }
        }
        return nullptr;
    }

    Check check(const QString& id) const {
        const SpeakerRow* row = this->row(id);
        return row != nullptr ? row->row.check : Check::NONE;
    }

    Status status(const QString& id) const {
        const SpeakerRow* row = this->row(id);
        return row != nullptr ? row->row.status : Status::NONE;
    }

    /// Plays to the Sendspin output for ms, in real time as the graph would: a tone, or silence
    void play(int ms, bool tone = true) {
        constexpr uint32_t BLOCK = 480;
        std::vector<float> samples(BLOCK * SendspinOutput::CHANNELS, 0.0F);
        QElapsedTimer clock;
        clock.start();
        uint64_t sent = 0;
        while (clock.elapsed() < ms) {
            QTest::qWait(5);
            const uint64_t due =
                static_cast<uint64_t>(clock.elapsed()) * SendspinOutput::RATE / 1000;
            for (; sent + BLOCK <= due; sent += BLOCK) {
                if (tone) {
                    for (uint32_t i = 0; i < BLOCK; i++) {
                        const double phase = 2.0 * std::numbers::pi * 440.0 *
                                             static_cast<double>(this->frame++) /
                                             SendspinOutput::RATE;
                        samples[2 * i] = samples[(2 * i) + 1] =
                            static_cast<float>(0.25 * std::sin(phase));
                    }
                }
                this->engine->feed().take(
                    {samples.data(), BLOCK, SendspinOutput::RATE, raw_now_ns(), !tone});
            }
        }
    }

    QString dir;
    SendspinOutput output;
    std::unique_ptr<CastEngine> engine;
    uint64_t frame{0};
};

}  // namespace

// The pinned sendspin-server-cpp (f217a96) sends a run's stream/start ahead of the
// server/activate that declares `playback`, and the pinned sendspin-cpp (v0.10.0) ignores a
// stream/start that comes without it, as the spec now asks. Until a server that orders them the
// other way is pinned, the speaker in these tests gets no stream; every check up to this one
// still holds. An unexpected pass fails the test: take this out then.
#define EXPECT_STREAM_IGNORED() \
    QEXPECT_FAIL("", "sendspin-server-cpp sends stream/start before it declares playback", Abort)

// The engine against a real sendspin-cpp player on the loopback interface.
class CastEngineTest : public QObject {
    Q_OBJECT

private slots:
    void init() {
        this->app_dir_ = std::make_unique<QTemporaryDir>();
        this->speaker_dir_ = std::make_unique<QTemporaryDir>();
        this->port_ = free_port();
    }

    // Our server must not take our own client's one connection
    void browsingLeavesThisAppsOwnClientAlone() {
        const QList<QHostAddress> own{QHostAddress("192.0.2.7"), QHostAddress("fe80::1%eth0")};
        sendspin::server::DiscoveredClient found;
        found.port = 8928;
        found.addresses = {"192.0.2.7"};
        QVERIFY(is_own_client(found, 8928, own));
        found.addresses = {"203.0.113.9", "fe80::1%wlan0"};
        QVERIFY(is_own_client(found, 8928, own));

        // Another machine's client on the same port, and another client on this machine
        found.addresses = {"192.0.2.8"};
        QVERIFY(!is_own_client(found, 8928, own));
        found.addresses = {"192.0.2.7"};
        QVERIFY(!is_own_client(found, 8930, own));
        // No client of our own
        QVERIFY(!is_own_client(found, 0, own));
    }

    // Nothing plays on a speaker the user didn't add
    void aSpeakerThatIsFoundIsListedAndWaitsOutOfTheGroup() {
        Peer peer(this->speaker_dir_->path(), this->port_, true);
        QVERIFY(peer.started);
        Rig rig(this->app_dir_->path(), this->port_);
        QVERIFY(rig.engine->start(0, false));

        QTRY_VERIFY(rig.row(peer.id()) != nullptr);
        QCOMPARE(rig.row(peer.id())->name, QStringLiteral("Kitchen"));
        QCOMPARE(rig.check(peer.id()), Check::EMPTY);
        QCOMPARE(rig.status(peer.id()), Status::NONE);
        QVERIFY(!rig.row(peer.id())->in_group);

        rig.play(500);
        QCOMPARE(peer.streams_started, 0);
        QCOMPARE(peer.loud_samples.load(), 0U);
        QVERIFY(!rig.engine->playing());
    }

    void addingASpeakerPutsItInTheGroupAndRemovingTakesItOut() {
        Peer peer(this->speaker_dir_->path(), this->port_, true);
        Rig rig(this->app_dir_->path(), this->port_);
        QVERIFY(rig.engine->start(0, false));
        QTRY_VERIFY(rig.row(peer.id()) != nullptr);

        rig.engine->add(peer.id());
        QVERIFY(rig.row(peer.id())->in_group);
        QTRY_COMPARE(rig.check(peer.id()), Check::FILLED);
        QTRY_VERIFY(rig.engine->feed().input() != nullptr);
        // The speaker buffer is the library's
        QVERIFY(rig.engine->feed().input()->latency_us() > 0);

        rig.play(500);
        QVERIFY(rig.engine->playing());
        QCOMPARE(rig.engine->feed().written_frames(), 24000U);
        QCOMPARE(rig.engine->feed().dropped_frames(), 0U);

        rig.engine->remove(peer.id());
        QCOMPARE(rig.check(peer.id()), Check::EMPTY);
        // The input is held only while a speaker is added
        QVERIFY(rig.engine->feed().input() == nullptr);
        rig.play(300);
        QCOMPARE(rig.engine->feed().written_frames(), 24000U);
        QVERIFY(!rig.engine->playing());
    }

    void anAddedSpeakerPlaysThePcsSoundAndRemovingItStopsIt() {
        Peer peer(this->speaker_dir_->path(), this->port_, true);
        Rig rig(this->app_dir_->path(), this->port_);
        QVERIFY(rig.engine->start(0, false));
        QTRY_VERIFY(rig.row(peer.id()) != nullptr);
        rig.engine->add(peer.id());
        QTRY_COMPARE(rig.check(peer.id()), Check::FILLED);
        QTRY_VERIFY(rig.engine->feed().input() != nullptr);

        rig.play(1500);
        EXPECT_STREAM_IGNORED();
        QTRY_VERIFY(peer.loud_samples.load() > 0);
        QCOMPARE(peer.streams_started, 1);

        rig.engine->remove(peer.id());
        QTRY_COMPARE(peer.streams_ended, 1);
        const uint64_t heard = peer.loud_samples.load();
        rig.play(500);
        QCOMPARE(peer.loud_samples.load(), heard);
        QCOMPARE(peer.streams_started, 1);
    }

    void idleSilenceEndsTheStreamAndSoundRestartsIt() {
        Peer peer(this->speaker_dir_->path(), this->port_, true);
        Rig rig(this->app_dir_->path(), this->port_);
        QVERIFY(rig.engine->start(0, false));
        QTRY_VERIFY(rig.row(peer.id()) != nullptr);
        rig.engine->add(peer.id());
        QTRY_COMPARE(rig.check(peer.id()), Check::FILLED);
        QTRY_VERIFY(rig.engine->feed().input() != nullptr);

        rig.play(1000);
        // A short silence keeps the stream
        rig.play(1000, false);
        QVERIFY(rig.engine->feed().streaming());
        QVERIFY(rig.engine->playing());

        rig.play(CastFeed::IDLE_NS / 1'000'000, false);
        QTRY_VERIFY(!rig.engine->feed().streaming());
        QTRY_VERIFY(!rig.engine->playing());
        // The speaker stays in the group, with nothing to play
        QCOMPARE(rig.check(peer.id()), Check::FILLED);

        rig.play(1500);
        QVERIFY(rig.engine->feed().streaming());
        QVERIFY(rig.engine->playing());

        // What the speaker saw of it: two streams, with an end between them
        EXPECT_STREAM_IGNORED();
        QCOMPARE(peer.streams_started, 2);
        QVERIFY(peer.streams_ended >= 1);
        QVERIFY(peer.loud_samples.load() > 0);
    }

    void theGroupsVolumeAndMuteReachItsSpeakers() {
        Peer peer(this->speaker_dir_->path(), this->port_, true);
        Rig rig(this->app_dir_->path(), this->port_);
        QVERIFY(rig.engine->start(0, false));
        QTRY_VERIFY(rig.row(peer.id()) != nullptr);
        rig.engine->add(peer.id());
        QTRY_COMPARE(rig.check(peer.id()), Check::FILLED);
        QTRY_VERIFY(rig.engine->feed().input() != nullptr);
        // Adding a speaker leaves its volume alone until one is set here
        QTest::qWait(300);
        QCOMPARE(peer.volume, -1);

        QSignalSpy changed(rig.engine.get(), &CastEngine::groupVolumeChanged);
        rig.engine->setGroupVolume(40);
        QCOMPARE(changed.count(), 1);
        QTRY_COMPARE(peer.volume, 40);

        rig.engine->setGroupMuted(true);
        QTRY_VERIFY(peer.muted);
        rig.engine->setGroupMuted(false);
        QTRY_VERIFY(!peer.muted);
    }

    void whatYouAddedStaysWhenItGoesAwayAndComesBackByItself() {
        auto peer = std::make_unique<Peer>(this->speaker_dir_->path(), this->port_, true);
        const QString id = peer->id();
        Rig rig(this->app_dir_->path(), this->port_);
        QVERIFY(rig.engine->start(0, false));
        QTRY_VERIFY(rig.row(id) != nullptr);
        rig.engine->add(id);
        QTRY_COMPARE(rig.check(id), Check::FILLED);

        peer.reset();
        QTRY_COMPARE(rig.status(id), Status::UNAVAILABLE);
        QCOMPARE(rig.check(id), Check::GREY);
        QVERIFY(rig.row(id)->row.warning);
        QCOMPARE(rig.row(id)->name, QStringLiteral("Kitchen"));

        // The same speaker, back on the network: the library dials it and the app moves it in
        peer = std::make_unique<Peer>(this->speaker_dir_->path(), this->port_, true);
        QCOMPARE(peer->id(), id);
        QTRY_COMPARE_WITH_TIMEOUT(rig.check(id), Check::FILLED, 30000);
        QCOMPARE(rig.status(id), Status::NONE);
    }

    // Another device under an added speaker's address is not let into the group unasked
    void anUnknownDeviceInAnAddedSpeakersPlaceShowsIdentityChangedUntilAccepted() {
        auto kitchen = std::make_unique<Peer>(this->speaker_dir_->path(), this->port_, true);
        const QString id = kitchen->id();
        Rig rig(this->app_dir_->path(), this->port_);
        QVERIFY(rig.engine->start(0, false));
        QTRY_VERIFY(rig.row(id) != nullptr);
        rig.engine->add(id);
        QTRY_COMPARE(rig.check(id), Check::FILLED);
        kitchen.reset();
        QTRY_COMPARE(rig.status(id), Status::UNAVAILABLE);

        const QTemporaryDir other_dir;
        Peer impostor(other_dir.path(), this->port_, true);
        QVERIFY(impostor.id() != id);
        QTRY_COMPARE_WITH_TIMEOUT(rig.status(impostor.id()), Status::IDENTITY_CHANGED, 30000);
        // One row for the two, grey: it is not approved and does not play
        QVERIFY(rig.row(id) == nullptr);
        QCOMPARE(rig.check(impostor.id()), Check::GREY);
        QVERIFY(rig.row(impostor.id())->row.warning);
        QTest::qWait(600);
        QCOMPARE(rig.status(impostor.id()), Status::IDENTITY_CHANGED);

        rig.engine->add(impostor.id());
        QTRY_COMPARE(rig.check(impostor.id()), Check::FILLED);
        QCOMPARE(rig.status(impostor.id()), Status::NONE);
        QVERIFY(rig.row(id) == nullptr);
    }

    void theAddedSpeakersAndTheVolumeAreKeptAcrossRestarts() {
        Peer peer(this->speaker_dir_->path(), this->port_, true);
        {
            Rig rig(this->app_dir_->path(), this->port_);
            QVERIFY(rig.engine->start(0, false));
            QTRY_VERIFY(rig.row(peer.id()) != nullptr);
            rig.engine->add(peer.id());
            QTRY_COMPARE(rig.check(peer.id()), Check::FILLED);
            QTRY_VERIFY(rig.engine->feed().input() != nullptr);
            rig.engine->setGroupVolume(30);
            QTRY_COMPARE(peer.volume, 30);
            // Changed at the speaker: the stored value is applied to the next group
            peer.volume = -1;
        }
        Rig rig(this->app_dir_->path(), this->port_);
        QCOMPARE(rig.engine->group_volume(), 30);
        // Listed before it is found again, by the name it had
        QVERIFY(rig.row(peer.id()) != nullptr);
        QVERIFY(rig.row(peer.id())->in_group);
        QCOMPARE(rig.row(peer.id())->name, QStringLiteral("Kitchen"));

        QVERIFY(rig.engine->start(0, false));
        QTRY_COMPARE_WITH_TIMEOUT(rig.check(peer.id()), Check::FILLED, 30000);
        QTRY_COMPARE(peer.volume, 30);
    }

    void aSpeakerThatNeedsPairingIsPairedWhenAddedAndForgottenAgain() {
        Peer peer(this->speaker_dir_->path(), this->port_, false);
        Rig rig(this->app_dir_->path(), this->port_);
        QVERIFY(rig.engine->start(0, false));
        QTRY_VERIFY(rig.row(peer.id()) != nullptr);
        QTRY_COMPARE(rig.status(peer.id()), Status::NEEDS_PAIRING);
        QVERIFY(rig.row(peer.id())->can_pair);
        QVERIFY(!rig.row(peer.id())->paired);

        QSignalSpy code_needed(rig.engine.get(), &CastEngine::pairingCodeNeeded);
        QSignalSpy finished(rig.engine.get(), &CastEngine::pairingFinished);
        rig.engine->add(peer.id());
        QTRY_COMPARE(code_needed.count(), 1);
        QCOMPARE(code_needed.first().at(0).toString(), peer.id());
        QTRY_VERIFY(!peer.pairing_code.isEmpty());
        QVERIFY(rig.engine->submitPairingCode(peer.id(), peer.pairing_code));

        QTRY_COMPARE(finished.count(), 1);
        QCOMPARE(finished.first().at(1).value<sendspin::server::PairingResult>(),
                 sendspin::server::PairingResult::PAIRED);
        QTRY_COMPARE_WITH_TIMEOUT(rig.check(peer.id()), Check::FILLED, 30000);
        QVERIFY(rig.row(peer.id())->paired);

        rig.engine->forget(peer.id());
        QTRY_VERIFY(rig.row(peer.id()) != nullptr && !rig.row(peer.id())->paired);
        QVERIFY(!rig.row(peer.id())->in_group);
    }

private:
    std::unique_ptr<QTemporaryDir> app_dir_;
    std::unique_ptr<QTemporaryDir> speaker_dir_;
    uint16_t port_{0};
};

QTEST_GUILESS_MAIN(CastEngineTest)
#include "cast_engine_test.moc"
