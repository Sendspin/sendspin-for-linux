#include "autostart.h"

#include <QFile>
#include <QTemporaryDir>
#include <QTest>

class AutostartTest : public QObject {
    Q_OBJECT

private slots:
    void init() {
        this->dir_ = std::make_unique<QTemporaryDir>();
        qputenv("XDG_CONFIG_HOME", this->dir_->path().toUtf8());
    }

    void offUntilSwitchedOn() {
        QVERIFY(!autostart::enabled());
        // Nothing to remove is not a failure
        QVERIFY(autostart::set_enabled(false));
        QVERIFY(!QFile::exists(autostart::path()));
    }

    void writesAnEntryTheDesktopStarts() {
        QVERIFY(autostart::set_enabled(true, "/usr/bin/sendspin-for-linux"));
        QVERIFY(autostart::enabled());
        QCOMPARE(autostart::path(),
                 this->dir_->path() + "/autostart/com.sendspin_audio.Sendspin.desktop");

        QFile file(autostart::path());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QList<QByteArray> lines = file.readAll().split('\n');
        QCOMPARE(lines.first(), "[Desktop Entry]");
        QVERIFY(lines.contains("Type=Application"));
        QVERIFY(lines.contains("Name=Sendspin"));
        QVERIFY(lines.contains("Exec=\"/usr/bin/sendspin-for-linux\""));
    }

    void switchingOffRemovesIt() {
        QVERIFY(autostart::set_enabled(true, "/usr/bin/sendspin-for-linux"));
        QVERIFY(autostart::set_enabled(false));
        QVERIFY(!QFile::exists(autostart::path()));
        QVERIFY(!autostart::enabled());
    }

    // As a desktop's own "startup applications" switches one off
    void aHiddenEntryIsOff() {
        QVERIFY(autostart::set_enabled(true, "/usr/bin/sendspin-for-linux"));
        QFile file(autostart::path());
        QVERIFY(file.open(QIODevice::Append));
        file.write("Hidden=true\n");
        file.close();
        QVERIFY(!autostart::enabled());
        // Switching it on again writes it afresh
        QVERIFY(autostart::set_enabled(true, "/usr/bin/sendspin-for-linux"));
        QVERIFY(autostart::enabled());
    }

    void anEntryGnomeSwitchedOffIsOff() {
        QVERIFY(autostart::set_enabled(true, "/usr/bin/sendspin-for-linux"));
        QFile file(autostart::path());
        QVERIFY(file.open(QIODevice::ReadWrite));
        const QByteArray entry = file.readAll().replace("X-GNOME-Autostart-enabled=true",
                                                        "X-GNOME-Autostart-enabled=false");
        file.resize(0);
        file.write(entry);
        file.close();
        QVERIFY(!autostart::enabled());
    }

    // The app moved, or starts differently than when the entry was written
    void refreshRewritesAStaleEntryAndOnlyAnEnabledOne() {
        const auto entry = [] {
            QFile file(autostart::path());
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        // Off: nothing appears
        autostart::refresh("/usr/bin/sendspin-for-linux");
        QVERIFY(!QFile::exists(autostart::path()));

        QVERIFY(autostart::set_enabled(true, "/opt/old/sendspin-for-linux"));
        autostart::refresh("/usr/bin/sendspin-for-linux");
        QVERIFY(entry().contains("Exec=\"/usr/bin/sendspin-for-linux\"\n"));
        QVERIFY(autostart::enabled());

        // Switched off by the desktop: left as it is
        QFile file(autostart::path());
        QVERIFY(file.open(QIODevice::Append));
        file.write("Hidden=true\n");
        file.close();
        const QByteArray hidden = entry();
        autostart::refresh("/somewhere/else");
        QCOMPARE(entry(), hidden);
    }

    void execIsQuotedAndEscaped() {
        QCOMPARE(autostart::exec_value("/opt/My Apps/sendspin"), "\"/opt/My Apps/sendspin\"");
        QCOMPARE(autostart::exec_value("/opt/50%/a$b"), "\"/opt/50%%/a\\\\$b\"");
    }

private:
    std::unique_ptr<QTemporaryDir> dir_;
};

QTEST_GUILESS_MAIN(AutostartTest)
#include "autostart_test.moc"
