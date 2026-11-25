#include "audio/play_along.h"

#include "audio/outputs.h"
#include "audio/pipewire_util.h"
#include "audio/sendspin_output.h"
#include "skip_unless_required.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTest>

#include <atomic>
#include <cmath>
#include <optional>
#include <vector>

namespace {

constexpr uint32_t CHANNELS = PlayAlong::CHANNELS;
constexpr uint32_t CYCLE = 480;
constexpr int64_t MS = 1'000'000;
constexpr int64_t SECOND = 1000 * MS;
/// 48 frames
constexpr int64_t FRAME_NS = SECOND / PlayAlong::RATE;

const QByteArray OWN = "sendspin-test-play-along-own";
const QByteArray SINK = "sendspin-test-play-along-sink";

/// Frames that say where they are: the frame at position n is n + 1 on both channels
std::vector<float> ramp(uint32_t frames, uint32_t from = 0) {
    std::vector<float> samples(static_cast<size_t>(frames) * CHANNELS);
    for (uint32_t i = 0; i < frames; i++) {
        samples[2 * i] = samples[(2 * i) + 1] = static_cast<float>(from + i + 1);
    }
    return samples;
}

/// One cycle of output, as its left channel
std::vector<float> render(PlayAlong& play_along, int64_t heard_ns, uint32_t frames = CYCLE) {
    std::vector<float> out(static_cast<size_t>(frames) * CHANNELS, -1.0F);
    play_along.render(out.data(), frames, heard_ns);
    std::vector<float> left(frames);
    for (uint32_t i = 0; i < frames; i++) {
        left[i] = out[2 * i];
    }
    return left;
}

/// What reached an output, and when, without locks
struct Heard {
    std::atomic<uint64_t> loud_frames{0};
    /// When the first loud frame arrived, by the output's own stamp of its graph cycle
    std::atomic<int64_t> first_loud_ns{0};
    std::atomic<int64_t> peak_millionths{0};

    void captured(const CapturedBlock& block) {
        for (uint32_t i = 0; i < block.frames; i++) {
            const float level = std::abs(block.samples[2 * i]);
            if (level < 0.001F) {
                continue;
            }
            if (this->loud_frames.fetch_add(1) == 0) {
                this->first_loud_ns.store(block.capture_time_ns +
                                          (static_cast<int64_t>(i) * SECOND / block.rate));
            }
            const auto millionths = static_cast<int64_t>(level * 1e6F);
            if (millionths > this->peak_millionths.load()) {
                this->peak_millionths.store(millionths);
            }
        }
    }
};

/// The nodes a node's output is linked to, by pw-dump
QSet<uint32_t> linked_to(uint32_t node) {
    QProcess dump;
    dump.start(QStringLiteral("pw-dump"), QStringList{});
    dump.waitForFinished(5000);
    QSet<uint32_t> inputs;
    const QJsonArray objects = QJsonDocument::fromJson(dump.readAllStandardOutput()).array();
    for (const QJsonValue& object : objects) {
        const QJsonObject info = object[QLatin1String("info")].toObject();
        if (object[QLatin1String("type")].toString() == QLatin1String("PipeWire:Interface:Link") &&
            info[QLatin1String("output-node-id")].toInteger(-1) == node) {
            inputs.insert(static_cast<uint32_t>(info[QLatin1String("input-node-id")].toInteger()));
        }
    }
    return inputs;
}

}  // namespace

class PlayAlongTest : public QObject {
    Q_OBJECT

private slots:
    // --- Each frame at the time it is due: no PipeWire needed ---

    void aFrameIsHeardWhenItIsDue() {
        PlayAlong play_along(OWN);
        QVERIFY(play_along.write(ramp(960).data(), 960, SECOND));
        // The cycle is heard 100 frames before the audio is due: it waits those in silence
        const std::vector<float> first = render(play_along, SECOND - (100 * FRAME_NS));
        QCOMPARE(first[99], 0.0F);
        QCOMPARE(first[100], 1.0F);
        QCOMPARE(first[479], 380.0F);
        // And goes on from there
        const std::vector<float> second = render(play_along, SECOND + (380 * FRAME_NS));
        QCOMPARE(second[0], 381.0F);
        QCOMPARE(play_along.stats().resyncs, 1U);
    }

    void audioDueLaterIsWaitedFor() {
        PlayAlong play_along(OWN);
        QVERIFY(play_along.write(ramp(480).data(), 480, SECOND));
        // Two whole cycles before it is due
        for (const int64_t heard : {SECOND - (20 * MS), SECOND - (10 * MS)}) {
            const std::vector<float> out = render(play_along, heard);
            QCOMPARE(std::ranges::count(out, 0.0F), 480);
        }
        QCOMPARE(render(play_along, SECOND)[0], 1.0F);
    }

    void whatIsLateIsDropped() {
        PlayAlong play_along(OWN);
        QVERIFY(play_along.write(ramp(960).data(), 960, SECOND));
        // 200 frames after the audio was due
        const std::vector<float> out = render(play_along, SECOND + (200 * FRAME_NS));
        QCOMPARE(out[0], 201.0F);
    }

    // The stream opens some time after the first sound: what waited too long goes, block by
    // block, and what is still in time plays in time
    void blocksThatAreLateAreSkippedWhole() {
        PlayAlong play_along(OWN);
        for (uint32_t block = 0; block < 10; block++) {
            QVERIFY(
                play_along.write(ramp(480, block * 480).data(), 480, SECOND + (block * 10 * MS)));
        }
        // 55 ms in: five blocks and half of the sixth are gone
        const std::vector<float> out = render(play_along, SECOND + (55 * MS));
        QCOMPARE(out[0], static_cast<float>((5 * 480) + 240 + 1));
    }

    // A new run of the group starts when it starts, not when the last one's audio ran out
    void aJumpInTheTimelineIsFollowed() {
        PlayAlong play_along(OWN);
        QVERIFY(play_along.write(ramp(480).data(), 480, SECOND));
        QVERIFY(play_along.write(ramp(480, 480).data(), 480, SECOND + (50 * MS)));
        QCOMPARE(render(play_along, SECOND)[479], 480.0F);
        // The second block is due 40 ms after the first ended
        for (const int64_t heard :
             {SECOND + (10 * MS), SECOND + (20 * MS), SECOND + (30 * MS), SECOND + (40 * MS)}) {
            QCOMPARE(std::ranges::count(render(play_along, heard), 0.0F), 480);
        }
        QCOMPARE(render(play_along, SECOND + (50 * MS))[0], 481.0F);
    }

    // The speaker buffer came down, or the group ended and this PC plays directly: what is
    // queued for later than the new audio is not played ahead of it
    void aJumpBackDropsWhatWasQueuedForLater() {
        PlayAlong play_along(OWN);
        QVERIFY(play_along.write(ramp(480).data(), 480, SECOND + (500 * MS)));
        QVERIFY(play_along.write(ramp(480, 480).data(), 480, SECOND));
        // The first block's time has not come; the second's has
        QCOMPARE(std::ranges::count(render(play_along, SECOND - (10 * MS)), 0.0F), 480);
        // Then the first block is late too, and the second by half a second: neither is played
        QCOMPARE(std::ranges::count(render(play_along, SECOND + (510 * MS)), 0.0F), 480);
        QCOMPARE(play_along.queued_frames(), 0U);
    }

    // The audio came too late to go on without a gap
    void runningDryIsAnUnderrunAndTheNextAudioIsPlacedAgain() {
        PlayAlong play_along(OWN);
        QVERIFY(play_along.write(ramp(480).data(), 480, SECOND));
        QCOMPARE(render(play_along, SECOND)[0], 1.0F);
        QCOMPARE(std::ranges::count(render(play_along, SECOND + (10 * MS)), 0.0F), 480);
        render(play_along, SECOND + (20 * MS));

        // The run goes on: what the output had no audio for is late now, and the rest in time
        QVERIFY(play_along.write(ramp(1440, 480).data(), 1440, SECOND + (10 * MS)));
        const std::vector<float> out = render(play_along, SECOND + (30 * MS));
        QCOMPARE(out[0], static_cast<float>(480 + 960 + 1));
        // Once, however many cycles it lasted
        QCOMPARE(play_along.stats().underruns, 1U);
        QCOMPARE(play_along.stats().resyncs, 2U);
    }

    // A run that ends leaves the output with nothing to play, as it should
    void theEndOfARunIsNoUnderrun() {
        PlayAlong play_along(OWN);
        QVERIFY(play_along.write(ramp(480).data(), 480, SECOND));
        QCOMPARE(render(play_along, SECOND)[0], 1.0F);
        QCOMPARE(std::ranges::count(render(play_along, SECOND + (10 * MS)), 0.0F), 480);
        // The next run, seconds later
        QVERIFY(play_along.write(ramp(480, 480).data(), 480, 5 * SECOND));
        QCOMPARE(render(play_along, 5 * SECOND)[0], 481.0F);
        QCOMPARE(play_along.stats().underruns, 0U);
    }

    // A low-latency app sets the graph's quantum to 32 frames: every block has its time, for as
    // long a speaker buffer as the queue holds
    void aQueueOfTheSmallestBlocksIsHeldWhole() {
        PlayAlong play_along(OWN);
        constexpr uint32_t QUANTUM = 32;
        constexpr uint32_t BLOCKS = PlayAlong::QUEUE_FRAMES / QUANTUM;
        for (uint32_t block = 0; block < BLOCKS; block++) {
            QVERIFY2(play_along.write(ramp(QUANTUM, block * QUANTUM).data(), QUANTUM,
                                      SECOND + (static_cast<int64_t>(block) * QUANTUM * SECOND /
                                                PlayAlong::RATE)),
                     qPrintable(QString::number(block)));
        }
        QCOMPARE(play_along.queued_frames(), static_cast<uint32_t>(PlayAlong::QUEUE_FRAMES));
        // Played through in order, in cycles that span many blocks, with no seam between them
        for (uint32_t cycle = 0; cycle < PlayAlong::QUEUE_FRAMES / CYCLE; cycle++) {
            const std::vector<float> out = render(play_along, SECOND + (cycle * 10 * MS));
            QCOMPARE(out[0], static_cast<float>((cycle * CYCLE) + 1));
            QCOMPARE(out[CYCLE - 1], static_cast<float>((cycle + 1) * CYCLE));
        }
        QCOMPARE(play_along.stats().resyncs, 1U);
        QCOMPARE(play_along.stats().dropped, 0U);
        QCOMPARE(play_along.stats().repeated, 0U);
    }

    // The output's clock runs 100 ppm slow: single frames are dropped to keep up, never a resync
    void aDriftingOutputIsFollowedFrameByFrame() {
        PlayAlong play_along(OWN);
        std::vector<float> silence(static_cast<size_t>(CYCLE) * CHANNELS, 0.0F);
        std::vector<float> out(static_cast<size_t>(CYCLE) * CHANNELS);
        double heard = static_cast<double>(SECOND);
        // Some audio ahead, as the speaker buffer leaves it
        constexpr int AHEAD = 5;
        for (int block = 0; block < AHEAD; block++) {
            QVERIFY(play_along.write(silence.data(), CYCLE, SECOND + (block * 10 * MS)));
        }
        for (int cycle = 0; cycle < 5000; cycle++) {
            QVERIFY(play_along.write(silence.data(), CYCLE, SECOND + ((cycle + AHEAD) * 10 * MS)));
            play_along.render(out.data(), CYCLE, static_cast<int64_t>(heard));
            heard += 10.0 * MS * (1.0 + 100e-6);
            QVERIFY(std::abs(play_along.stats().error_ns) < 250'000);
        }
        const PlayAlong::Stats stats = play_along.stats();
        QCOMPARE(stats.resyncs, 1U);
        QCOMPARE(stats.underruns, 0U);
        QVERIFY(stats.dropped > 0);
        QCOMPARE(stats.repeated, 0U);
        // 50 s at 100 ppm is 5 ms: 240 frames
        QVERIFY2(stats.dropped > 200 && stats.dropped < 260,
                 qPrintable(QString::number(stats.dropped)));
    }

    void aChangeOfGainIsRamped() {
        PlayAlong play_along(OWN);
        std::vector<float> ones(static_cast<size_t>(960) * CHANNELS, 1.0F);
        QVERIFY(play_along.write(ones.data(), 960, SECOND));
        play_along.set_gain(0.0F);
        const std::vector<float> down = render(play_along, SECOND);
        QVERIFY(down[0] < 1.0F && down[0] > 0.99F);
        QVERIFY(down[240] < 0.51F && down[240] > 0.49F);
        QCOMPARE(down[479], 0.0F);
        QCOMPARE(render(play_along, SECOND + (10 * MS))[100], 0.0F);
    }

    void aFullQueueDropsTheBlock() {
        PlayAlong play_along(OWN);
        const std::vector<float> second = ramp(PlayAlong::RATE);
        for (int i = 0; i < 4; i++) {
            QVERIFY(play_along.write(second.data(), PlayAlong::RATE, (i + 1) * SECOND));
        }
        QVERIFY(!play_along.write(second.data(), 1, 5 * SECOND));
        // Not counted with no stream to play it: what waits then is stale anyway
        QCOMPARE(play_along.stats().overflows, 0U);
        // It reads on across the end of its storage
        render(play_along, 3 * SECOND, PlayAlong::RATE);
        QVERIFY(play_along.write(second.data(), PlayAlong::RATE, 5 * SECOND));
        const std::vector<float> out = render(play_along, 5 * SECOND, PlayAlong::RATE);
        QCOMPARE(out[0], 1.0F);
        QCOMPARE(out[PlayAlong::RATE - 1], static_cast<float>(PlayAlong::RATE));
    }

    // --- Loop guard: never the Sendspin output ---

    void theSendspinOutputIsNoTarget() {
        PlayAlong play_along(OWN);
        play_along.set_target(OWN);
        QVERIFY(!play_along.target().has_value());
        play_along.set_active(true);
        QVERIFY(!play_along.is_open());
        QCOMPARE(play_along.node_id(), PW_ID_ANY);
    }

    // --- Against a live PipeWire graph, into an output of the test's own ---

    void playsOnItsTargetWhenItIsDue() {
        SendspinOutput sink(nullptr, SINK);
        Heard heard;
        sink.set_consumer([&heard](const CapturedBlock& block) { heard.captured(block); });
        if (!sink.start()) {
            SKIP_UNLESS_REQUIRED("no PipeWire daemon");
        }
        PlayAlong play_along(OWN, "sendspin-test-play-along");
        play_along.set_target(SINK);
        play_along.set_active(true);
        QVERIFY(play_along.is_open());
        // The graph runs it, and says how far ahead it needs its audio
        QTRY_VERIFY(play_along.delay_ns() > 0);
        QVERIFY2(play_along.delay_ns() < 500 * MS,
                 qPrintable(QString::number(play_along.delay_ns())));

        // Half a second of a constant level, the first frame due 300 ms from now
        std::vector<float> level(static_cast<size_t>(24000) * CHANNELS, 0.5F);
        const int64_t due_ns = clock_ns(CLOCK_MONOTONIC_RAW) + (300 * MS);
        QVERIFY(play_along.write(level.data(), 24000, due_ns));
        QTRY_VERIFY(heard.loud_frames.load() >= 23000);
        const double peak = static_cast<double>(heard.peak_millionths.load()) / 1e6;
        QVERIFY2(std::abs(peak - 0.5) < 0.01, qPrintable(QString::number(peak)));

        // Sync rests on this: the first frame reached the output when it was due, by the
        // output's own stamp of the graph cycle that delivered it
        const int64_t error_us = (heard.first_loud_ns.load() - due_ns) / 1000;
        qInfo("first frame heard %lld us from when it was due", static_cast<long long>(error_us));
        QVERIFY2(std::abs(error_us) < 250, qPrintable(QString::number(error_us)));
        QCOMPARE(play_along.stats().overflows, 0U);

        play_along.set_active(false);
        QVERIFY(!play_along.is_open());
    }

    // With the Sendspin output as the system's default, this PC's own output is still another,
    // and the stream is linked to that one alone
    void neverLinksToTheSendspinOutput() {
        // It changes the system's default output: never on a desktop's own daemon
        if (!qEnvironmentVariableIsSet("SENDSPIN_REQUIRE_PIPEWIRE_TESTS")) {
            QSKIP("only against the private daemon of ci/start-pipewire.sh");
        }
        SendspinOutput own(nullptr, OWN);
        Heard came_back;
        own.set_consumer([&came_back](const CapturedBlock& block) { came_back.captured(block); });
        QVERIFY(own.start());
        Outputs outputs(OWN);
        QVERIFY(outputs.start());
        const std::optional<OutputSink> before = outputs.local();
        QVERIFY(before.has_value());

        QCOMPARE(QProcess::execute(QStringLiteral("wpctl"),
                                   {QStringLiteral("set-default"), QString::number(own.node_id())}),
                 0);
        QTest::qWait(500);
        const std::optional<OutputSink> local = outputs.local();
        QVERIFY(local.has_value());
        QVERIFY(local->name != OWN);
        QCOMPARE(local->name, before->name);

        PlayAlong play_along(OWN, "sendspin-test-play-along");
        play_along.set_target(local->name);
        play_along.set_active(true);
        QTRY_VERIFY(play_along.delay_ns() > 0);
        std::vector<float> level(static_cast<size_t>(24000) * CHANNELS, 0.5F);
        QVERIFY(play_along.write(level.data(), 24000, clock_ns(CLOCK_MONOTONIC_RAW) + (200 * MS)));
        QTest::qWait(1000);

        // Nothing of it came back in as the PC's own sound
        QCOMPARE(came_back.loud_frames.load(), uint64_t{0});
        const QSet<uint32_t> links = linked_to(play_along.node_id());
        QCOMPARE(links, QSet<uint32_t>{local->id});
        QVERIFY(!links.contains(own.node_id()));

        QCOMPARE(QProcess::execute(QStringLiteral("wpctl"),
                                   {QStringLiteral("set-default"), QString::number(before->id)}),
                 0);
    }
};

QTEST_GUILESS_MAIN(PlayAlongTest)
#include "play_along_test.moc"
