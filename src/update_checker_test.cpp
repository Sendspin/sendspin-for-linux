#include "update_checker.h"

#include "update_test_server.h"

#include <QDateTime>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

namespace {

const QVersionNumber OLDER(1, 0, 0);

QSettings conf() {
    return QSettings(QStringLiteral("sendspin"), QStringLiteral("sendspin"));
}

}  // namespace

class UpdateCheckerTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // Before anything reads a setting: QSettings fixes its paths at first use
        qputenv("XDG_CONFIG_HOME", this->config_.path().toUtf8());
    }

    void init() {
        conf().clear();
    }

    void theReplysReleaseIsRead() {
        const auto release = UpdateChecker::parse(
            "{\"tag_name\": \"v1.2.3\", \"html_url\": \"https://example.org/v1.2.3\","
            " \"assets\": [{\"name\": \"a.deb\","
            "              \"browser_download_url\": \"https://example.org/a.deb\"},"
            "             {\"name\": \"\", \"browser_download_url\": \"https://example.org/b\"}]}");
        QVERIFY(release.has_value());
        QCOMPARE(release->version, QVersionNumber(1, 2, 3));
        QCOMPARE(release->page, QUrl("https://example.org/v1.2.3"));
        QCOMPARE(release->files.keys(), QStringList{"a.deb"});
        QCOMPARE(release->files.value("a.deb"), QUrl("https://example.org/a.deb"));
    }

    void whatIsNoReleaseIsNotOffered_data() {
        QTest::addColumn<QByteArray>("reply");
        QTest::newRow("nothing") << QByteArray();
        QTest::newRow("an error page") << QByteArray("<html>rate limited</html>");
        QTest::newRow("no tag") << QByteArray("{\"message\": \"Not Found\"}");
        QTest::newRow("a tag that is no version") << QByteArray("{\"tag_name\": \"nightly\"}");
        QTest::newRow("a pre-release") << QByteArray("{\"tag_name\": \"v1.2.0-rc1\"}");
    }
    void whatIsNoReleaseIsNotOffered() {
        QFETCH(QByteArray, reply);
        QVERIFY(!UpdateChecker::parse(reply).has_value());
    }

    void aNewerReleaseIsFoundAndKept() {
        ReleaseServer server;
        {
            UpdateChecker checker(OLDER, server.url());
            QSignalSpy changed(&checker, &UpdateChecker::changed);
            QSignalSpy checked(&checker, &UpdateChecker::checked);
            QVERIFY(!checker.update().has_value());
            checker.start();
            QVERIFY(checked.wait());
            QCOMPARE(checked.first().first().toBool(), true);
            QCOMPARE(changed.count(), 1);
            QVERIFY(checker.update().has_value());
            QCOMPARE(checker.update()->version, QVersionNumber(9, 0, 0));
            QCOMPARE(checker.update()->page, server.url("page"));
            QCOMPARE(checker.update()->files.value(ReleaseServer::PACKAGE),
                     server.url(ReleaseServer::PACKAGE));
        }
        // The next start knows of it without asking
        UpdateChecker next(OLDER, server.url());
        next.start();
        QVERIFY(next.update().has_value());
        QCOMPARE(next.update()->files.size(), 3);
        QCOMPARE(server.checks, 1);
    }

    void itAsksOnceADay() {
        ReleaseServer server;
        {
            UpdateChecker checker(OLDER, server.url());
            QSignalSpy checked(&checker, &UpdateChecker::checked);
            checker.start();
            QVERIFY(checked.wait());
            QCOMPARE(server.checks, 1);
        }
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        // Started again, any number of times, within the day
        conf().setValue("updates/last_check", now - 23 * 3600);
        for (int start = 0; start < 3; ++start) {
            UpdateChecker checker(OLDER, server.url());
            QSignalSpy checked(&checker, &UpdateChecker::checked);
            checker.start();
            QVERIFY(!checked.wait(200));
        }
        QCOMPARE(server.checks, 1);

        // Left running, it asks when the day is over
        conf().setValue("updates/last_check", now - 24 * 3600 + 1);
        UpdateChecker checker(OLDER, server.url());
        QSignalSpy checked(&checker, &UpdateChecker::checked);
        checker.start();
        QVERIFY(checked.wait(3000));
        QCOMPARE(server.checks, 2);
        // And the day starts again
        QVERIFY(conf().value("updates/last_check").toLongLong() >= now);
        QVERIFY(!checked.wait(200));
        QCOMPARE(server.checks, 2);
    }

    void theSettingTurnsTheChecksOff() {
        ReleaseServer server;
        conf().setValue("updates/check", false);
        UpdateChecker checker(OLDER, server.url());
        QSignalSpy checked(&checker, &UpdateChecker::checked);
        QVERIFY(!checker.enabled());
        checker.start();
        QVERIFY(!checked.wait(300));
        QCOMPARE(server.checks, 0);

        checker.set_enabled(true);
        QVERIFY(conf().value("updates/check").toBool());
        QVERIFY(checked.wait());
        QCOMPARE(server.checks, 1);
        QVERIFY(checker.update().has_value());

        // Off forgets the release, and nothing asks again
        QSignalSpy changed(&checker, &UpdateChecker::changed);
        checker.set_enabled(false);
        QCOMPARE(changed.count(), 1);
        QVERIFY(!checker.update().has_value());
        QVERIFY(!conf().contains("updates/latest"));
        UpdateChecker next(OLDER, server.url());
        QSignalSpy next_checked(&next, &UpdateChecker::checked);
        next.start();
        QVERIFY(!next_checked.wait(300));
        QCOMPARE(server.checks, 1);

        // On again within the day: it asks, having forgotten the answer
        checker.set_enabled(true);
        QVERIFY(checked.wait());
        QCOMPARE(server.checks, 2);
        QVERIFY(checker.update().has_value());
    }

    void theSameOrAnOlderReleaseIsNoUpdate_data() {
        QTest::addColumn<QString>("current");
        QTest::newRow("the same") << "9.0.0";
        QTest::newRow("newer than the latest") << "9.0.1";
    }
    void theSameOrAnOlderReleaseIsNoUpdate() {
        QFETCH(QString, current);
        ReleaseServer server;
        UpdateChecker checker(QVersionNumber::fromString(current), server.url());
        QSignalSpy checked(&checker, &UpdateChecker::checked);
        checker.start();
        QVERIFY(checked.wait());
        QVERIFY(!checker.update().has_value());
    }

    // An answer that is no release leaves the day unspent
    void aCheckWithoutAnAnswerIsNotTheDaysCheck() {
        ReleaseServer server;
        UpdateChecker checker(OLDER, server.url("nothing-here"));
        QSignalSpy checked(&checker, &UpdateChecker::checked);
        checker.start();
        QVERIFY(checked.wait());
        QCOMPARE(checked.first().first().toBool(), false);
        QVERIFY(!checker.update().has_value());
        QVERIFY(!conf().contains("updates/last_check"));
    }

    // A test's App, and a build that was given no version
    void withoutAVersionNothingIsAsked() {
        ReleaseServer server;
        UpdateChecker checker(QVersionNumber(), server.url());
        QSignalSpy checked(&checker, &UpdateChecker::checked);
        checker.start();
        QVERIFY(!checked.wait(300));
        QCOMPARE(server.checks, 0);
    }

private:
    QTemporaryDir config_;
};

QTEST_GUILESS_MAIN(UpdateCheckerTest)
#include "update_checker_test.moc"
