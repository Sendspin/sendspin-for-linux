#include "audio/drift_corrector.h"

#include <QTest>

#include <cmath>

namespace {

constexpr uint32_t RATE = 48000;
constexpr uint32_t CYCLE = 1024;
constexpr uint32_t QUEUED = 48000;
constexpr int64_t US = 1000;
using Step = DriftCorrector::Step;

/// A corrector past its first resync, on time
DriftCorrector started() {
    DriftCorrector corrector(RATE);
    corrector.next(0, CYCLE, QUEUED);
    return corrector;
}

}  // namespace

class DriftCorrectorTest : public QObject {
    Q_OBJECT

private slots:
    // --- The one-shot resync: at the start, after an underrun, past 1 ms ---

    void startsByDroppingWhatIsLate() {
        DriftCorrector corrector(RATE);
        QVERIFY(corrector.resyncing());
        // 10 ms late is 480 frames
        QCOMPARE(corrector.next(10'000 * US, CYCLE, QUEUED), (Step{.drop = 480}));
        QVERIFY(!corrector.resyncing());
        QCOMPARE(corrector.counters().resyncs, 1U);
    }

    void startsByWaitingWhenItIsEarly() {
        DriftCorrector corrector(RATE);
        QCOMPARE(corrector.next(-10'000 * US, CYCLE, QUEUED), (Step{.silence = 480}));
        QVERIFY(!corrector.resyncing());
    }

    void earlyByMoreThanACycleItKeepsWaiting() {
        DriftCorrector corrector(RATE);
        QCOMPARE(corrector.next(-100'000 * US, CYCLE, QUEUED), (Step{.silence = CYCLE}));
        QVERIFY(corrector.resyncing());
        QCOMPARE(corrector.counters().resyncs, 0U);
        // The rest of the wait, then the audio
        QCOMPARE(corrector.next(-5'000 * US, CYCLE, QUEUED), (Step{.silence = 240}));
        QVERIFY(!corrector.resyncing());
    }

    void withNothingInTimeItTriesAgain() {
        DriftCorrector corrector(RATE);
        // All 100 queued frames are late
        QCOMPARE(corrector.next(10'000 * US, CYCLE, 100), (Step{.drop = 100}));
        QVERIFY(corrector.resyncing());
    }

    void nothingQueuedIsNothingToDo() {
        DriftCorrector corrector(RATE);
        QCOMPARE(corrector.next(10'000 * US, CYCLE, 0), Step{});
        QVERIFY(corrector.resyncing());
    }

    void anUnderrunResyncs() {
        DriftCorrector corrector = started();
        corrector.resync();
        QVERIFY(corrector.resyncing());
        QCOMPARE(corrector.next(2'000 * US, CYCLE, QUEUED), (Step{.drop = 96}));
        QCOMPARE(corrector.counters().resyncs, 2U);
    }

    void pastOneMillisecondItResyncs() {
        DriftCorrector corrector = started();
        // The smoothed error takes a few cycles to get there; then one shot puts it right
        Step step;
        int cycles = 0;
        while (corrector.counters().resyncs == 1 && cycles < 100) {
            step = corrector.next(3'000 * US, CYCLE, QUEUED);
            cycles++;
        }
        QCOMPARE(corrector.counters().resyncs, 2U);
        QCOMPARE(step.drop, 144U);
        QVERIFY(cycles > 1);
        QCOMPARE(corrector.smoothed_error_ns(), 0);
    }

    void oneJitteryCycleDoesNotResync() {
        DriftCorrector corrector = started();
        corrector.next(5'000 * US, CYCLE, QUEUED);
        for (int i = 0; i < 100; i++) {
            corrector.next(0, CYCLE, QUEUED);
        }
        QCOMPARE(corrector.counters().resyncs, 1U);
    }

    // --- The dead band ---

    void insideTheDeadBandNothingIsDone() {
        DriftCorrector corrector = started();
        for (const int64_t error_us : {90, -90, 50, 0}) {
            for (int i = 0; i < 200; i++) {
                QCOMPARE(corrector.next(error_us * US, CYCLE, QUEUED), Step{});
            }
        }
        QCOMPARE(corrector.counters().dropped, 0U);
        QCOMPARE(corrector.counters().repeated, 0U);
    }

    void lateOutsideTheDeadBandItDropsSingleFrames() {
        DriftCorrector corrector = started();
        uint32_t dropped = 0;
        for (int i = 0; i < 200; i++) {
            const Step step = corrector.next(300 * US, CYCLE, QUEUED);
            QVERIFY(step.drop <= 1);
            QVERIFY(!step.repeat);
            QCOMPARE(step.silence, 0U);
            dropped += step.drop;
        }
        QVERIFY(dropped > 0);
        QCOMPARE(corrector.counters().dropped, dropped);
        QCOMPARE(corrector.counters().resyncs, 1U);
    }

    void earlyOutsideTheDeadBandItRepeatsSingleFrames() {
        DriftCorrector corrector = started();
        uint32_t repeated = 0;
        for (int i = 0; i < 200; i++) {
            const Step step = corrector.next(-300 * US, CYCLE, QUEUED);
            QCOMPARE(step.drop, 0U);
            repeated += step.repeat ? 1 : 0;
        }
        QVERIFY(repeated > 0);
        QCOMPARE(corrector.counters().repeated, repeated);
    }

    // What a correction took off the error is known at once, not after the smoothing catches up
    void aCorrectionComesOffTheError() {
        DriftCorrector corrector = started();
        while (corrector.counters().dropped == 0) {
            corrector.next(150 * US, CYCLE, QUEUED);
        }
        const int64_t after = corrector.smoothed_error_ns();
        QVERIFY(after > 100 * US - 20'834);
        QVERIFY(after <= 150 * US - 20'833);
    }

    // --- The most it corrects: one frame in 200 ---

    void itCorrectsAtMostHalfAPercentOfTheFrames_data() {
        QTest::addColumn<uint32_t>("cycle");
        QTest::newRow("a small cycle") << 64U;
        QTest::newRow("200 frames") << 200U;
        QTest::newRow("a usual cycle") << 1024U;
        QTest::newRow("a large cycle") << 8192U;
    }

    void itCorrectsAtMostHalfAPercentOfTheFrames() {
        QFETCH(uint32_t, cycle);
        DriftCorrector corrector = started();
        uint64_t frames = 0;
        uint64_t since = DriftCorrector::CORRECTION_SPACING;
        // Late enough to want a correction every cycle, and not enough to resync
        for (int i = 0; i < 2000; i++) {
            const Step step = corrector.next(900 * US, cycle, QUEUED);
            if (step.drop != 0) {
                QVERIFY2(since >= DriftCorrector::CORRECTION_SPACING,
                         qPrintable(QString::number(since)));
                since = 0;
            }
            since += cycle;
            frames += cycle;
        }
        const uint32_t corrected = corrector.counters().dropped;
        QVERIFY(corrected > 0);
        QVERIFY2(uint64_t{corrected} * 200 <= frames, qPrintable(QString::number(corrected)));
        QCOMPARE(corrector.counters().resyncs, 1U);
    }

    // A clock 100 ppm fast is followed without ever resyncing
    void itFollowsADriftingClock() {
        DriftCorrector corrector = started();
        double error_ns = 0;
        const double frame_ns = 1e9 / RATE;
        for (int i = 0; i < 20000; i++) {
            const Step step = corrector.next(static_cast<int64_t>(error_ns), CYCLE, QUEUED);
            error_ns += CYCLE * frame_ns * 100e-6;
            error_ns -= step.drop * frame_ns;
            error_ns += step.repeat ? frame_ns : 0;
            QVERIFY(std::abs(error_ns) < 250'000);
        }
        QCOMPARE(corrector.counters().resyncs, 1U);
        QVERIFY(corrector.counters().dropped > 0);
        QCOMPARE(corrector.counters().repeated, 0U);
    }
};

QTEST_APPLESS_MAIN(DriftCorrectorTest)
#include "drift_corrector_test.moc"
