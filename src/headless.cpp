#include "headless.h"

#include "audio/sendspin_output.h"
#include "cast/cast_engine.h"
#include "persistence.h"
#include "single_instance.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QSocketNotifier>
#include <QStandardPaths>
#include <QTextStream>

#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <csignal>
#include <string_view>

namespace {

int signal_fds[2] = {-1, -1};

void on_signal(int /*signal*/) {
    const char byte = 0;
    // Only async-signal-safe calls here; the notifier quits on the main thread
    (void)!::write(signal_fds[0], &byte, 1);
}

QString check_text(speaker_menu::Check check) {
    switch (check) {
        case speaker_menu::Check::FILLED:
            return QStringLiteral("[x]");
        case speaker_menu::Check::GREY:
            return QStringLiteral("[-]");
        case speaker_menu::Check::SPINNER:
            return QStringLiteral("[…]");
        default:
            return QStringLiteral("[ ]");
    }
}

void print(QTextStream& out, const CastEngine& engine, const SpeakerModel& model) {
    out << "Speakers" << (engine.playing() ? " (playing)" : "") << ":\n";
    for (const SpeakerRow& row : model.rows()) {
        out << "  " << check_text(row.row.check) << ' ' << row.name;
        if (row.row.status != speaker_menu::Status::NONE) {
            out << "  (" << SpeakerModel::status_text(row.row.status) << ')';
        }
        out << '\n';
    }
    if (model.rows().empty()) {
        out << "  none found yet\n";
    }
    out.flush();
}

}  // namespace

std::optional<int> run_headless(int argc, char** argv) {
    const bool asked = std::any_of(argv + 1, argv + argc, [](const char* arg) {
        const std::string_view text(arg);
        return text == "--list" || text.starts_with("--add") || text.starts_with("--remove");
    });
    if (!asked) {
        return std::nullopt;
    }

    QCoreApplication qt(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("sendspin"));
    QCommandLineParser parser;
    parser.addHelpOption();
    parser.addOptions({
        {"list", "Print the speakers as they change."},
        {"add", "Add the speaker with this name to the group.", "name"},
        {"remove", "Take the speaker with this name out of the group.", "name"},
    });
    parser.process(qt);

    QTextStream out(stdout);
    QTextStream err(stderr);
    // The app and this are the same Sendspin output and the same server
    SingleInstance instance(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation));
    if (!instance.claim()) {
        err << "Sendspin is already running; quit it first.\n";
        return 1;
    }

    SendspinOutput output;
    CastEngine engine(data_dir(), output);
    output.set_consumer([&engine](const CapturedBlock& block) { engine.feed().take(block); });
    QString error;
    if (!output.start(&error)) {
        err << "No Sendspin output: " << error << '\n';
    }
    if (!engine.start()) {
        err << "The server did not start.\n";
        return 1;
    }

    QStringList to_add = parser.values("add");
    QStringList to_remove = parser.values("remove");
    const SpeakerModel& model = *engine.speakers();
    // A speaker is found some time after the start, so each wish waits for its speaker
    const auto apply = [&] {
        for (const SpeakerRow& row : model.rows()) {
            if (to_add.removeAll(row.name) > 0 && !row.in_group) {
                out << "Adding " << row.name << '\n';
                QMetaObject::invokeMethod(
                    &engine, [&engine, id = row.id] { engine.add(id); }, Qt::QueuedConnection);
            }
            if (to_remove.removeAll(row.name) > 0 && row.in_group) {
                out << "Removing " << row.name << '\n';
                QMetaObject::invokeMethod(
                    &engine, [&engine, id = row.id] { engine.remove(id); }, Qt::QueuedConnection);
            }
        }
        if (parser.isSet("list")) {
            print(out, engine, model);
        }
    };
    QObject::connect(&model, &QAbstractItemModel::modelReset, &qt, apply);
    QObject::connect(&model, &QAbstractItemModel::dataChanged, &qt, apply);
    QObject::connect(&engine, &CastEngine::playingChanged, &qt, apply);
    QObject::connect(&engine, &CastEngine::pairingCodeNeeded, &qt, [&](const QString& id) {
        err << "This speaker needs pairing, which the headless mode can't do: " << id << '\n';
        err.flush();
        engine.cancelPairing(id);
    });
    apply();

    // Ctrl-C leaves through the destructors: the node goes and the speakers get their goodbye
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, signal_fds) == 0) {
        auto* notifier = new QSocketNotifier(signal_fds[1], QSocketNotifier::Read, &qt);
        QObject::connect(notifier, &QSocketNotifier::activated, &qt, &QCoreApplication::quit);
        std::signal(SIGINT, on_signal);
        std::signal(SIGTERM, on_signal);
    }
    return QCoreApplication::exec();
}
