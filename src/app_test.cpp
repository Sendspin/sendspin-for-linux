#include "app.h"

#include <QTemporaryDir>
#include <QTest>

class AppTest : public QObject {
    Q_OBJECT

private slots:
    // Constructs and destroys a SendspinServer and a SendspinClient, so both libraries link
    void menuIsTheStub() {
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
        QCOMPARE(enabled, (QList<bool>{false, false, true}));
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

    void secondLaunchIsHandled() {
        const QTemporaryDir dir;
        App app(dir.path().toStdString());
        app.onSecondLaunch();
    }
};

QTEST_MAIN(AppTest)
#include "app_test.moc"
