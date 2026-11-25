#include "device_name.h"

#include <QSysInfo>
#include <QTemporaryDir>
#include <QTest>

class DeviceNameTest : public QObject {
    Q_OBJECT

private slots:
    void readsThePrettyHostname_data() {
        QTest::addColumn<QByteArray>("contents");
        QTest::addColumn<QString>("expected");
        const QString hostname = QSysInfo::machineHostName();

        QTest::newRow("plain") << QByteArray("PRETTY_HOSTNAME=Desk\n") << "Desk";
        QTest::newRow("quoted") << QByteArray(
                                       "ICON_NAME=computer\nPRETTY_HOSTNAME=\"Chris's PC\"\n")
                                << "Chris's PC";
        QTest::newRow("single quoted")
            << QByteArray("PRETTY_HOSTNAME='Living Room'\n") << "Living Room";
        QTest::newRow("escaped") << QByteArray("PRETTY_HOSTNAME=\"The \\\"Big\\\" One\"\n")
                                 << "The \"Big\" One";
        QTest::newRow("utf-8") << QByteArray("PRETTY_HOSTNAME=\"Büro\"\n") << "Büro";
        QTest::newRow("empty") << QByteArray("PRETTY_HOSTNAME=\"\"\n") << hostname;
        QTest::newRow("absent") << QByteArray("CHASSIS=desktop\n") << hostname;
    }

    void readsThePrettyHostname() {
        QFETCH(QByteArray, contents);
        QFETCH(QString, expected);
        const QTemporaryDir dir;
        const QString path = dir.filePath("machine-info");
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(contents);
        file.close();
        QCOMPARE(device_name(path), expected);
    }

    void fallsBackToTheHostname() {
        QCOMPARE(device_name("/nonexistent/machine-info"), QSysInfo::machineHostName());
    }
};

QTEST_APPLESS_MAIN(DeviceNameTest)
#include "device_name_test.moc"
