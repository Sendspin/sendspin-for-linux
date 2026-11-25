#include "paired_servers.h"

#include <QDir>
#include <QTemporaryDir>
#include <QTest>

class PairedServersTest : public QObject {
    Q_OBJECT

private slots:
    void noneAndNoFileBeforeAPairing() {
        const QTemporaryDir dir;
        const PairedServers servers(dir.path().toStdString());
        QVERIFY(servers.list().isEmpty());
        QVERIFY(QDir(dir.path()).isEmpty());
    }

    void keepsPairingsAcrossRestartsLatestFirst() {
        const QTemporaryDir dir;
        const QDateTime monday(QDate(2026, 11, 23), QTime(9, 0), QTimeZone::UTC);
        const QDateTime tuesday = monday.addDays(1);
        {
            PairedServers servers(dir.path().toStdString());
            servers.add("server-a", "Music Assistant", monday);
            servers.add("server-b", "Den", tuesday);
        }
        const PairedServers servers(dir.path().toStdString());
        QCOMPARE(servers.list(), (QList<PairedServer>{{"server-b", "Den", tuesday},
                                                      {"server-a", "Music Assistant", monday}}));
    }

    void pairingAgainReplacesTheRecord() {
        const QTemporaryDir dir;
        const QDateTime monday(QDate(2026, 11, 23), QTime(9, 0), QTimeZone::UTC);
        PairedServers servers(dir.path().toStdString());
        servers.add("server-a", "Music Assistant", monday);
        servers.add("server-b", "Den", monday);
        servers.add("server-a", "Renamed", monday.addDays(2));
        QCOMPARE(servers.list().size(), 2);
        QCOMPARE(servers.list().first(), (PairedServer{"server-a", "Renamed", monday.addDays(2)}));
    }
};

QTEST_GUILESS_MAIN(PairedServersTest)
#include "paired_servers_test.moc"
