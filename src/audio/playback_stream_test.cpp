#include "audio/playback_stream.h"

#include "audio/pipewire_util.h"
#include "audio/sendspin_output.h"
#include "audio/volume_curve.h"

#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include <atomic>
#include <cmath>
#include <optional>
#include <vector>

namespace {

const QByteArray SINK = "sendspin-test-playback-sink";
constexpr PlaybackStream::Format FORMAT{48000, 2, 16};
constexpr size_t CHUNK_FRAMES = 480;

/// What the stream reported played, and what reached the output, without locks
struct Heard {
    std::atomic<uint64_t> played_frames{0};
    std::atomic<uint64_t> reports{0};
    std::atomic<uint64_t> out_of_range{0};
    std::atomic<uint64_t> loud_frames{0};
    std::atomic<int64_t> peak_millionths{0};

    void played(uint32_t frames, int64_t finish_us) {
        this->played_frames += frames;
        this->reports++;
        // Heard within moments of now, on the libraries' clock. Audio with nowhere to play is
        // stamped as it is taken, a moment before this runs.
        const int64_t ahead_us = finish_us - (clock_ns(CLOCK_MONOTONIC_RAW) / 1000);
        if (ahead_us < -10'000 || ahead_us > 500'000) {
            this->out_of_range++;
        }
    }
    void captured(const CapturedBlock& block) {
        if (block.silent) {
            return;
        }
        this->loud_frames += block.frames;
        float peak = 0.0F;
        for (size_t i = 0; i < size_t{block.frames} * SendspinOutput::CHANNELS; ++i) {
            peak = std::max(peak, std::abs(block.samples[i]));
        }
        const auto millionths = static_cast<int64_t>(peak * 1e6F);
        if (millionths > this->peak_millionths.load()) {
            this->peak_millionths.store(millionths);
        }
    }
    void reset_peak() {
        this->loud_frames.store(0);
        this->peak_millionths.store(0);
    }
};

/// 10 ms of a half-scale tone
std::vector<int16_t> tone() {
    static uint32_t position = 0;
    std::vector<int16_t> samples(CHUNK_FRAMES * FORMAT.channels);
    for (size_t frame = 0; frame < CHUNK_FRAMES; ++frame, ++position) {
        const auto value =
            static_cast<int16_t>(16384.0F * std::sin(static_cast<float>(position) * 0.0576F));
        samples[frame * 2] = samples[(frame * 2) + 1] = value;
    }
    return samples;
}

/// Writes ms of tone as the player does: whole chunks, again when one is not taken whole
size_t play(PlaybackStream& stream, int ms) {
    size_t frames = 0;
    for (int chunk = 0; chunk < ms / 10; ++chunk) {
        const std::vector<int16_t> samples = tone();
        const auto* bytes = reinterpret_cast<const uint8_t*>(samples.data());
        const size_t length = samples.size() * sizeof(int16_t);
        size_t done = 0;
        for (int attempt = 0; done < length && attempt < 50; ++attempt) {
            done += stream.write(bytes + done, length - done, FORMAT, 100);
        }
        frames += done / (FORMAT.channels * sizeof(int16_t));
        // write() is another thread's in the app: let the event loop see the stream's reports
        QCoreApplication::processEvents();
    }
    return frames;
}

}  // namespace

/// The playback stream against a live PipeWire graph, playing into an output of the test's own
/// so nothing is heard. Skipped without a daemon.
class PlaybackStreamTest : public QObject {
    Q_OBJECT

private:
    Heard heard_;
    SendspinOutput sink_{nullptr, SINK};
    bool live_{false};

private slots:
    void initTestCase() {
        this->sink_.set_consumer(
            [this](const CapturedBlock& block) { this->heard_.captured(block); });
        this->live_ = this->sink_.start();
    }

    void audioWithNowhereToPlayIsTakenAndCounted() {
        PlaybackStream stream(nullptr, "sendspin-test-player");
        Heard heard;
        stream.set_played(
            [&heard](uint32_t frames, int64_t finish_us) { heard.played(frames, finish_us); });
        QCOMPARE(play(stream, 100), size_t{4800});
        QCOMPARE(heard.played_frames.load(), uint64_t{4800});
        QCOMPARE(heard.out_of_range.load(), uint64_t{0});
        QVERIFY(!stream.is_open());
    }

    void playsOnItsTargetAndReportsWhenItIsHeard() {
        if (!this->live_) {
            QSKIP("no PipeWire daemon");
        }
        PlaybackStream stream(nullptr, "sendspin-test-player");
        stream.set_played(
            [this](uint32_t frames, int64_t finish_us) { this->heard_.played(frames, finish_us); });
        stream.set_target(SINK);
        const size_t written = play(stream, 1000);
        QCOMPARE(written, size_t{48000});
        QVERIFY(stream.is_open());

        // All of it leaves for the output, each report timed for moments from now
        QTRY_COMPARE(this->heard_.played_frames.load(), uint64_t{48000});
        QVERIFY(this->heard_.reports.load() > 10);
        QCOMPARE(this->heard_.out_of_range.load(), uint64_t{0});
        // And arrives there, at the scale it was written
        QTRY_VERIFY(this->heard_.loud_frames.load() > 40000);
        const double peak = static_cast<double>(this->heard_.peak_millionths.load()) / 1e6;
        QVERIFY2(std::abs(peak - 0.5) < 0.01, qPrintable(QString::number(peak)));
    }

    void theVolumeIsTheStreamsOwnBothWays() {
        if (!this->live_) {
            QSKIP("no PipeWire daemon");
        }
        if (QStandardPaths::findExecutable(QStringLiteral("wpctl")).isEmpty()) {
            QSKIP("wpctl is not installed");
        }
        PlaybackStream stream(nullptr, "sendspin-test-player");
        QSignalSpy gains(&stream, &PlaybackStream::gainChanged);
        QSignalSpy mutes(&stream, &PlaybackStream::mutedChanged);
        stream.set_target(SINK);
        // Set before there is a stream: it opens at this volume, and says nothing of its own
        stream.set_gain(volume_to_amplitude(25));
        play(stream, 300);
        this->heard_.reset_peak();
        play(stream, 500);
        QTRY_VERIFY(this->heard_.loud_frames.load() > 10000);
        const double peak = static_cast<double>(this->heard_.peak_millionths.load()) / 1e6;
        QVERIFY2(std::abs(peak - 0.5 * 0.125) < 0.005, qPrintable(QString::number(peak)));
        QCOMPARE(gains.size(), 0);
        QCOMPARE(mutes.size(), 0);

        // The OS mixer moves it, a while after it was last set from here; wpctl's scale is the
        // cube root of the gain
        QTest::qWait(400);
        const QString id = QString::number(stream.node_id());
        QCOMPARE(QProcess::execute(QStringLiteral("wpctl"), {"set-volume", id, "0.8"}), 0);
        QTRY_COMPARE(gains.size(), 1);
        QVERIFY(std::abs(gains.first().first().toFloat() - 0.512F) < 0.001F);
        QCOMPARE(QProcess::execute(QStringLiteral("wpctl"), {"set-mute", id, "1"}), 0);
        QTRY_COMPARE(mutes.size(), 1);
        QCOMPARE(mutes.first().first().toBool(), true);

        // Setting it from here is not the mixer's doing
        stream.set_muted(false);
        stream.set_gain(1.0F);
        play(stream, 200);
        QTest::qWait(200);
        QCOMPARE(gains.size(), 1);
        QCOMPARE(mutes.size(), 1);
    }

    void movesToANewTargetAndKeepsCount() {
        if (!this->live_) {
            QSKIP("no PipeWire daemon");
        }
        SendspinOutput other(nullptr, "sendspin-test-playback-other");
        Heard elsewhere;
        other.set_consumer([&elsewhere](const CapturedBlock& block) { elsewhere.captured(block); });
        QVERIFY(other.start());

        PlaybackStream stream(nullptr, "sendspin-test-player");
        Heard heard;
        stream.set_played(
            [&heard](uint32_t frames, int64_t finish_us) { heard.played(frames, finish_us); });
        stream.set_target(SINK);
        size_t written = play(stream, 300);
        stream.set_target(QByteArray("sendspin-test-playback-other"));
        written += play(stream, 500);
        QTRY_VERIFY(elsewhere.loud_frames.load() > 10000);
        // What the old stream still held is counted with the rest
        QTRY_COMPARE(heard.played_frames.load(), uint64_t{written});

        // The end of the server's stream closes it, and the next one starts from nothing
        stream.stop();
        QVERIFY(!stream.is_open());
        const uint64_t before = heard.played_frames.load();
        const size_t again = play(stream, 300);
        QVERIFY(stream.is_open());
        QTRY_COMPARE(heard.played_frames.load(), before + again);
    }
};

QTEST_GUILESS_MAIN(PlaybackStreamTest)
#include "playback_stream_test.moc"
