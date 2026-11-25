#include "logging.h"

#include <QLoggingCategory>
#include <QTest>

#include <sendspin/client.h>

class LoggingTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        if (qEnvironmentVariableIsSet("QT_LOGGING_RULES")) {
            QSKIP("QT_LOGGING_RULES overrides the rules under test");
        }
    }

    void debugIsOffByDefault() {
        set_log_rules(false);
        const QLoggingCategory category("sendspin.cast");
        QVERIFY(!category.isDebugEnabled());
        QVERIFY(category.isInfoEnabled());
        QVERIFY(category.isWarningEnabled());
        QCOMPARE(sendspin::SendspinClient::get_log_level(), sendspin::LogLevel::WARN);
    }

    void verboseTurnsDebugOn() {
        set_log_rules(true);
        QVERIFY(QLoggingCategory("sendspin.cast").isDebugEnabled());
        QVERIFY(QLoggingCategory("sendspin.server.lib").isDebugEnabled());
        QCOMPARE(sendspin::SendspinClient::get_log_level(), sendspin::LogLevel::INFO);
    }
};

QTEST_GUILESS_MAIN(LoggingTest)
#include "logging_test.moc"
