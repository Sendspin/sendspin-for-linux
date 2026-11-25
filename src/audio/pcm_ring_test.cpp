#include "audio/pcm_ring.h"

#include <QTest>

#include <array>
#include <numeric>

class PcmRingTest : public QObject {
    Q_OBJECT

private slots:
    void readsBackWhatWasWrittenAcrossTheWrap() {
        PcmRing ring;
        ring.reset(8 + 1);
        std::array<uint8_t, 6> in{};
        std::array<uint8_t, 6> out{};
        for (uint8_t round = 0; round < 5; ++round) {
            std::iota(in.begin(), in.end(), static_cast<uint8_t>(round * 10));
            QCOMPARE(ring.write(in.data(), in.size()), in.size());
            QCOMPARE(ring.read(out.data(), out.size()), out.size());
            QCOMPARE(out, in);
        }
    }

    void aFullRingTakesNoMore() {
        PcmRing ring;
        ring.reset(4 + 1);
        const std::array<uint8_t, 6> in{1, 2, 3, 4, 5, 6};
        QCOMPARE(ring.free_space(), size_t{4});
        QCOMPARE(ring.write(in.data(), in.size()), size_t{4});
        QCOMPARE(ring.write(in.data(), in.size()), size_t{0});
        QCOMPARE(ring.available(), size_t{4});
    }

    void aShortReadIsPaddedWithSilence() {
        PcmRing ring;
        ring.reset(8 + 1);
        const std::array<uint8_t, 2> in{7, 9};
        ring.write(in.data(), in.size());
        std::array<uint8_t, 4> out{1, 1, 1, 1};
        QCOMPARE(ring.read(out.data(), out.size()), size_t{2});
        QCOMPARE(out, (std::array<uint8_t, 4>{7, 9, 0, 0}));
    }

    void aRequestedClearEmptiesItOnTheNextRead() {
        PcmRing ring;
        ring.reset(8 + 1);
        const std::array<uint8_t, 3> in{1, 2, 3};
        ring.write(in.data(), in.size());
        ring.request_clear();
        std::array<uint8_t, 3> out{9, 9, 9};
        QCOMPARE(ring.read(out.data(), out.size()), size_t{0});
        QCOMPARE(out, (std::array<uint8_t, 3>{0, 0, 0}));
        QCOMPARE(ring.available(), size_t{0});
    }
};

QTEST_APPLESS_MAIN(PcmRingTest)
#include "pcm_ring_test.moc"
