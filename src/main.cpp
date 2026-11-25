#include "app.h"
#include "app_id.h"
#include "headless.h"
#include "single_instance.h"

#include <QApplication>
#include <QStandardPaths>

int main(int argc, char** argv) {
    // Before QApplication: the headless mode needs no display
    if (const std::optional<int> code = run_headless(argc, argv)) {
        return *code;
    }

    QApplication qt(argc, argv);
    QApplication::setApplicationName(QStringLiteral("sendspin"));
    QApplication::setApplicationDisplayName(QStringLiteral("Sendspin"));
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
    app.showPicker();
    return QApplication::exec();
}
