#include "app.h"

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

    void pairingCodeIsGrouped() {
        QCOMPARE(App::grouped_code("042735"), "042-735");
        QCOMPARE(App::grouped_code("0427"), "0427");
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
        QVERIFY(QMetaObject::invokeMethod(app.picker_window()->window(), "settingsRequested"));
        QCOMPARE(asked.count(), 1);
    }
};

QTEST_MAIN(AppTest)
#include "app_test.moc"
