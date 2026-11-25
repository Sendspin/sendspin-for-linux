#include "audio/player_formats.h"

#include <QTest>

using sendspin::SendspinCodecFormat;

namespace {

struct Format {
    SendspinCodecFormat codec;
    uint32_t rate;
    int bits;
    bool operator==(const Format&) const = default;
};

std::vector<Format> advertised(const SinkFormats& sink, const ClockRates& clock) {
    std::vector<Format> result;
    for (const auto& format : player_formats(sink, clock)) {
        if (format.channels != 2) {
            return {};
        }
        result.push_back({format.codec, format.sample_rate, format.bit_depth});
    }
    return result;
}

constexpr auto FLAC = SendspinCodecFormat::FLAC;
constexpr auto PCM = SendspinCodecFormat::PCM;

}  // namespace

class PlayerFormatsTest : public QObject {
    Q_OBJECT

private slots:
    void parsesAllowedRates() {
        QCOMPARE(parse_rates("[ 44100 48000 ]"), (std::vector<uint32_t>{44100, 48000}));
        QCOMPARE(parse_rates("[48000,96000]"), (std::vector<uint32_t>{48000, 96000}));
        QCOMPARE(parse_rates("[ ]"), std::vector<uint32_t>{});
        QCOMPARE(parse_rates(""), std::vector<uint32_t>{});
    }

    // PipeWire as installed: one allowed rate, a sound card that would take anything
    void aStockDaemonPlaysOnlyItsOwnRate() {
        const SinkFormats sink{.rate_min = 8000, .rate_max = 384000, .over_16_bit = true};
        const ClockRates clock{.rate = 48000, .allowed = {48000}};
        QCOMPARE(advertised(sink, clock),
                 (std::vector<Format>{
                     {FLAC, 48000, 24}, {FLAC, 48000, 16}, {PCM, 48000, 24}, {PCM, 48000, 16}}));
    }

    void onlyRatesBothSidesHaveAreAdvertised() {
        const SinkFormats sink{.rates = {44100, 48000}};
        const ClockRates clock{.rate = 48000, .allowed = {96000, 44100, 48000}};
        QCOMPARE(native_rates(sink, clock), (std::vector<uint32_t>{48000, 44100}));
        QCOMPARE(advertised(sink, clock),
                 (std::vector<Format>{
                     {FLAC, 48000, 16}, {FLAC, 44100, 16}, {PCM, 48000, 16}, {PCM, 44100, 16}}));
    }

    void theGraphsRateComesFirst() {
        const SinkFormats sink{.rate_min = 8000, .rate_max = 192000};
        const ClockRates clock{.rate = 96000, .allowed = {44100, 48000, 96000, 192000}};
        QCOMPARE(native_rates(sink, clock), (std::vector<uint32_t>{96000, 44100, 48000, 192000}));
    }

    void aForcedRateIsTheOnlyOne() {
        const SinkFormats sink{.rate_min = 8000, .rate_max = 192000};
        const ClockRates clock{.rate = 48000, .allowed = {44100, 48000}, .forced = 44100};
        QCOMPARE(native_rates(sink, clock), std::vector<uint32_t>{44100});
    }

    // A Bluetooth output at 44.1 kHz under a daemon that only runs at 48: resampled either way
    void nothingInCommonFallsBackToTheGraphsRate() {
        const SinkFormats sink{.rates = {44100}};
        const ClockRates clock{.rate = 48000, .allowed = {48000}};
        QCOMPARE(native_rates(sink, clock), std::vector<uint32_t>{48000});
    }

    void anOutputThatCouldNotBeAskedLimitsNothing() {
        const ClockRates clock{.rate = 48000, .allowed = {44100, 48000}};
        QCOMPARE(native_rates({}, clock), (std::vector<uint32_t>{48000, 44100}));
        // No daemon at all
        QCOMPARE(advertised({}, {}), (std::vector<Format>{{FLAC, 48000, 16}, {PCM, 48000, 16}}));
    }
};

QTEST_APPLESS_MAIN(PlayerFormatsTest)
#include "player_formats_test.moc"
