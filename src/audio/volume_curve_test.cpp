#include "audio/volume_curve.h"

#include <QTest>

class VolumeCurveTest : public QObject {
    Q_OBJECT

private slots:
    void followsTheSpecsCurve() {
        QCOMPARE(volume_to_amplitude(0), 0.0F);
        QCOMPARE(volume_to_amplitude(100), 1.0F);
        // (50 / 100)^1.5
        QCOMPARE(volume_to_amplitude(50), 0.35355338F);
        QCOMPARE(volume_to_amplitude(25), 0.125F);
        QCOMPARE(volume_to_amplitude(200), 1.0F);
    }

    void everyVolumeRoundTrips() {
        for (int volume = 0; volume <= 100; ++volume) {
            QCOMPARE(amplitude_to_volume(volume_to_amplitude(static_cast<uint8_t>(volume))),
                     volume);
        }
    }

    void gainsOutsideTheRangeClamp() {
        QCOMPARE(amplitude_to_volume(-0.5F), 0);
        // The OS mixer goes past 100 %
        QCOMPARE(amplitude_to_volume(1.5F), 100);
        QCOMPARE(amplitude_to_volume(0.125F), 25);
    }
};

QTEST_APPLESS_MAIN(VolumeCurveTest)
#include "volume_curve_test.moc"
