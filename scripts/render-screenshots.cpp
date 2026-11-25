// The metainfo's screenshots, drawn offscreen: the picker and Settings as the tests load them,
// over rows that are made up. Nothing is started but PipeWire's listing of this PC's output,
// so no speaker is dialled and no network is touched. Run by scripts/render-screenshots.sh.

#include "app.h"
#include "picker/picker.h"
#include "picker/picker_window.h"

#include <QApplication>
#include <QSettings>
#include <QStyleHints>
#include <QTemporaryDir>
#include <QTest>

#include <cstdio>

using speaker_menu::Connection;

namespace {

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

/// One of each: playing, connecting, found, and found but not paired
std::vector<SpeakerRow> speakers() {
    return {
        speaker("Living Room", true,
                {.in_group = true, .plays = true, .connection = Connection::CONNECTED}),
        speaker("Kitchen", true, {.in_group = true, .connection = Connection::CONNECTING}),
        speaker("Bedroom", true, {.connection = Connection::WAITING}),
        speaker("Office", false, {.connection = Connection::NEEDS_PAIRING}),
    };
}

bool save(QQuickWindow* window, const QString& path) {
    // A frame or two for the icons and the layout to settle
    QTest::qWait(500);
    if (window == nullptr || !window->grabWindow().save(path)) {
        std::fprintf(stderr, "render-screenshots: no %s\n", qPrintable(path));
        return false;
    }
    return true;
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
    const std::string data = (dir.path() + "/data").toStdString();

    {
        ClientPersistence persistence(data);
        Outputs outputs;
        Client client({.name = "Study PC", .port = Client::free_port(), .advertise = false},
                      &persistence);
        SendspinOutput output;
        CastEngine cast(data, output);
        Picker picker(cast, client, outputs);
        PickerWindow window(picker, cast);
        // Without its output listed the picker has a notice in place of its rows
        if (!outputs.start() || !QTest::qWaitFor([&] { return outputs.local().has_value(); })) {
            std::fputs("render-screenshots: no PipeWire output to list\n", stderr);
            return 1;
        }
        cast.speakers()->set_rows(speakers());
        window.show();
        if (!save(window.window(), out + "/picker.png")) {
            return 1;
        }
    }

    App app(data);
    app.cast()->speakers()->set_rows(speakers());
    app.showSettings();
    QQuickWindow* settings = app.settings_window();
    if (settings != nullptr) {
        // Speakers
        settings->setProperty("section", 1);
    }
    return save(settings, out + "/settings.png") ? 0 : 1;
}
