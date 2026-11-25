#include "cast/cast_feed.h"

#include "persistence.h"

#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <ctime>
#include <vector>

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

int64_t raw_now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (static_cast<int64_t>(ts.tv_sec) * SECOND_NS) + ts.tv_nsec;
}

uint32_t queued(const PlayAlong& play_along) {
    return play_along.queued_frames();
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

    // --- This PC's own output ---

    // Beside the speakers each block is due when the group plays it
    void thisPcGetsEachBlockForWhenTheGroupPlaysIt() {
        PlayAlong play_along;
        this->feed_->set_play_along(&play_along);
        this->feed_->set_local(CastFeed::Local::GROUP);
        this->feed_->take(block(SECOND_NS, true));
        this->feed_->take(block(SECOND_NS, true));
        const int64_t plays_ns = this->feed_->input()->time_of(0) * 1000;
        // Well after it was written: the speaker buffer
        QVERIFY(plays_ns - raw_now_ns() > 20'000'000);

        // An output heard 100 frames before the first frame plays waits those 100 frames
        std::vector<float> out(2 * FRAMES * SendspinOutput::CHANNELS, -1.0F);
        play_along.render(out.data(), 2 * FRAMES, plays_ns - (100 * SECOND_NS / 48000));
        QCOMPARE(out[2 * 99], 0.0F);
        QCOMPARE(out[2 * 100], 0.25F);
        // The second block follows the first without a seam
        QCOMPARE(out[2 * (100 + FRAMES)], 0.25F);
        QCOMPARE(play_along.stats().resyncs, 1U);
    }

    // Silence outside a run reaches no speaker, and not this PC either
    void thisPcGetsWhatTheGroupGets() {
        PlayAlong play_along;
        this->feed_->set_play_along(&play_along);
        this->feed_->set_local(CastFeed::Local::GROUP);
        this->feed_->take(block(SECOND_NS, false));
        QCOMPARE(queued(play_along), 0U);
        this->feed_->take(block(SECOND_NS, true));
        QCOMPARE(queued(play_along), FRAMES);
    }

    // Alone there is no group to wait for: a block is due as soon as the output can play it
    void aloneThisPcGetsEachBlockDirectly() {
        PlayAlong play_along;
        this->feed_->set_play_along(&play_along);
        this->feed_->detach();
        this->feed_->set_local(CastFeed::Local::DIRECT);
        this->feed_->take(block(SECOND_NS, true));
        // One block of 10 ms after its capture, for an output whose delay is not known yet
        std::vector<float> out(FRAMES * SendspinOutput::CHANNELS, -1.0F);
        play_along.render(out.data(), FRAMES, SECOND_NS + 5'000'000);
        QCOMPARE(out[2 * 239], 0.0F);
        QCOMPARE(out[2 * 240], 0.25F);
    }

    // Captured audio is never played directly beside a group that plays it
    void withAnInputNothingIsPlayedDirectly() {
        PlayAlong play_along;
        this->feed_->set_play_along(&play_along);
        this->feed_->set_local(CastFeed::Local::DIRECT);
        this->feed_->take(block(SECOND_NS, true));
        QCOMPARE(queued(play_along), 0U);
    }

    void outOfTheGroupThisPcGetsNothing() {
        PlayAlong play_along;
        this->feed_->set_play_along(&play_along);
        this->feed_->take(block(SECOND_NS, true));
        this->feed_->detach();
        this->feed_->take(block(SECOND_NS, true));
        QCOMPARE(queued(play_along), 0U);
    }

    // Alone, silence after the sound is played for as long as a stream would stay open
    void aloneSilenceIsPlayedOnlyWhileThereWasSound() {
        PlayAlong play_along;
        this->feed_->set_play_along(&play_along);
        this->feed_->detach();
        this->feed_->set_local(CastFeed::Local::DIRECT);
        this->feed_->take(block(SECOND_NS, false));
        QCOMPARE(queued(play_along), 0U);
        this->feed_->take(block(SECOND_NS, true));
        this->feed_->take(block(2 * SECOND_NS, false));
        QCOMPARE(queued(play_along), 2 * FRAMES);
        this->feed_->take(block(5 * SECOND_NS, false));
        QCOMPARE(queued(play_along), 2 * FRAMES);
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
