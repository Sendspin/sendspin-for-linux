#include "audio/playback_stream.h"

#include "audio/pipewire_util.h"
#include "audio/sendspin_output.h"
#include "audio/volume_curve.h"
#include "skip_unless_required.h"

#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include <pipewire/extensions/metadata.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <optional>
#include <vector>

namespace {

const QByteArray SINK = "sendspin-test-playback-sink";
constexpr PlaybackStream::Format FORMAT{48000, 2, 16};
constexpr size_t CHUNK_FRAMES = 480;
/// Both stamps come from the same graph cycle; 2 ms was one mislaid 96-frame term
constexpr int64_t MAX_HEARD_ERROR_US = 250;

/// What the stream reported played, and what reached the output, without locks
struct Heard {
    std::atomic<uint64_t> played_frames{0};
    std::atomic<uint64_t> reports{0};
    std::atomic<uint64_t> out_of_range{0};
    std::atomic<uint64_t> loud_frames{0};
    std::atomic<int64_t> peak_millionths{0};
    /// When the first frame played was to be heard, and when the first loud frame arrived, in
    /// microseconds on the libraries' clock; 0 for not yet
    std::atomic<int64_t> first_heard_us{0};
    std::atomic<int64_t> first_loud_us{0};

    void played(uint32_t frames, int64_t finish_us) {
        if (this->played_frames.load() == 0) {
            this->first_heard_us.store(finish_us - (int64_t{frames} * 1'000'000 / FORMAT.rate));
        }
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
        float peak = 0.0F;
        for (size_t i = 0; i < size_t{block.frames} * SendspinOutput::CHANNELS; ++i) {
            peak = std::max(peak, std::abs(block.samples[i]));
            if (this->first_loud_us.load() == 0 && peak > 0.001F) {
                const auto frame = static_cast<int64_t>(i / SendspinOutput::CHANNELS);
                this->first_loud_us.store((block.capture_time_ns / 1000) +
                                          (frame * 1'000'000 / block.rate));
            }
        }
        this->loud_frames += block.frames;
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

/// What the session manager's registry showed: its "default" metadata, and an output's serial
struct Lookup {
    pw_thread_loop* loop{nullptr};
    QByteArray sink;
    uint32_t metadata_id{PW_ID_ANY};
    QByteArray serial;
    int seq{0};
    bool done{false};
};

void lookup_global(void* data, uint32_t id, uint32_t /*permissions*/, const char* type,
                   uint32_t /*version*/, const spa_dict* props) {
    auto* lookup = static_cast<Lookup*>(data);
    const char* metadata = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
    const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    const char* serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
    if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && metadata != nullptr &&
        std::strcmp(metadata, "default") == 0) {
        lookup->metadata_id = id;
    } else if (name != nullptr && serial != nullptr && lookup->sink == name) {
        lookup->serial = serial;
    }
}

void lookup_done(void* data, uint32_t /*id*/, int seq) {
    auto* lookup = static_cast<Lookup*>(data);
    if (seq == lookup->seq) {
        lookup->done = true;
        pw_thread_loop_signal(lookup->loop, false);
    }
}

/// Asks the session manager to move a node to an output, as a mixer's "move this stream" does;
/// an empty sink takes the request back.
bool set_target_metadata(uint32_t node, const QByteArray& sink) {
    static constexpr pw_registry_events registry_events = [] {
        pw_registry_events table{};
        table.version = PW_VERSION_REGISTRY_EVENTS;
        table.global = lookup_global;
        return table;
    }();
    static constexpr pw_core_events core_events = [] {
        pw_core_events table{};
        table.version = PW_VERSION_CORE_EVENTS;
        table.done = lookup_done;
        return table;
    }();

    Lookup lookup;
    lookup.sink = sink;
    lookup.loop = pw_thread_loop_new("test-move", nullptr);
    pw_thread_loop_start(lookup.loop);
    bool moved = false;
    {
        const LoopLock lock(lookup.loop);
        const auto round_trip = [&lookup](pw_core* core) {
            lookup.done = false;
            lookup.seq = pw_core_sync(core, PW_ID_CORE, 0);
            timespec deadline{};
            pw_thread_loop_get_time(lookup.loop, &deadline, int64_t{2} * 1000 * 1000 * 1000);
            while (!lookup.done && pw_thread_loop_timed_wait_full(lookup.loop, &deadline) == 0) {}
        };
        pw_context* context = pw_context_new(pw_thread_loop_get_loop(lookup.loop), nullptr, 0);
        pw_core* core = pw_context_connect(context, nullptr, 0);
        if (core != nullptr) {
            spa_hook core_hook{};
            spa_hook registry_hook{};
            pw_core_add_listener(core, &core_hook, &core_events, &lookup);
            pw_registry* registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
            pw_registry_add_listener(registry, &registry_hook, &registry_events, &lookup);
            round_trip(core);
            if (lookup.metadata_id != PW_ID_ANY && (sink.isEmpty() || !lookup.serial.isEmpty())) {
                auto* metadata = static_cast<pw_metadata*>(
                    pw_registry_bind(registry, lookup.metadata_id, PW_TYPE_INTERFACE_Metadata,
                                     PW_VERSION_METADATA, 0));
                pw_metadata_set_property(metadata, node, "target.object",
                                         sink.isEmpty() ? nullptr : "Spa:Id",
                                         sink.isEmpty() ? nullptr : lookup.serial.constData());
                round_trip(core);
                moved = lookup.done;
                pw_proxy_destroy(reinterpret_cast<pw_proxy*>(metadata));
            }
            spa_hook_remove(&registry_hook);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
            spa_hook_remove(&core_hook);
            pw_core_disconnect(core);
        }
        pw_context_destroy(context);
    }
    pw_thread_loop_destroy(lookup.loop);
    return moved;
}

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
            SKIP_UNLESS_REQUIRED("no PipeWire daemon");
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

        // Sync rests on this: the first frame was heard when the stream said it would be, by
        // the output's own stamp of the graph cycle that delivered it
        QVERIFY(this->heard_.first_loud_us.load() != 0);
        const int64_t error_us =
            this->heard_.first_heard_us.load() - this->heard_.first_loud_us.load();
        qInfo("first frame heard %lld us from when it was reported",
              static_cast<long long>(error_us));
        QVERIFY2(std::abs(error_us) < MAX_HEARD_ERROR_US, qPrintable(QString::number(error_us)));
    }

    void theVolumeIsTheStreamsOwnBothWays() {
        if (!this->live_) {
            SKIP_UNLESS_REQUIRED("no PipeWire daemon");
        }
        if (QStandardPaths::findExecutable(QStringLiteral("wpctl")).isEmpty()) {
            SKIP_UNLESS_REQUIRED("wpctl is not installed");
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

        // The OS mixer moves it; wpctl's scale is the cube root of the gain
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

        // The mixer right behind a set from here is still heard of
        stream.set_gain(0.3F);
        QCOMPARE(QProcess::execute(QStringLiteral("wpctl"), {"set-volume", id, "0.5"}), 0);
        QTRY_COMPARE(gains.size(), 2);
        QVERIFY(std::abs(gains.last().first().toFloat() - 0.125F) < 0.001F);
        QCOMPARE(stream.gain(), gains.last().first().toFloat());
    }

    // Loop guard: a mixer's "move this stream" is how it would end up in the Sendspin output
    void aMixerCannotMoveItToAnotherOutput() {
        if (!this->live_) {
            SKIP_UNLESS_REQUIRED("no PipeWire daemon");
        }
        const QByteArray other_name = "sendspin-test-playback-other";
        SendspinOutput other(nullptr, other_name);
        Heard elsewhere;
        other.set_consumer([&elsewhere](const CapturedBlock& block) { elsewhere.captured(block); });
        QVERIFY(other.start());

        PlaybackStream stream(nullptr, "sendspin-test-player");
        stream.set_target(SINK);
        play(stream, 300);
        const uint32_t node = stream.node_id();
        if (!set_target_metadata(node, other_name)) {
            SKIP_UNLESS_REQUIRED("no session manager metadata to move a stream with");
        }
        QTest::qWait(300);
        this->heard_.reset_peak();
        play(stream, 500);
        QTRY_VERIFY(this->heard_.loud_frames.load() > 10000);
        QCOMPARE(elsewhere.loud_frames.load(), uint64_t{0});
        QCOMPARE(stream.node_id(), node);
        set_target_metadata(node, {});
    }

    void movesToANewTargetAndKeepsCount() {
        if (!this->live_) {
            SKIP_UNLESS_REQUIRED("no PipeWire daemon");
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
