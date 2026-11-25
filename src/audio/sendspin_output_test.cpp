#include "audio/sendspin_output.h"

#include "skip_unless_required.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QVersionNumber>

#include <atomic>
#include <cmath>
#include <ctime>
#include <numbers>

namespace {

constexpr int TONE_MS = 1000;
/// How late into its cycle a callback may run on a busy machine.
constexpr int64_t MAX_SCHEDULING_LAG_NS = 20000000;
/// How far a block's timestamp may be from where the block before it ended: the graph's own
/// wakeup jitter.
constexpr int64_t MAX_STEP_ERROR_NS = 1000000;

int64_t raw_now_ns() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
    return (static_cast<int64_t>(ts.tv_sec) * 1000000000) + ts.tv_nsec;
}

/// What the realtime callback saw, without locks.
struct Capture {
    std::atomic<uint64_t> blocks{0};
    std::atomic<uint64_t> loud_frames{0};
    std::atomic<uint64_t> silent_blocks{0};
    std::atomic<uint64_t> out_of_order{0};
    std::atomic<uint64_t> off_clock{0};
    std::atomic<int64_t> worst_step_error_ns{0};
    std::atomic<int64_t> worst_age_ns{0};
    std::atomic<uint64_t> wrong_rate{0};
    std::atomic<int64_t> peak_millionths{0};
    int64_t last_time_ns{0};  ///< callback only
    int64_t next_time_ns{0};  ///< callback only; where the last block ended

    void take(const CapturedBlock& block) {
        this->blocks++;
        if (block.rate != SendspinOutput::RATE) {
            this->wrong_rate++;
        }
        if (block.capture_time_ns <= this->last_time_ns) {
            this->out_of_order++;
        }
        this->last_time_ns = block.capture_time_ns;
        // One run of back-to-back blocks: each starts where the last one ended
        if (this->next_time_ns != 0) {
            const int64_t error_ns = std::abs(block.capture_time_ns - this->next_time_ns);
            if (error_ns > this->worst_step_error_ns.load()) {
                this->worst_step_error_ns.store(error_ns);
            }
        }
        const int64_t block_ns = static_cast<int64_t>(block.frames) * 1000000000 / block.rate;
        this->next_time_ns = block.capture_time_ns + block_ns;
        // The callback runs within the cycle the block was captured in, on the same clock. A
        // timestamp left on CLOCK_MONOTONIC would be off by that clock's offset from the raw one.
        const int64_t age_ns = raw_now_ns() - block.capture_time_ns;
        if (age_ns < 0 || age_ns > block_ns + MAX_SCHEDULING_LAG_NS) {
            this->off_clock++;
        }
        if (age_ns > this->worst_age_ns.load()) {
            this->worst_age_ns.store(age_ns);
        }
        if (block.silent) {
            this->silent_blocks++;
            return;
        }
        this->loud_frames += block.frames;
        float peak = 0.0F;
        for (uint32_t i = 0; i < block.frames * SendspinOutput::CHANNELS; i++) {
            peak = std::max(peak, std::abs(block.samples[i]));
        }
        const auto scaled = static_cast<int64_t>(peak * 1e6F);
        if (scaled > this->peak_millionths.load()) {
            this->peak_millionths.store(scaled);
        }
    }
};

/// A 16-bit stereo 48 kHz sine at half scale.
bool write_tone(const QString& path) {
    const quint32 rate = SendspinOutput::RATE;
    const quint32 frames = rate * TONE_MS / 1000;
    const quint32 data_bytes = frames * 4;
    QByteArray wav;
    QDataStream out(&wav, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4);
    out << quint32(36 + data_bytes);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(2) << rate << quint32(rate * 4) << quint16(4)
        << quint16(16);
    out.writeRawData("data", 4);
    out << data_bytes;
    for (quint32 i = 0; i < frames; i++) {
        const auto sample = static_cast<qint16>(
            16384.0 * std::sin(2.0 * std::numbers::pi * 440.0 * i / static_cast<double>(rate)));
        out << sample << sample;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(wav) == wav.size();
}

/// The graph's object with this id, as pw-dump prints it; empty if there is none.
QJsonObject dump_object(uint32_t id) {
    QProcess dump;
    dump.start(QStringLiteral("pw-dump"), {QString::number(id)});
    if (!dump.waitForFinished(5000)) {
        return {};
    }
    const QJsonArray objects = QJsonDocument::fromJson(dump.readAllStandardOutput()).array();
    for (const QJsonValue& object : objects) {
        if (object[QLatin1String("id")].toInteger(-1) == id) {
            return object.toObject();
        }
    }
    return {};
}

QJsonObject node_props(const QJsonObject& node) {
    return node[QLatin1String("info")][QLatin1String("props")].toObject();
}

/// Not the app's node name: the session manager saves volume per name.
const QByteArray TEST_NODE = "sendspin-test";

bool near(float a, float b) {
    return std::abs(a - b) < 0.001F;
}

/// Sets volume and mute until they hold: the session manager's restore of the node's saved
/// volume lands shortly after start() and overrides an earlier set.
bool settle(SendspinOutput& output, float volume, bool muted) {
    return QTest::qWaitFor([&] {
        output.set_volume(volume);
        output.set_muted(muted);
        QTest::qWait(50);
        return near(output.volume(), volume) && output.muted() == muted;
    });
}

/// Where libpipewire would connect: with a socket there, a failed start() is a failure, not
/// a missing daemon.
bool daemon_socket_exists() {
    const QString remote = qEnvironmentVariable("PIPEWIRE_REMOTE", QStringLiteral("pipewire-0"));
    if (QDir::isAbsolutePath(remote)) {
        return QFileInfo::exists(remote);
    }
    const QString dir =
        qEnvironmentVariable("PIPEWIRE_RUNTIME_DIR", qEnvironmentVariable("XDG_RUNTIME_DIR"));
    return !dir.isEmpty() && QFileInfo::exists(dir + QLatin1Char('/') + remote);
}

bool have(const char* tool) {
    return !QStandardPaths::findExecutable(QString::fromLatin1(tool)).isEmpty();
}

}  // namespace

// Everything past silenceIsDetected() needs a PipeWire daemon and skips without one. Under
// ci/start-pipewire.sh there is always one, so there a skip would be a silent pass and fails.
#define START_OR_SKIP(output)                                                      \
    do {                                                                           \
        QString start_error;                                                       \
        if (!(output).start(&start_error)) {                                       \
            QVERIFY2(!daemon_socket_exists(), qPrintable(start_error));            \
            SKIP_UNLESS_REQUIRED(                                                  \
                qPrintable(QStringLiteral("no PipeWire daemon: ") + start_error)); \
        }                                                                          \
    } while (false)

// PipeWire 1.4.2 drops volume and mute on an idle node and never publishes the latency; fix
// pending. Remove with it.
#define EXPECT_FAIL_BEFORE_1_6(mode)                                              \
    do {                                                                          \
        if (QVersionNumber::fromString(QLatin1String(pw_get_library_version())) < \
            QVersionNumber(1, 6)) {                                               \
            QEXPECT_FAIL("", "PipeWire < 1.6, fix pending", mode);                \
        }                                                                         \
    } while (false)

#define NEED_TOOL(tool)                                     \
    do {                                                    \
        if (!have(tool)) {                                  \
            SKIP_UNLESS_REQUIRED(tool " is not installed"); \
        }                                                   \
    } while (false)

class SendspinOutputTest : public QObject {
    Q_OBJECT

private slots:
    void silenceIsDetected() {
        const float quiet[4] = {0.0F, -0.0F, 0.0F, 0.0F};
        const float loud[4] = {0.0F, 0.0F, 1e-6F, 0.0F};
        QVERIFY(is_silent(quiet, 4));
        QVERIFY(!is_silent(loud, 4));
        QVERIFY(is_silent(loud, 0));
    }

    void startFailsWithoutADaemon() {
        const QTemporaryDir empty;
        const QByteArray runtime_dir = qgetenv("XDG_RUNTIME_DIR");
        const QByteArray remote = qgetenv("PIPEWIRE_REMOTE");
        qputenv("XDG_RUNTIME_DIR", empty.path().toUtf8());
        qunsetenv("PIPEWIRE_REMOTE");

        SendspinOutput output(nullptr, TEST_NODE);
        QString error;
        const bool started = output.start(&error);

        qputenv("XDG_RUNTIME_DIR", runtime_dir);
        if (!remote.isNull()) {
            qputenv("PIPEWIRE_REMOTE", remote);
        }
        QVERIFY(!started);
        QVERIFY(!error.isEmpty());
        QCOMPARE(output.node_id(), PW_ID_ANY);
    }

    void nodeAppearsAsANetworkSinkAndGoesWithStop() {
        SendspinOutput output(nullptr, TEST_NODE);
        START_OR_SKIP(output);
        NEED_TOOL("pw-dump");

        const uint32_t id = output.node_id();
        QVERIFY(id != PW_ID_ANY);
        const QJsonObject props = node_props(dump_object(id));
        QCOMPARE(props[QLatin1String("media.class")].toString(), QStringLiteral("Audio/Sink"));
        QCOMPARE(props[QLatin1String("node.description")].toString(), QStringLiteral("Sendspin"));
        QCOMPARE(props[QLatin1String("node.network")].toVariant().toString(),
                 QStringLiteral("true"));
        QVERIFY(!props.contains(QLatin1String("node.virtual")));
        QVERIFY(!props.contains(QLatin1String("object.linger")));
        QCOMPARE(props[QLatin1String("priority.session")].toVariant().toInt(), 0);

        output.stop();
        QCOMPARE(output.node_id(), PW_ID_ANY);
        // The id may be reused, but not by this node
        QTRY_VERIFY(node_props(dump_object(id))[QLatin1String("node.name")].toString() !=
                    QString::fromLatin1(TEST_NODE));
    }

    void playedAudioArrivesTimestampedAtTheReportedRate() {
        Capture capture;
        SendspinOutput output(nullptr, TEST_NODE);
        const QSignalSpy rates(&output, &SendspinOutput::rateChanged);
        output.set_consumer([&capture](const CapturedBlock& block) { capture.take(block); });
        START_OR_SKIP(output);
        NEED_TOOL("pw-play");
        NEED_TOOL("pw-dump");
        QVERIFY(settle(output, 1.0F, false));

        const QTemporaryDir dir;
        const QString tone = dir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(write_tone(tone));
        const QString serial =
            node_props(dump_object(output.node_id()))[QLatin1String("object.serial")]
                .toVariant()
                .toString();
        QVERIFY(!serial.isEmpty());
        QCOMPARE(QProcess::execute(QStringLiteral("pw-play"),
                                   {QStringLiteral("--target"), serial, tone}),
                 0);

        // The tone, whole: pw-play drains before it exits
        const uint64_t tone_frames = SendspinOutput::RATE * TONE_MS / 1000;
        QTRY_VERIFY(capture.loud_frames.load() >= tone_frames - 1);
        // Blocks are whole quanta, so the tone's ends share theirs with silence
        QVERIFY(capture.loud_frames.load() <= tone_frames + 2 * 8192);
        QVERIFY(capture.blocks.load() > 1);
        QCOMPARE(capture.out_of_order.load(), 0U);
        QVERIFY2(capture.off_clock.load() == 0,
                 qPrintable(QStringLiteral("worst age %1 ns").arg(capture.worst_age_ns.load())));
        QVERIFY2(capture.worst_step_error_ns.load() < MAX_STEP_ERROR_NS,
                 qPrintable(QString::number(capture.worst_step_error_ns.load())));
        // Every timestamp came from the graph, none from the fallback
        QCOMPARE(output.untimed_blocks(), 0U);
        QCOMPARE(capture.wrong_rate.load(), 0U);
        // Half scale at full volume
        const double peak = static_cast<double>(capture.peak_millionths.load()) / 1e6;
        QVERIFY2(std::abs(peak - 0.5) < 0.01, qPrintable(QString::number(peak)));

        // The format is negotiated once, when the first player links
        QTRY_COMPARE(rates.count(), 1);
        QCOMPARE(rates.first().first().toUInt(), SendspinOutput::RATE);
        QCOMPARE(output.rate(), SendspinOutput::RATE);
        output.stop();
        QCOMPARE(output.rate(), 0U);
        QCOMPARE(rates.last().first().toUInt(), 0U);
    }

    void volumeAndMuteSurfaceAsSignals() {
        SendspinOutput output(nullptr, TEST_NODE);
        START_OR_SKIP(output);
        QVERIFY(settle(output, 1.0F, false));
        const QSignalSpy volumes(&output, &SendspinOutput::volumeChanged);
        const QSignalSpy mutes(&output, &SendspinOutput::mutedChanged);

        output.set_volume(0.5F);
        // Abort: mute is dropped the same way, so nothing past here holds either
        EXPECT_FAIL_BEFORE_1_6(Abort);
        QTRY_COMPARE(volumes.count(), 1);
        QVERIFY(near(volumes.first().first().toFloat(), 0.5F));
        QVERIFY(near(output.volume(), 0.5F));

        output.set_muted(true);
        QTRY_COMPARE(mutes.count(), 1);
        QVERIFY(output.muted());
        output.set_muted(false);
        QTRY_COMPARE(mutes.count(), 2);
        QVERIFY(!output.muted());

        QVERIFY(settle(output, 1.0F, false));
    }

    void volumeChangedElsewhereSurfacesAndScalesTheCapture() {
        Capture capture;
        SendspinOutput output(nullptr, TEST_NODE);
        output.set_consumer([&capture](const CapturedBlock& block) { capture.take(block); });
        START_OR_SKIP(output);
        NEED_TOOL("wpctl");
        NEED_TOOL("pw-play");
        NEED_TOOL("pw-dump");
        QVERIFY(settle(output, 1.0F, false));
        const QSignalSpy mutes(&output, &SendspinOutput::mutedChanged);
        const QString id = QString::number(output.node_id());

        QCOMPARE(QProcess::execute(QStringLiteral("wpctl"),
                                   {QStringLiteral("set-volume"), id, QStringLiteral("0.5")}),
                 0);
        // Abort: the capture and mute checks below rest on this volume having taken
        EXPECT_FAIL_BEFORE_1_6(Abort);
        QTRY_VERIFY(near(output.volume(), 0.5F));

        const QTemporaryDir dir;
        const QString tone = dir.filePath(QStringLiteral("tone.wav"));
        QVERIFY(write_tone(tone));
        const QString serial =
            node_props(dump_object(output.node_id()))[QLatin1String("object.serial")]
                .toVariant()
                .toString();
        QCOMPARE(QProcess::execute(QStringLiteral("pw-play"),
                                   {QStringLiteral("--target"), serial, tone}),
                 0);
        // Half scale at half volume: the gain is the volume cubed
        QTRY_VERIFY(capture.loud_frames.load() > 0);
        const double peak = static_cast<double>(capture.peak_millionths.load()) / 1e6;
        QVERIFY2(std::abs(peak - 0.0625) < 0.005, qPrintable(QString::number(peak)));

        QCOMPARE(QProcess::execute(QStringLiteral("wpctl"),
                                   {QStringLiteral("set-mute"), id, QStringLiteral("1")}),
                 0);
        QTRY_VERIFY(output.muted());
        QCOMPARE(mutes.count(), 1);

        QVERIFY(settle(output, 1.0F, false));
    }

    void latencyIsReportedToTheGraph() {
        SendspinOutput output(nullptr, TEST_NODE);
        START_OR_SKIP(output);
        NEED_TOOL("pw-dump");

        const auto reported_ns = [&output]() -> qint64 {
            const QJsonArray latencies =
                dump_object(output.node_id())[QLatin1String("info")][QLatin1String("params")]
                                             [QLatin1String("Latency")]
                                                 .toArray();
            for (const QJsonValue& latency : latencies) {
                if (latency[QLatin1String("direction")].toString() == QStringLiteral("Input")) {
                    return latency[QLatin1String("minNs")].toInteger();
                }
            }
            return -1;
        };

        output.set_latency(250000000);
        EXPECT_FAIL_BEFORE_1_6(Continue);
        QTRY_COMPARE(reported_ns(), 250000000);
        output.set_latency(400000000);
        EXPECT_FAIL_BEFORE_1_6(Continue);
        QTRY_COMPARE(reported_ns(), 400000000);
    }
};

QTEST_GUILESS_MAIN(SendspinOutputTest)
#include "sendspin_output_test.moc"
