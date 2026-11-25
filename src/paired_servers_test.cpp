#include "paired_servers.h"

#include <QDir>
#include <QFile>
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

    void theFileIsOwnerOnlyAndDatedInIso() {
        const QTemporaryDir dir;
        const QString path = dir.path() + "/paired_servers.ini";
        const QDateTime monday(QDate(2026, 11, 23), QTime(9, 0), QTimeZone::UTC);
        PairedServers servers(dir.path().toStdString());
        servers.add("server-a", "Music Assistant", monday);
        const auto owner_only = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
        const auto all = owner_only | QFileDevice::ReadGroup | QFileDevice::WriteGroup |
                         QFileDevice::ReadOther | QFileDevice::WriteOther;
        QCOMPARE(QFile::permissions(path) & all, owner_only);

        // One written before the mode was set is put right at the next pairing
        QVERIFY(QFile::setPermissions(path, all));
        servers.add("server-b", "Den", monday);
        QCOMPARE(QFile::permissions(path) & all, owner_only);

        QFile file(path);
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("paired_at=2026-11-23T09:00:00Z"));
    }

    void keepsOnlyTheNewest() {
        const QTemporaryDir dir;
        const QDateTime start(QDate(2026, 1, 1), QTime(9, 0), QTimeZone::UTC);
        PairedServers servers(dir.path().toStdString());
        for (int i = 0; i < PairedServers::MAX_SERVERS + 4; i++) {
            servers.add(QStringLiteral("server-%1").arg(i), "A server", start.addDays(i));
        }
        QCOMPARE(servers.list().size(), PairedServers::MAX_SERVERS);
        QCOMPARE(servers.list().first().id,
                 QStringLiteral("server-%1").arg(PairedServers::MAX_SERVERS + 3));
        QCOMPARE(servers.list().last().id, "server-4");
    }
};

QTEST_GUILESS_MAIN(PairedServersTest)
#include "paired_servers_test.moc"
