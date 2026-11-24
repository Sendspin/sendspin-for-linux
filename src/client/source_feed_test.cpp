#include "source_feed.h"

#include <QTest>

#include <array>
#include <cmath>
#include <vector>

class SourceFeedTest : public QObject {
    Q_OBJECT

private slots:
    void streamsFlac16AtTheOutputsFormat() {
        const sendspin::SourceRoleConfig config = SourceFeed::config();
        QCOMPARE(config.codec, sendspin::SendspinCodecFormat::FLAC);
        QCOMPARE(config.sample_rate, SendspinOutput::RATE);
        QCOMPARE(uint32_t{config.channels}, SendspinOutput::CHANNELS);
        QCOMPARE(int{config.bit_depth}, 16);
        QCOMPARE(config.chunk_duration_ms, 20U);
        QVERIFY(config.line_sense);
    }

    void silenceStaysSilent() {
        const std::array<float, 64> in{};
        std::array<int16_t, 64> out;
        out.fill(1);
        uint32_t state = 1;
        to_s16_dithered(in.data(), out.data(), in.size(), state);
        for (const int16_t sample : out) {
            QCOMPARE(sample, int16_t{0});
        }
    }

    void ditherStaysWithinOneStep() {
        std::vector<float> in(4800);
        for (size_t i = 0; i < in.size(); ++i) {
            in[i] = 0.5F * std::sin(static_cast<float>(i) * 0.05F);
        }
        std::vector<int16_t> out(in.size());
        uint32_t state = 1;
        to_s16_dithered(in.data(), out.data(), in.size(), state);

        bool dithered = false;
        for (size_t i = 0; i < in.size(); ++i) {
            const float exact = in[i] * 32768.0F;
            QVERIFY(std::abs(static_cast<float>(out[i]) - exact) <= 1.5F);
            dithered = dithered || out[i] != static_cast<int16_t>(std::lround(exact));
        }
        QVERIFY(dithered);
    }

    void fullScaleClamps() {
        const std::array<float, 4> in{1.0F, -1.0F, 2.0F, -2.0F};
        std::array<int16_t, 4> out{};
        uint32_t state = 1;
        to_s16_dithered(in.data(), out.data(), in.size(), state);
        QVERIFY(out[0] >= 32766);
        QVERIFY(out[1] <= -32767);
        QCOMPARE(out[2], int16_t{32767});
        QCOMPARE(out[3], int16_t{-32768});
    }
};

QTEST_APPLESS_MAIN(SourceFeedTest)
#include "source_feed_test.moc"
