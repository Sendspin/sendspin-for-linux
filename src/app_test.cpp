#include "app.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class AppTest : public QObject {
    Q_OBJECT

private slots:
    // Constructs and destroys a SendspinServer and a SendspinClient, so both libraries link
    void menuHoldsStatusSettingsAndQuit() {
        const QTemporaryDir dir;
        App app(dir.path().toStdString());

        QStringList labels;
        QList<bool> enabled;
        for (const QAction* action : app.menu()->actions()) {
            if (!action->isSeparator()) {
                labels << action->text();
                enabled << action->isEnabled();
            }
        }
        QCOMPARE(labels, (QStringList{"Not in use", "Settings…", "Quit"}));
        QCOMPARE(enabled, (QList<bool>{false, true, true}));
    }

    void constructingWritesNothing() {
        const QTemporaryDir dir;
        { const App app(dir.path().toStdString()); }
        QVERIFY(QDir(dir.path()).isEmpty());
    }

    // Waves while the PC's sound goes out through Sendspin, the mark alone otherwise
    void trayIconFollowsPlaying() {
        QCOMPARE(App::tray_icon_name(true), "sendspin-waves-symbolic");
        QCOMPARE(App::tray_icon_name(false), "sendspin-symbolic");
        for (const bool plays : {true, false}) {
            QVERIFY(
                QFile::exists(QStringLiteral(":/icons/%1.svg").arg(App::tray_icon_name(plays))));
        }
    }

    // Installed where hicolor looks, the icon is found by name, which is what the tray hands the
    // panel; otherwise it is the picture
    void theTrayIconGoesByNameOnceInstalled() {
        const QString name = App::tray_icon_name(true);
        // Drawing the picture takes Qt's SVG plugin, which a build machine may lack
        QCOMPARE(App::tray_icon(name).name(), "");

        const QString hicolor = QStringLiteral("/usr/share/icons/hicolor/index.theme");
        if (!QFile::exists(hicolor)) {
            QSKIP("no hicolor icon theme on this machine");
        }
        const QTemporaryDir prefix;
        const QString theme = prefix.path() + "/hicolor";
        QVERIFY(QDir().mkpath(theme + "/symbolic/apps"));
        QVERIFY(QFile::copy(hicolor, theme + "/index.theme"));
        QVERIFY(QFile::copy(QStringLiteral(":/icons/%1.svg").arg(name),
                            theme + "/symbolic/apps/" + name + ".svg"));
        const QStringList paths = QIcon::themeSearchPaths();
        const QString before = QIcon::themeName();
        QIcon::setThemeSearchPaths({prefix.path()});
        QIcon::setThemeName("hicolor");
        QCOMPARE(App::tray_icon(name).name(), name);
        QIcon::setThemeSearchPaths(paths);
        QIcon::setThemeName(before);
    }

    // The tray icon, the launcher and a second launch all end here
    void showPickerOpensThePicker() {
        const QTemporaryDir dir;
        App app(dir.path().toStdString());
        QVERIFY(app.picker_window()->window() == nullptr);
        app.showPicker();
        QVERIFY(app.picker_window()->window() != nullptr);
        QVERIFY(app.picker_window()->window()->isVisible());
    }

    void theGearAsksForSettings() {
        const QTemporaryDir dir;
        App app(dir.path().toStdString());
        app.showPicker();
        QSignalSpy asked(app.picker_window(), &PickerWindow::settingsRequested);
        QObject* gear = app.picker_window()->window()->findChild<QObject*>("settings");
        QVERIFY(gear != nullptr);
        QVERIFY(QMetaObject::invokeMethod(gear, "clicked"));
        QCOMPARE(asked.count(), 1);
    }
};

QTEST_MAIN(AppTest)
#include "app_test.moc"
