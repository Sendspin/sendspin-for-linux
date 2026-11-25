#include "app.h"
#include "app_id.h"
#include "headless.h"
#include "single_instance.h"

#include <QApplication>
#include <QStandardPaths>

#include <cstdio>
#include <string_view>

int main(int argc, char** argv) {
    // Only the tray: how a login starts it
    bool background = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--version") {
            std::puts(SENDSPIN_VERSION);
            return 0;
        }
        background = background || arg == "--background";
    }
    // Before QApplication: the headless mode needs no display
    if (const std::optional<int> code = run_headless(argc, argv)) {
        return *code;
    }

    QApplication qt(argc, argv);
    QApplication::setApplicationName(QStringLiteral("sendspin"));
    QApplication::setApplicationDisplayName(QStringLiteral("Sendspin"));
    QApplication::setApplicationVersion(QStringLiteral(SENDSPIN_VERSION));
    // Ties the windows to the launcher's entry
    QApplication::setDesktopFileName(QLatin1String(APP_ID));
    // A tray app: no window is not a reason to exit
    QApplication::setQuitOnLastWindowClosed(false);

    SingleInstance instance(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation));
    // Unset by Qt once the first window has used it
    const QByteArray activation_token = qgetenv("XDG_ACTIVATION_TOKEN");
    if (!instance.claim(activation_token)) {
        return 0;
    }

    App app(data_dir());
    QObject::connect(&instance, &SingleInstance::secondLaunch, &app, &App::showPicker);
    app.start();
    // Without a tray (stock GNOME) the launcher is the only way in
    if (!background) {
        app.showPicker();
    }
    return QApplication::exec();
}
