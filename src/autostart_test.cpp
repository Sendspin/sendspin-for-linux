#include "autostart.h"

#include "skip_unless_required.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>

namespace {

/// The Background portal, as far as the app uses it: answers each request with what it was told
class FakePortal : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Background")

public:
    bool grant_ = true;
    QVariantMap asked_;

public slots:
    QDBusObjectPath RequestBackground(const QString& /*parent_window*/, const QVariantMap& options,
                                      const QDBusMessage& message) {
        this->asked_ = options;
        const QString path = QStringLiteral("/org/freedesktop/portal/desktop/request/%1/%2")
                                 .arg(message.service().mid(1).replace(u'.', u'_'),
                                      options.value("handle_token").toString());
        QDBusMessage response = QDBusMessage::createSignal(
            path, QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"));
        const bool autostart = this->grant_ && options.value("autostart").toBool();
        response << (this->grant_ ? 0U : 1U) << QVariantMap{{"autostart", autostart}};
        QDBusConnection::sessionBus().send(response);
        return QDBusObjectPath(path);
    }
};

}  // namespace

class AutostartTest : public QObject {
    Q_OBJECT

private slots:
    void init() {
        this->dir_ = std::make_unique<QTemporaryDir>();
        qputenv("XDG_CONFIG_HOME", (this->dir_->path() + "/home").toUtf8());
        qputenv("XDG_CONFIG_DIRS", (this->dir_->path() + "/etc").toUtf8());
    }

    /// What a package installs for every user
    void install_packaged_entry() {
        QVERIFY(QDir().mkpath(this->dir_->path() + "/etc/autostart"));
        QFile file(this->dir_->path() + "/etc/autostart/com.sendspin_audio.Sendspin.desktop");
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("[Desktop Entry]\nType=Application\nName=Sendspin\n"
                   "Exec=sendspin-for-linux --background\n");
    }

    void offUntilSwitchedOn() {
        QVERIFY(!autostart::enabled());
        // Nothing to remove is not a failure
        autostart::set_enabled(false);
        QVERIFY(!QFile::exists(autostart::path()));
    }

    void writesAnEntryThatStartsTheTrayAlone() {
        autostart::set_enabled(true, "/usr/bin/sendspin-for-linux");
        QVERIFY(autostart::enabled());
        QCOMPARE(autostart::path(),
                 this->dir_->path() + "/home/autostart/com.sendspin_audio.Sendspin.desktop");

        QFile file(autostart::path());
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QList<QByteArray> lines = file.readAll().split('\n');
        QCOMPARE(lines.first(), "[Desktop Entry]");
        QVERIFY(lines.contains("Type=Application"));
        QVERIFY(lines.contains("Name=Sendspin"));
        QVERIFY(lines.contains("Exec=\"/usr/bin/sendspin-for-linux\" --background"));
        QVERIFY(lines.contains("Icon=com.sendspin_audio.Sendspin"));
    }

    void switchingOffRemovesIt() {
        autostart::set_enabled(true, "/usr/bin/sendspin-for-linux");
        autostart::set_enabled(false);
        QVERIFY(!QFile::exists(autostart::path()));
        QVERIFY(!autostart::enabled());
    }

    // The same entry, by name: the user's own is written only to switch the package's off
    void aPackagedEntryIsOnUntilSwitchedOff() {
        this->install_packaged_entry();
        QVERIFY(autostart::enabled());
        QVERIFY(!QFile::exists(autostart::path()));

        autostart::set_enabled(false);
        QVERIFY(!autostart::enabled());
        QFile file(autostart::path());
        QVERIFY(file.open(QIODevice::ReadOnly));
        QVERIFY(file.readAll().contains("\nHidden=true\n"));

        autostart::set_enabled(true);
        QVERIFY(autostart::enabled());
        QVERIFY(!QFile::exists(autostart::path()));
    }

    // The portal writes the Flatpak's entry where this build writes its own
    void theFlatpaksEntryIsLeftUntilSwitchedOn() {
        const QByteArray theirs =
            "[Desktop Entry]\nType=Application\nName=com.sendspin_audio.Sendspin\n"
            "Exec=flatpak run --command=sendspin-for-linux com.sendspin_audio.Sendspin "
            "--background\nX-Flatpak=com.sendspin_audio.Sendspin\n";
        const auto entry = [] {
            QFile file(autostart::path());
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };
        const auto write_theirs = [&theirs] {
            QVERIFY(QDir().mkpath(QFileInfo(autostart::path()).path()));
            QFile file(autostart::path());
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(theirs);
        };
        write_theirs();
        // It starts the Flatpak, not this build
        QVERIFY(!autostart::enabled());
        autostart::refresh("/usr/bin/sendspin-for-linux");
        QCOMPARE(entry(), theirs);
        autostart::set_enabled(false);
        QCOMPARE(entry(), theirs);

        autostart::set_enabled(true, "/usr/bin/sendspin-for-linux");
        QVERIFY(autostart::enabled());
        QVERIFY(entry().contains("Exec=\"/usr/bin/sendspin-for-linux\" --background\n"));

        // Over a package's entry too, which it keeps from starting
        this->install_packaged_entry();
        write_theirs();
        QVERIFY(!autostart::enabled());
        autostart::set_enabled(false);
        QCOMPARE(entry(), theirs);
        autostart::set_enabled(true);
        QVERIFY(autostart::enabled());
        QVERIFY(!QFile::exists(autostart::path()));
    }

    void thePortalIsAskedForTheTrayAloneAndAnswers() {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) {
            SKIP_UNLESS_REQUIRED("no session bus");
        }
        const QString service = QStringLiteral("com.sendspin_audio.Test.Portal");
        FakePortal portal;
        QVERIFY(bus.registerService(service));
        QVERIFY(bus.registerObject("/org/freedesktop/portal/desktop", &portal,
                                   QDBusConnection::ExportAllSlots));

        std::optional<bool> granted;
        const auto done = [&granted](bool autostart) { granted = autostart; };
        autostart::ask_portal(true, service, this, done);
        QTRY_VERIFY(granted.has_value());
        QVERIFY(*granted);
        QCOMPARE(portal.asked_.value("autostart").toBool(), true);
        QCOMPARE(portal.asked_.value("commandline").toStringList(),
                 QStringList({"sendspin-for-linux", "--background"}));

        granted.reset();
        autostart::ask_portal(false, service, this, done);
        QTRY_VERIFY(granted.has_value());
        QVERIFY(!*granted);

        // Refused: nothing starts it
        portal.grant_ = false;
        granted.reset();
        autostart::ask_portal(true, service, this, done);
        QTRY_VERIFY(granted.has_value());
        QVERIFY(!*granted);

        // No portal at all answers the same
        bus.unregisterService(service);
        granted.reset();
        autostart::ask_portal(true, service, this, done);
        QTRY_VERIFY(granted.has_value());
        QVERIFY(!*granted);
    }

    // As a desktop's own "startup applications" switches one off
    void aHiddenEntryIsOff() {
        autostart::set_enabled(true, "/usr/bin/sendspin-for-linux");
        QFile file(autostart::path());
        QVERIFY(file.open(QIODevice::Append));
        file.write("Hidden=true\n");
        file.close();
        QVERIFY(!autostart::enabled());
        // Switching it on again writes it afresh
        autostart::set_enabled(true, "/usr/bin/sendspin-for-linux");
        QVERIFY(autostart::enabled());
    }

    void anEntryGnomeSwitchedOffIsOff() {
        autostart::set_enabled(true, "/usr/bin/sendspin-for-linux");
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

        autostart::set_enabled(true, "/opt/old/sendspin-for-linux");
        autostart::refresh("/usr/bin/sendspin-for-linux");
        QVERIFY(entry().contains("Exec=\"/usr/bin/sendspin-for-linux\" --background\n"));
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
