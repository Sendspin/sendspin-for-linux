#include "cast/cast_feed.h"

#include "persistence.h"

#include <QTemporaryDir>
#include <QTest>

#include <array>

using namespace sendspin::server;

namespace {

constexpr int64_t SECOND_NS = 1'000'000'000;
constexpr uint32_t FRAMES = 480;

/// A 10 ms block at capture time time_ns, of a constant level or of silence
CapturedBlock block(int64_t time_ns, bool sound) {
    static const std::array<float, FRAMES * SendspinOutput::CHANNELS> silence{};
    static const std::array<float, FRAMES * SendspinOutput::CHANNELS> tone = [] {
        std::array<float, FRAMES * SendspinOutput::CHANNELS> samples{};
        samples.fill(0.25F);
        return samples;
    }();
    return {sound ? tone.data() : silence.data(), FRAMES, SendspinOutput::RATE, time_ns, !sound};
}

}  // namespace

// The idle rule: 3 s without sound ends the stream, and the next sound starts a new one.
class CastFeedTest : public QObject {
    Q_OBJECT

private slots:
    void init() {
        ServerConfig config;
        config.listen_port = 0;
        config.advertise = false;
        config.browse = false;
        this->persistence_ = std::make_unique<ServerPersistence>(this->dir_.path().toStdString());
        this->server_ = std::make_unique<SendspinServer>();
        QCOMPARE(this->server_->start(config, this->persistence_.get(), nullptr), StartResult::OK);

        InputOptions options;
        options.format = {SendspinOutput::RATE, SendspinOutput::CHANNELS, SampleFormat::F32};
        options.kind = InputKind::LIVE;
        OpenedInput opened = this->server_->open_input(*this->server_->default_group(), options);
        QCOMPARE(opened.result, OpenInputResult::OK);
        // It admits nothing until the server has attached it to its group
        QTRY_COMPARE(opened.input->state(), InputState::OPEN);
        this->feed_ = std::make_unique<CastFeed>();
        this->feed_->attach(std::move(opened.input));
    }

    void cleanup() {
        this->feed_.reset();
        this->server_.reset();
    }

    // Silence must not open a stream on the speakers
    void silenceStartsNoStream() {
        this->feed_->take(block(SECOND_NS, false));
        QVERIFY(!this->feed_->streaming());
        QCOMPARE(this->feed_->written_frames(), 0U);
        QVERIFY(!this->feed_->has_sound(SECOND_NS));
    }

    void soundStartsAStream() {
        this->feed_->take(block(SECOND_NS, true));
        QVERIFY(this->feed_->streaming());
        QCOMPARE(this->feed_->written_frames(), FRAMES);
        QVERIFY(this->feed_->has_sound(SECOND_NS));
    }

    // A pause between tracks keeps the stream and its timeline
    void silenceShorterThanThreeSecondsIsWrittenAndKeepsTheStream() {
        this->feed_->take(block(SECOND_NS, true));
        this->feed_->take(block(2 * SECOND_NS, false));
        QCOMPARE(this->feed_->poll((4 * SECOND_NS) - 1), CastFeed::State::OK);
        QVERIFY(this->feed_->streaming());
        QCOMPARE(this->feed_->written_frames(), 2 * FRAMES);
    }

    void threeSecondsOfSilenceEndTheStream() {
        this->feed_->take(block(SECOND_NS, true));
        this->feed_->take(block(3 * SECOND_NS, false));
        QCOMPARE(this->feed_->poll(4 * SECOND_NS), CastFeed::State::OK);
        QVERIFY(!this->feed_->streaming());
        QVERIFY(!this->feed_->has_sound(4 * SECOND_NS));
        // Silence after the end is not written: it would start a stream
        this->feed_->take(block(5 * SECOND_NS, false));
        QCOMPARE(this->feed_->written_frames(), 2 * FRAMES);
    }

    // The graph stops the output while nothing plays to it, so playback ends as blocks stopping
    void noBlocksAtAllCountAsSilence() {
        this->feed_->take(block(SECOND_NS, true));
        this->feed_->poll(4 * SECOND_NS);
        QVERIFY(!this->feed_->streaming());
    }

    void soundRestartsTheStream() {
        this->feed_->take(block(SECOND_NS, true));
        this->feed_->poll(4 * SECOND_NS);
        QVERIFY(!this->feed_->streaming());
        // The input took the stop and admits a new run
        QTRY_COMPARE(this->feed_->input()->state(), InputState::OPEN);

        this->feed_->take(block(5 * SECOND_NS, true));
        QVERIFY(this->feed_->streaming());
        QCOMPARE(this->feed_->written_frames(), 2 * FRAMES);
        QVERIFY(this->feed_->has_sound(5 * SECOND_NS));
    }

    void detachedItWritesNothing() {
        this->feed_->detach();
        this->feed_->take(block(SECOND_NS, true));
        QVERIFY(!this->feed_->input());
        QVERIFY(!this->feed_->streaming());
        QCOMPARE(this->feed_->written_frames(), 0U);
    }

    // The server stopping takes the input with it; the engine releases it
    void anInputTheServerClosedIsReported() {
        this->feed_->take(block(SECOND_NS, true));
        this->server_->stop();
        QTRY_COMPARE(this->feed_->poll(SECOND_NS), CastFeed::State::CLOSED);
    }

private:
    QTemporaryDir dir_;
    std::unique_ptr<ServerPersistence> persistence_;
    std::unique_ptr<SendspinServer> server_;
    std::unique_ptr<CastFeed> feed_;
};

QTEST_GUILESS_MAIN(CastFeedTest)
#include "cast_feed_test.moc"
