// The metainfo's screenshots, drawn offscreen from the windows' own QML, as the tests load it.
// The picker is over the real engine and four stand-in speakers on the loopback interface, with
// a tone going into the Sendspin output; Settings is over made-up rows. Nothing is advertised
// or looked for, so no network is touched. Run by scripts/render-screenshots.sh.

#include "app.h"
#include "cast/test_peer.h"
#include "picker/picker.h"
#include "picker/picker_window.h"

#include <QApplication>
#include <QFile>
#include <QProcess>
#include <QSettings>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>

#include <cmath>
#include <cstdio>
#include <numbers>

using speaker_menu::Connection;

namespace {

constexpr int WAIT_MS = 15000;

int fail(const char* what) {
    std::fprintf(stderr, "render-screenshots: %s\n", what);
    return 1;
}

bool save(QQuickWindow* window, const QString& path) {
    // A frame or two for the icons and the layout to settle
    QTest::qWait(500);
    return window != nullptr && window->grabWindow().save(path);
}

/// A minute of a 440 Hz tone: 48 kHz, 16-bit, stereo
bool write_tone(const QString& path) {
    constexpr quint32 RATE = 48000;
    constexpr quint32 BYTES = RATE * 60 * 4;
    QByteArray wav;
    QDataStream out(&wav, QIODevice::WriteOnly);
    out.setByteOrder(QDataStream::LittleEndian);
    out.writeRawData("RIFF", 4);
    out << quint32{36 + BYTES};
    out.writeRawData("WAVEfmt ", 8);
    out << quint32{16} << quint16{1} << quint16{2} << RATE << quint32{RATE * 4} << quint16{4}
        << quint16{16};
    out.writeRawData("data", 4);
    out << BYTES;
    for (quint32 n = 0; n < RATE * 60; n++) {
        const auto sample =
            static_cast<qint16>(8000 * std::sin(2 * std::numbers::pi * 440 * n / RATE));
        out << sample << sample;
    }
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(wav) == wav.size();
}

SpeakerRow speaker(const QString& name, bool paired, const speaker_menu::Output& output) {
    SpeakerRow row;
    row.id = name;
    row.name = name;
    row.in_group = output.in_group;
    row.plays = output.plays;
    row.paired = paired;
    row.can_pair = !paired;
    row.can_play = speaker_menu::can_play(output);
    row.row = speaker_menu::row(output, {});
    return row;
}

/// The picker with sound going out: two speakers playing, one found, one that wants pairing
int render_picker(const QString& work, const QString& path) {
    const std::string data = (work + "/picker").toStdString();
    const uint16_t ports[] = {free_port(), free_port(), free_port(), free_port()};
    Peer living_room(work + "/living-room", ports[0], true, "Living Room");
    Peer kitchen(work + "/kitchen", ports[1], true, "Kitchen");
    const Peer bedroom(work + "/bedroom", ports[2], true, "Bedroom");
    const Peer office(work + "/office", ports[3], false, "Office");

    QStringList urls;
    for (const uint16_t port : ports) {
        urls.append(QStringLiteral("ws://127.0.0.1:%1/sendspin").arg(port));
    }
    {
        QSettings settings(work + "/picker/cast.ini", QSettings::IniFormat);
        settings.setValue("speaker_urls", urls);
    }

    ClientPersistence persistence(data);
    Outputs outputs;
    Client client({.name = "Study PC", .port = Client::free_port(), .advertise = false},
                  &persistence);
    SendspinOutput output;
    CastEngine cast(data, output);
    Picker picker(cast, client, outputs);
    PickerWindow window(picker, cast);
    // As the app does: what the Sendspin output captures goes to the speakers
    output.set_consumer([&cast](const CapturedBlock& block) { cast.feed().take(block); });
    if (!output.start() || !outputs.start() ||
        !QTest::qWaitFor([&] { return outputs.local().has_value(); }, WAIT_MS)) {
        return fail("no PipeWire: run it through scripts/render-screenshots.sh");
    }
    if (!cast.start(free_port(), false) ||
        !QTest::qWaitFor([&] { return cast.speakers()->rows().size() == 4; }, WAIT_MS)) {
        return fail("the stand-in speakers did not connect");
    }
    picker.toggle(living_room.id());
    picker.toggle(kitchen.id());

    QProcess tone;
    if (!write_tone(work + "/tone.wav")) {
        return fail("no tone to play");
    }
    tone.start("pw-play", {"--target", "sendspin", work + "/tone.wav"});
    const bool plays =
        QTest::qWaitFor([&] { return picker.dot() == Picker::Dot::Plays; }, WAIT_MS) &&
        QTest::qWaitFor(
            [&] {
                return std::ranges::count(cast.speakers()->rows(), true, &SpeakerRow::plays) == 2;
            },
            WAIT_MS);
    if (plays) {
        window.show();
    }
    const bool saved = plays && save(window.window(), path);
    tone.kill();
    tone.waitForFinished();
    return saved ? 0 : fail("the picker never showed Sendspin playing");
}

/// Settings on its Speakers section, no taller than what it shows
int render_settings(const QString& work, const QString& path) {
    App app((work + "/settings").toStdString());
    app.cast()->speakers()->set_rows({
        speaker("Living Room", true, {.in_group = true, .plays = true}),
        speaker("Kitchen", true, {.in_group = true, .plays = true}),
        speaker("Bedroom", true, {.connection = Connection::WAITING}),
        speaker("Office", false, {.connection = Connection::NEEDS_PAIRING}),
    });
    app.showSettings();
    QQuickWindow* window = app.settings_window();
    if (window == nullptr) {
        return fail("Settings did not load");
    }
    window->setProperty("section", 1);
    window->setHeight(window->minimumHeight());
    return save(window, path) ? 0 : fail("no picture of Settings");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fputs("usage: render-screenshots <directory>\n", stderr);
        return 2;
    }
    const QString out = QString::fromLocal8Bit(argv[1]);

    QApplication qt(argc, argv);
    QApplication::setApplicationName(QStringLiteral("sendspin"));
    QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);

    // Nobody's settings, and nobody's hostname
    const QTemporaryDir dir;
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, dir.path() + "/config");
    QSettings(QStringLiteral("sendspin"), QStringLiteral("sendspin"))
        .setValue("client/name", QStringLiteral("Study PC"));

    if (const int failed = render_picker(dir.path(), out + "/picker.png")) {
        return failed;
    }
    return render_settings(dir.path(), out + "/settings.png");
}
