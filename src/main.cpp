#include "app.h"
#include "single_instance.h"

#include <QApplication>
#include <QStandardPaths>

int main(int argc, char** argv) {
    QApplication qt(argc, argv);
    QApplication::setApplicationName(QStringLiteral("sendspin"));
    QApplication::setApplicationDisplayName(QStringLiteral("Sendspin"));
    // A tray app: no window is not a reason to exit
    QApplication::setQuitOnLastWindowClosed(false);

    SingleInstance instance(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation));
    if (!instance.claim()) {
        return 0;
    }

    App app(data_dir());
    QObject::connect(&instance, &SingleInstance::secondLaunch, &app, &App::onSecondLaunch);
    app.start();
    return QApplication::exec();
}
