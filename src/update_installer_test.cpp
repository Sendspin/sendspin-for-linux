#include "update_installer.h"

#include "skip_unless_required.h"
#include "update_test_server.h"

#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

using State = UpdateInstaller::State;
using Failure = UpdateInstaller::Failure;
using update::Origin;

namespace {

/// The release as the checker would hand it over
Release release_of(const ReleaseServer& server) {
    return *UpdateChecker::parse(server.latest());
}

bool probe(UpdateInstaller& installer) {
    QSignalSpy changed(&installer, &UpdateInstaller::changed);
    installer.probe();
    return installer.probed() || changed.wait();
}

/// Runs an install to its end
void install(UpdateInstaller& installer, const Release& release) {
    installer.install(release);
    QTRY_VERIFY_WITH_TIMEOUT(!installer.busy(), 10000);
}

}  // namespace

class UpdateInstallerTest : public QObject {
    Q_OBJECT

private slots:
    void init() {
        if (QStandardPaths::findExecutable("gpgv").isEmpty()) {
            SKIP_UNLESS_REQUIRED("no gpgv to verify a signature with");
        }
    }

    // ==========================================================================
    // Which package, and who updates it
    // ==========================================================================

    void aReleasesPackageIsNamedForTheSystem_data() {
        QTest::addColumn<QString>("os_id");
        QTest::addColumn<QString>("os_version");
        QTest::addColumn<QString>("cpu");
        QTest::addColumn<QString>("name");
        QTest::newRow("Debian") << "debian" << "13" << "x86_64"
                                << "sendspin-for-linux-1.2.3-debian13-amd64.deb";
        QTest::newRow("Ubuntu") << "ubuntu" << "26.04" << "x86_64"
                                << "sendspin-for-linux-1.2.3-ubuntu26.04-amd64.deb";
        QTest::newRow("Ubuntu on ARM") << "ubuntu" << "26.04" << "arm64"
                                       << "sendspin-for-linux-1.2.3-ubuntu26.04-arm64.deb";
        QTest::newRow("Fedora") << "fedora" << "44" << "x86_64"
                                << "sendspin-for-linux-1.2.3-fedora44-x86_64.rpm";
        QTest::newRow("Fedora on ARM") << "fedora" << "44" << "arm64"
                                       << "sendspin-for-linux-1.2.3-fedora44-aarch64.rpm";
        // Arch's package is the AUR's to update, and a derivative's libraries are its own
        QTest::newRow("Arch") << "arch" << "" << "x86_64" << "";
        QTest::newRow("Linux Mint") << "linuxmint" << "22" << "x86_64" << "";
    }
    void aReleasesPackageIsNamedForTheSystem() {
        QFETCH(QString, os_id);
        QFETCH(QString, os_version);
        QFETCH(QString, cpu);
        QTEST(update::package_name(QVersionNumber(1, 2, 3), os_id, os_version, cpu), "name");
    }

    void aptSaysWhereThePackageCameFrom() {
        QCOMPARE(update::apt_origin(LOCAL_POLICY), Origin::LOCAL);
        QCOMPARE(update::apt_origin(REPOSITORY_POLICY), Origin::REPOSITORY);
        // The repository has it, and it is not installed
        QCOMPARE(update::apt_origin("sendspin-for-linux:\n  Installed: (none)\n"
                                    "  Candidate: 9.0.0\n  Version table:\n     9.0.0 500\n"
                                    "        500 https://example.org/apt debian13/main amd64 "
                                    "Packages\n"),
                 Origin::NONE);
        QCOMPARE(update::apt_origin(""), Origin::NONE);
    }

    void dnfSaysWhereThePackageCameFrom() {
        QCOMPARE(update::dnf_origin("@commandline\n"), Origin::LOCAL);
        QCOMPARE(update::dnf_origin("<unknown>\n"), Origin::LOCAL);
        QCOMPARE(update::dnf_origin("sendspin\n"), Origin::REPOSITORY);
        QCOMPARE(update::dnf_origin(""), Origin::NONE);
    }

    // ==========================================================================
    // Where the app installs nothing itself
    // ==========================================================================

    void nothingInstallsBeforeThePackageManagerAnswered() {
        const ReleaseServer server;
        const UpdateMachine machine;
        UpdateInstaller installer(machine.system);
        QVERIFY(!installer.can_install(release_of(server)));
        QVERIFY(probe(installer));
        QCOMPARE(installer.origin(), Origin::LOCAL);
        QVERIFY(installer.can_install(release_of(server)));
    }

    void aRepositorysPackageIsLeftToTheRepository() {
        const ReleaseServer server;
        const UpdateMachine machine("exit 0", REPOSITORY_POLICY);
        UpdateInstaller installer(machine.system);
        QVERIFY(probe(installer));
        QCOMPARE(installer.origin(), Origin::REPOSITORY);
        QVERIFY(!installer.can_install(release_of(server)));
        install(installer, release_of(server));
        QCOMPARE(installer.state(), State::IDLE);
        QVERIFY(machine.ran().isEmpty());
    }

    void theFlatpakInstallsNothing() {
        const ReleaseServer server;
        UpdateMachine machine;
        machine.system.sandboxed = true;
        UpdateInstaller installer(machine.system);
        QVERIFY(probe(installer));
        QVERIFY(installer.sandboxed());
        QVERIFY(!installer.can_install(release_of(server)));
        install(installer, release_of(server));
        QCOMPARE(installer.state(), State::IDLE);
        QVERIFY(machine.ran().isEmpty());
    }

    void whatIsMissingLeavesItToTheUser_data() {
        QTest::addColumn<QString>("missing");
        QTest::newRow("no package from a package manager") << "package";
        QTest::newRow("no signing key on this machine") << "key";
        QTest::newRow("no pkexec") << "pkexec";
        QTest::newRow("an image-based system") << "image";
        QTest::newRow("no package for this release of the distro") << "distro";
        QTest::newRow("a release without a signature") << "signature";
    }
    void whatIsMissingLeavesItToTheUser() {
        QFETCH(QString, missing);
        ReleaseServer server;
        UpdateMachine machine;
        if (missing == "package") {
            machine.write("policy", "");
        } else if (missing == "key") {
            machine.system.key.clear();
        } else if (missing == "pkexec") {
            machine.system.pkexec.clear();
        } else if (missing == "image") {
            machine.system.image_based = true;
        } else if (missing == "distro") {
            machine.system.os_version = "14";
        } else {
            server.files.remove("SHA256SUMS.asc");
        }
        UpdateInstaller installer(machine.system);
        QVERIFY(probe(installer));
        QVERIFY(!installer.can_install(release_of(server)));
    }

    // ==========================================================================
    // Installing
    // ==========================================================================

    void aSignedPackageIsHandedToPkexec() {
        const ReleaseServer server;
        const UpdateMachine machine;
        UpdateInstaller installer(machine.system);
        QVERIFY(probe(installer));
        QSignalSpy installed(&installer, &UpdateInstaller::installed);
        QList<State> states;
        connect(&installer, &UpdateInstaller::changed, this, [&] {
            if (states.isEmpty() || states.last() != installer.state()) {
                states << installer.state();
            }
        });
        install(installer, release_of(server));
        QCOMPARE(states, (QList<State>{State::DOWNLOADING, State::VERIFYING, State::INSTALLING,
                                       State::INSTALLED}));
        QCOMPARE(installed.count(), 1);

        // Only the helper is run as root, and only on the three files
        const QStringList ran = machine.ran();
        QCOMPARE(ran.size(), 5);
        QCOMPARE(ran.mid(0, 2), (QStringList{machine.system.helper, "install"}));
        QVERIFY(ran[2].endsWith(QLatin1String("/") + ReleaseServer::PACKAGE));
        QVERIFY(ran[3].endsWith("/SHA256SUMS"));
        QVERIFY(ran[4].endsWith("/SHA256SUMS.asc"));
    }

    void whatTheKeyDidNotSignIsRefused_data() {
        QTest::addColumn<QString>("changed");
        QTest::newRow("another package under the release's name") << "package";
        QTest::newRow("another package, and its own sum in SHA256SUMS") << "sums";
        QTest::newRow("a signature that is none") << "signature";
        QTest::newRow("a machine that trusts another key") << "key";
    }
    void whatTheKeyDidNotSignIsRefused() {
        QFETCH(QString, changed);
        ReleaseServer server;
        UpdateMachine machine;
        if (changed == "package" || changed == "sums") {
            server.files[ReleaseServer::PACKAGE] = "Something else entirely.\n";
        }
        if (changed == "sums") {
            server.files["SHA256SUMS"] =
                "9d2bc1fb1b4b0d5d2bd4d62cd3ba29a0ca8ee48cd2a3a7b8a6a4be1c2ec2f6a4  " +
                QByteArray(ReleaseServer::PACKAGE) + "\n";
        } else if (changed == "signature") {
            server.files["SHA256SUMS.asc"] = "-----BEGIN PGP SIGNATURE-----\n\nAAAA\n=AAAA\n"
                                             "-----END PGP SIGNATURE-----\n";
        } else if (changed == "key") {
            // Armored, and nobody's
            machine.write("other.asc", "-----BEGIN PGP PUBLIC KEY BLOCK-----\n\nAAAA\n=AAAA\n"
                                       "-----END PGP PUBLIC KEY BLOCK-----\n");
            machine.system.key = machine.path("other.asc");
        }
        UpdateInstaller installer(machine.system);
        QVERIFY(probe(installer));
        QSignalSpy installed(&installer, &UpdateInstaller::installed);
        install(installer, release_of(server));
        QCOMPARE(installer.state(), State::FAILED);
        QCOMPARE(installer.failure(), Failure::SIGNATURE);
        QCOMPARE(installed.count(), 0);
        // Nobody was asked for a password
        QVERIFY(machine.ran().isEmpty());
    }

    // What runs as root verifies its own copy: a download changed after the app verified it
    // does not get installed. This pkexec changes it, then runs the helper as far as a test may.
    void aFileSwappedAfterVerifyingIsRefused() {
        const ReleaseServer server;
        const UpdateMachine machine(
            "echo swapped >\"$3\"\nexec \"$1\" verify \"$3\" \"$4\" \"$5\"");
        UpdateInstaller installer(machine.system);
        QVERIFY(probe(installer));
        QSignalSpy installed(&installer, &UpdateInstaller::installed);
        install(installer, release_of(server));
        QVERIFY(!machine.ran().isEmpty());
        QCOMPARE(installer.state(), State::FAILED);
        QCOMPARE(installer.failure(), Failure::INSTALL);
        QCOMPARE(installed.count(), 0);
    }

    void aDismissedPromptIsNoFailure() {
        const ReleaseServer server;
        const UpdateMachine machine("exit 126");
        UpdateInstaller installer(machine.system);
        QVERIFY(probe(installer));
        install(installer, release_of(server));
        QCOMPARE(installer.state(), State::IDLE);
        // And it can be asked again
        QVERIFY(installer.can_install(release_of(server)));
    }

    void aFailedInstallSaysSoAndCanBeTriedAgain() {
        const ReleaseServer server;
        const UpdateMachine machine("[ -e \"$0.again\" ] || { touch \"$0.again\"; exit 1; }");
        UpdateInstaller installer(machine.system);
        QVERIFY(probe(installer));
        install(installer, release_of(server));
        QCOMPARE(installer.state(), State::FAILED);
        QCOMPARE(installer.failure(), Failure::INSTALL);
        install(installer, release_of(server));
        QCOMPARE(installer.state(), State::INSTALLED);
    }

    void aFileLargerThanAReleasesIsNotTaken() {
        ReleaseServer server;
        server.files["SHA256SUMS"] = QByteArray(update::MAX_TEXT_BYTES + 1, 'x');
        const UpdateMachine machine;
        UpdateInstaller installer(machine.system);
        QVERIFY(probe(installer));
        install(installer, release_of(server));
        QCOMPARE(installer.state(), State::FAILED);
        QCOMPARE(installer.failure(), Failure::DOWNLOAD);
        QVERIFY(machine.ran().isEmpty());
    }

    void aFileThatDoesNotDownloadFailsIt() {
        ReleaseServer server;
        const UpdateMachine machine;
        UpdateInstaller installer(machine.system);
        QVERIFY(probe(installer));
        const Release release = release_of(server);
        server.files.remove(ReleaseServer::PACKAGE);
        install(installer, release);
        QCOMPARE(installer.state(), State::FAILED);
        QCOMPARE(installer.failure(), Failure::DOWNLOAD);
        QVERIFY(machine.ran().isEmpty());
    }
};

QTEST_GUILESS_MAIN(UpdateInstallerTest)
#include "update_installer_test.moc"
