#include "app.h"

#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTest>

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
    void menuHoldsTheStatusSettingsAndQuit() {
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
        QCOMPARE(labels, (QStringList{"Not connected", "Settings…", "Quit"}));
        QCOMPARE(enabled, (QList<bool>{false, true, true}));
    }

    void constructingWritesNothing() {
        const QTemporaryDir dir;
        { const App app(dir.path().toStdString()); }
        QVERIFY(QDir(dir.path()).isEmpty());
    }

    void statusLineFollowsTheClient() {
        QCOMPARE(App::status_text(Client::State::Stopped, {}), "Not connected");
        QCOMPARE(App::status_text(Client::State::Waiting, {}), "Waiting for a server");
        QCOMPARE(App::status_text(Client::State::Connected, "Music Assistant"),
                 "Connected to Music Assistant");
        QCOMPARE(App::status_text(Client::State::Pairing, "Music Assistant"),
                 "Pairing with Music Assistant…");
        QCOMPARE(App::status_text(Client::State::Pairing, {}), "Pairing…");
    }

    void statusLineNamesWhatTheServerPlaysHere() {
        QCOMPARE(App::status_text(Client::State::Connected, "Music Assistant",
                                  {.title = "So What", .artist = "Miles Davis"}),
                 "So What — Miles Davis");
        QCOMPARE(App::status_text(Client::State::Connected, "Music Assistant", {.title = "Radio"}),
                 "Radio");
        // Only while connected
        QCOMPARE(App::status_text(Client::State::Waiting, {}, {.title = "So What"}),
                 "Waiting for a server");
    }

    void pairingCodeIsGrouped() {
        QCOMPARE(App::grouped_code("042735"), "042-735");
        QCOMPARE(App::grouped_code("0427"), "0427");
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
        app.settings()->cancelPairing();
        QTest::qWait(50);
        QCOMPARE(qml_complaints, QStringList());

        // Asked for again, closed or not, it is the same window
        window->close();
        QVERIFY(!window->isVisible());
        app.onSecondLaunch();
        QCOMPARE(app.settings_window(), window);
        QVERIFY(window->isVisible());
        QCOMPARE(window->property("section").toInt(), 2);
        app.showSettings();
        QCOMPARE(app.settings_window(), window);
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
};

QTEST_MAIN(AppTest)
#include "app_test.moc"
