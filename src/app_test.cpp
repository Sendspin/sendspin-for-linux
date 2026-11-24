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

    void secondLaunchIsHandled() {
        const QTemporaryDir dir;
        App app(dir.path().toStdString());
        app.onSecondLaunch();
    }
};

QTEST_MAIN(AppTest)
#include "app_test.moc"
