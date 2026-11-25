#include "single_instance.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

class SingleInstanceTest : public QObject {
    Q_OBJECT

private slots:
    void firstClaimWins() {
        const QTemporaryDir dir;
        SingleInstance first(dir.path());
        QVERIFY(first.claim());
    }

    void secondClaimLosesAndPingsTheFirst() {
        const QTemporaryDir dir;
        SingleInstance first(dir.path());
        const QSignalSpy pinged(&first, &SingleInstance::secondLaunch);
        QVERIFY(first.claim());

        SingleInstance second(dir.path());
        const QSignalSpy second_pinged(&second, &SingleInstance::secondLaunch);
        QVERIFY(!second.claim());
        QTRY_COMPARE(pinged.count(), 1);

        SingleInstance third(dir.path());
        QVERIFY(!third.claim());
        QTRY_COMPARE(pinged.count(), 2);
        QCOMPARE(second_pinged.count(), 0);
    }

    // On Wayland only the launch that the user made may give a window the focus
    void theSecondLaunchHandsOverItsActivationToken() {
        const QTemporaryDir dir;
        SingleInstance first(dir.path());
        QSignalSpy pinged(&first, &SingleInstance::secondLaunch);
        QVERIFY(first.claim());

        SingleInstance second(dir.path());
        QVERIFY(!second.claim("token-1"));
        QTRY_COMPARE(pinged.count(), 1);
        QCOMPARE(pinged.takeFirst().at(0).toString(), "token-1");

        SingleInstance third(dir.path());
        QVERIFY(!third.claim());
        QTRY_COMPARE(pinged.count(), 1);
        QCOMPARE(pinged.takeFirst().at(0).toString(), "");
    }

    void claimIsFreeAgainAfterTheInstanceExits() {
        const QTemporaryDir dir;
        {
            SingleInstance first(dir.path());
            QVERIFY(first.claim());
        }
        SingleInstance next(dir.path());
        QVERIFY(next.claim());
    }

    void staleSocketOfACrashedInstanceIsReplaced() {
        const QTemporaryDir dir;
        // A crash leaves the socket file behind with nothing listening on it
        QFile stale(dir.path() + QStringLiteral("/sendspin.sock"));
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.close();

        SingleInstance first(dir.path());
        const QSignalSpy pinged(&first, &SingleInstance::secondLaunch);
        QVERIFY(first.claim());
        SingleInstance second(dir.path());
        QVERIFY(!second.claim());
        QTRY_COMPARE(pinged.count(), 1);
    }

    void instancesInOtherDirectoriesAreIndependent() {
        const QTemporaryDir a;
        const QTemporaryDir b;
        SingleInstance first(a.path());
        SingleInstance other(b.path());
        QVERIFY(first.claim());
        QVERIFY(other.claim());
    }
};

QTEST_GUILESS_MAIN(SingleInstanceTest)
#include "single_instance_test.moc"
