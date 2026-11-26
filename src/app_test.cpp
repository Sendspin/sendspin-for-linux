#include "app.h"

#include "update_test_server.h"

#include <QDir>
#include <QFile>
#include <QQmlEngine>
#include <QScopeGuard>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

namespace {

QStringList qml_complaints;

/// Keeps what the QML engine says of the app's own files: a wrong name or binding only warns
void note_qml_complaints(QtMsgType type, const QMessageLogContext& /*context*/,
                         const QString& message) {
    if (type != QtDebugMsg && type != QtInfoMsg && message.contains("qrc:/qml/")) {
        qml_complaints << message;
    }
}

}  // namespace

class AppTest : public QObject {
    Q_OBJECT

private slots:
    // Constructs and destroys a SendspinServer and a SendspinClient, so both libraries link
    void menuHoldsOpenStatusSettingsAndQuit() {
        const QTemporaryDir dir;
        App app(dir.path().toStdString());

        QStringList labels;
        QList<bool> enabled;
        for (const QAction* action : app.menu()->actions()) {
            if (!action->isSeparator() && action->isVisible()) {
                labels << action->text();
                enabled << action->isEnabled();
            }
        }
        QCOMPARE(labels, (QStringList{"Open Sendspin", "Not in use", "Settings…", "Quit"}));
        QCOMPARE(enabled, (QList<bool>{true, false, true, true}));
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

    void settingsOpenInOneWindow() {
        const QTemporaryDir dir;
        App app(dir.path().toStdString());
        QVERIFY(app.settings_window() == nullptr);

        qml_complaints.clear();
        const QtMessageHandler previous = qInstallMessageHandler(note_qml_complaints);
        const auto restore = qScopeGuard([previous] { qInstallMessageHandler(previous); });

        app.showSettings();
        QQuickWindow* window = app.settings_window();
        QVERIFY(window != nullptr);
        QVERIFY(window->isVisible());
        QCOMPARE(window->property("section").toInt(), 0);

        // Every section, and the pairing dialog, with nothing the QML engine objects to
        for (int section = 0; section < 3; section++) {
            window->setProperty("section", section);
            QTest::qWait(50);
        }
        app.settings()->pair("no-such-speaker");
        QTest::qWait(50);
        // A speaker names itself: its name is never read as rich text
        const QObject* title = window->findChild<QObject*>("pairingTitle");
        QVERIFY(title != nullptr);
        QCOMPARE(title->property("textFormat").toInt(), int{Qt::PlainText});
        app.settings()->cancelPairing();
        QTest::qWait(50);
        QCOMPARE(qml_complaints, QStringList());

        // Asked for again, closed or not, it is the same window
        window->close();
        QVERIFY(!window->isVisible());
        app.showSettings();
        QCOMPARE(app.settings_window(), window);
        QVERIFY(window->isVisible());
        QCOMPARE(window->property("section").toInt(), 2);
        app.showSettings();
        QCOMPARE(app.settings_window(), window);
    }

    // Hidden in a build with no Discord application to show itself as
    void theDiscordSwitchIsOffAndThereOnlyWithAnApplication() {
        const QTemporaryDir dir;
        App app(dir.path().toStdString());
        app.showSettings();
        // Music Assistant: what is on another section is not visible either way
        app.settings_window()->setProperty("section", 2);
        const QObject* discord = app.settings_window()->findChild<QObject*>("discordRichPresence");
        QVERIFY(discord != nullptr);
        QCOMPARE(discord->property("visible").toBool(), app.settings()->discord_available());
        QCOMPARE(app.settings()->discord_available(),
                 !DiscordPresence::build_client_id().isEmpty());
        QCOMPARE(discord->property("checked").toBool(), false);
    }

    // A click on a pairing notification
    void showPairingOpensSettingsOnMusicAssistant() {
        const QTemporaryDir dir;
        App app(dir.path().toStdString());
        app.showSettings();
        QQuickWindow* window = app.settings_window();
        QVERIFY(window != nullptr);
        window->setProperty("section", 0);
        window->close();

        app.showPairing();
        QVERIFY(window->isVisible());
        QCOMPARE(window->property("section").toInt(), 2);
        // Copy is there for when digits show; with none it is hidden, and not given the focus
        const QObject* copy = window->findChild<QObject*>("copyCode");
        QVERIFY(copy != nullptr);
        QVERIFY(!copy->property("visible").toBool());
        QVERIFY(!copy->property("activeFocus").toBool());
    }

    // The notification only says the code was copied where it was
    void theCodesNoticeSaysWhereTheCodeIs() {
        QCOMPARE(App::code_notice_body("Music Assistant", true),
                 "It is on the clipboard: paste it in Music Assistant to pair this PC.");
        QCOMPARE(App::code_notice_body("Music Assistant", false),
                 "Enter it in Music Assistant to pair this PC. Click to copy it from Settings.");
        QVERIFY(App::code_notice_body(QString(), true).contains("on the clipboard"));
        QVERIFY(!App::code_notice_body(QString(), false).contains("on the clipboard"));
    }

    void theTraysSettingsOpensTheWindow() {
        const QTemporaryDir dir;
        App app(dir.path().toStdString());
        for (QAction* action : app.menu()->actions()) {
            if (action->text() == "Settings…") {
                action->trigger();
            }
        }
        QVERIFY(app.settings_window() != nullptr);
        QVERIFY(app.settings_window()->isVisible());
    }

    // Between the status and Settings, and only while there is one
    void theTrayShowsANewerRelease() {
        const ReleaseServer server;
        qputenv("SENDSPIN_UPDATE_URL", server.url().toString().toUtf8());
        QCoreApplication::setApplicationVersion("1.0.0");
        const auto restore = qScopeGuard([] {
            qunsetenv("SENDSPIN_UPDATE_URL");
            QCoreApplication::setApplicationVersion({});
            QSettings(QStringLiteral("sendspin"), QStringLiteral("sendspin")).remove("updates");
        });
        const QTemporaryDir dir;
        App app(dir.path().toStdString());
        QAction* update = app.menu()->actions().at(3);
        QVERIFY(!update->isVisible());
        QTRY_VERIFY(update->isVisible());
        QCOMPARE(update->text(), "Sendspin 9.0.0 is available…");
        QCOMPARE(app.menu()->actions().at(4)->text(), "Settings…");

        // It leads to the switch and the button
        update->trigger();
        QVERIFY(app.settings_window() != nullptr);
        QCOMPARE(app.settings_window()->property("section").toInt(), 0);

        app.settings()->setCheckForUpdates(false);
        QVERIFY(!update->isVisible());
    }

    void theTraysOpenOpensThePicker() {
        const QTemporaryDir dir;
        App app(dir.path().toStdString());
        app.menu()->actions().first()->trigger();
        QVERIFY(app.picker_window()->window() != nullptr);
        QVERIFY(app.picker_window()->window()->isVisible());
    }

    // Where there is no tray, this is the only Quit
    void theSettingsButtonQuits() {
        const QTemporaryDir dir;
        App app(dir.path().toStdString());
        app.showSettings();
        QObject* quit = app.settings_window()->findChild<QObject*>("quit");
        QVERIFY(quit != nullptr);
        // Which the window's engine hands to the application
        QSignalSpy asked(qmlEngine(app.settings_window()), &QQmlEngine::quit);
        QVERIFY(QMetaObject::invokeMethod(quit, "clicked"));
        QCOMPARE(asked.count(), 1);
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

    // The switch is live: what the announcer is told of shows, or does not, from the next track
    void theSwitchTurnsTrackNotificationsOnAndOff() {
        const QTemporaryDir dir;
        App app(dir.path().toStdString());
        const auto forget = qScopeGuard([] {
            QSettings(QStringLiteral("sendspin"), QStringLiteral("sendspin"))
                .remove("notifications");
        });
        // Nothing onto the desktop of whoever runs the tests
        QVERIFY(disconnect(app.announcer(), nullptr, app.track_notifier(), nullptr));
        QSignalSpy announced(app.announcer(), &TrackAnnouncer::announced);
        // Not the second a track takes to be due
        app.announcer()->findChild<QTimer*>("settle")->setInterval(0);
        const auto plays = [&app](const char* title) {
            app.announcer()->update(true, {.title = title}, {});
            QTest::qWait(20);
        };

        QVERIFY(!app.settings()->track_notifications());
        plays("So What");
        QCOMPARE(announced.count(), 0);

        // As the switch in Settings does
        app.showSettings();
        QObject* toggle = app.settings_window()->findChild<QObject*>("trackNotifications");
        QVERIFY(toggle != nullptr);
        QCOMPARE(toggle->property("checked").toBool(), false);
        app.settings()->setTrackNotifications(true);
        QCOMPARE(toggle->property("checked").toBool(), true);
        plays("Freddie Freeloader");
        QCOMPARE(announced.count(), 1);
        QCOMPARE(announced.last().first().toString(), "Freddie Freeloader");

        app.settings()->setTrackNotifications(false);
        plays("Blue in Green");
        QCOMPARE(announced.count(), 1);
        app.settings()->setTrackNotifications(true);
        plays("All Blues");
        QCOMPARE(announced.count(), 2);
    }

    void aClickOnTheTracksNotificationOpensThePicker() {
        const QTemporaryDir dir;
        App app(dir.path().toStdString());
        QVERIFY(app.picker_window()->window() == nullptr);
        // As the desktop reports the click
        emit app.track_notifier()->clicked({});
        QVERIFY(app.picker_window()->window() != nullptr);
        QVERIFY(app.picker_window()->window()->isVisible());
        // Not Settings, which the pairing notification opens
        QVERIFY(app.settings_window() == nullptr);
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
        // And gets them
        QVERIFY(app.settings_window() != nullptr);
        QVERIFY(app.settings_window()->isVisible());
    }
};

QTEST_MAIN(AppTest)
#include "app_test.moc"
