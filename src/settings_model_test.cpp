#include "settings_model.h"

#include "autostart.h"
#include "cast/test_peer.h"
#include "device_name.h"
#include "skip_unless_required.h"
#include "update_test_server.h"

#include <QClipboard>
#include <QFile>
#include <QGuiApplication>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>

namespace server = sendspin::server;
using Step = SettingsModel::PairingStep;
using Action = SettingsModel::UpdateAction;

namespace {

/// What the model stands on, as App builds it, with nothing on the network or in the graph
struct Parts {
    /// @param speaker_port a speaker the cast engine dials; 0 for none
    /// @param release where the update checker asks, as version 1.0.0; empty for no checks
    explicit Parts(const QString& dir, uint16_t speaker_port = 0, const QUrl& release = {},
                   const UpdateInstaller::System& system = {})
        : client_persistence(dir.toStdString()),
          client({.name = SettingsModel::stored_device_name(),
                  .port = Client::free_port(0),
                  .advertise = false,
                  .unpaired_access = SettingsModel::stored_unpaired_access()},
                 &client_persistence),
          mpris(client, dir + "/art", ".settings_test"),
          servers(dir.toStdString()),
          updates(release.isEmpty() ? QVersionNumber() : QVersionNumber(1, 0, 0), release),
          installer(system) {
        if (speaker_port != 0) {
            QSettings cast_settings(dir + "/cast.ini", QSettings::IniFormat);
            cast_settings.setValue(
                "speaker_urls",
                QStringList{QStringLiteral("ws://127.0.0.1:%1/sendspin").arg(speaker_port)});
        }
        this->cast = std::make_unique<CastEngine>(dir.toStdString(), this->output);
        this->model =
            std::make_unique<SettingsModel>(this->client, *this->cast, this->outputs, this->mpris,
                                            this->servers, this->updates, this->installer);
    }

    const SpeakerRow* row(const QString& id) const {
        for (const SpeakerRow& row : this->cast->speakers()->rows()) {
            if (row.id == id) {
                return &row;
            }
        }
        return nullptr;
    }

    ClientPersistence client_persistence;
    Outputs outputs;
    Client client;
    Mpris mpris;
    SendspinOutput output;
    PairedServers servers;
    UpdateChecker updates;
    UpdateInstaller installer;
    std::unique_ptr<CastEngine> cast;
    std::unique_ptr<SettingsModel> model;
};

QSettings conf() {
    return QSettings(QStringLiteral("sendspin"), QStringLiteral("sendspin"));
}

}  // namespace

class SettingsModelTest : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        // Before anything reads a setting: QSettings fixes its paths at first use
        qputenv("XDG_CONFIG_HOME", this->config_.path().toUtf8());
    }

    void init() {
        conf().clear();
        autostart::set_enabled(false);
    }

    // ==========================================================================
    // This PC
    // ==========================================================================

    void defaultsWithNothingStored() {
        const QTemporaryDir dir;
        Parts parts(dir.path());
        const SettingsModel& model = *parts.model;
        QCOMPARE(model.device_name(), device_name());
        QVERIFY(!model.device_name_waits());
        QCOMPARE(model.output(), QString());
        QVERIFY(!model.open_at_login());
        QVERIFY(model.unpaired_access());
        QVERIFY(model.media_controls());
        // Looking writes nothing
        QVERIFY(conf().allKeys().isEmpty());
    }

    void changesAreKeptInTheirKeysAndReadAtTheNextStart() {
        const QTemporaryDir dir;
        {
            Parts parts(dir.path());
            SettingsModel& model = *parts.model;
            QSignalSpy unpaired(&model, &SettingsModel::unpairedAccessChanged);
            QSignalSpy controls(&model, &SettingsModel::mediaControlsChanged);
            model.setDeviceName("  Den PC ");
            model.setOutput("alsa_output.usb-dac");
            model.setUnpairedAccess(false);
            model.setMediaControls(false);
            QCOMPARE(unpaired.size(), 1);
            QCOMPARE(controls.size(), 1);
            // The same again is no change
            model.setUnpairedAccess(false);
            QCOMPARE(unpaired.size(), 1);
        }
        QCOMPARE(conf().value("client/name").toString(), "Den PC");
        QCOMPARE(conf().value("player/output").toString(), "alsa_output.usb-dac");
        QCOMPARE(conf().value("client/unpaired_access").toBool(), false);
        QCOMPARE(conf().value("mpris/enabled").toBool(), false);

        Parts parts(dir.path());
        QCOMPARE(parts.client.name(), "Den PC");
        QCOMPARE(parts.model->device_name(), "Den PC");
        QVERIFY(!parts.model->device_name_waits());
        QCOMPARE(parts.model->output(), "alsa_output.usb-dac");
        QVERIFY(!parts.model->unpaired_access());
        QVERIFY(!parts.model->media_controls());
    }

    void aNewDeviceNameWaitsForTheNextStart() {
        const QTemporaryDir dir;
        Parts parts(dir.path());
        SettingsModel& model = *parts.model;
        QSignalSpy changed(&model, &SettingsModel::deviceNameChanged);
        model.setDeviceName("Den PC");
        QCOMPARE(changed.size(), 1);
        QCOMPARE(model.device_name(), "Den PC");
        QVERIFY(model.device_name_waits());

        // Emptied, it is the hostname again, and not kept
        model.setDeviceName("");
        QCOMPARE(model.device_name(), device_name());
        QVERIFY(!conf().contains("client/name"));
        QVERIFY(!model.device_name_waits());
    }

    void outputsOfferTheDefaultFirstAndKeepAnUnpluggedChoice() {
        const QTemporaryDir dir;
        Parts parts(dir.path());
        SettingsModel& model = *parts.model;
        // No graph is watched here: only the default
        QCOMPARE(model.outputs().size(), 1);
        QCOMPARE(model.outputs().first().toMap().value("name").toString(), QString());

        QSignalSpy listed(&model, &SettingsModel::outputsChanged);
        model.setOutput("usb-dac");
        QCOMPARE(listed.size(), 1);
        QCOMPARE(model.outputs().size(), 2);
        QCOMPARE(model.outputs().last().toMap().value("name").toString(), "usb-dac");
        QCOMPARE(model.outputs().last().toMap().value("description").toString(),
                 "usb-dac (not connected)");

        // Back to the default: the key goes
        model.setOutput("");
        QVERIFY(!conf().contains("player/output"));
        QCOMPARE(model.outputs().size(), 1);

        // The Sendspin output is never offered, even written into the file by hand
        model.setOutput("sendspin");
        QCOMPARE(model.outputs().size(), 1);
    }

    void openAtLoginWritesAndRemovesTheEntry() {
        const QTemporaryDir dir;
        Parts parts(dir.path());
        SettingsModel& model = *parts.model;
        QSignalSpy changed(&model, &SettingsModel::openAtLoginChanged);
        model.setOpenAtLogin(true);
        QVERIFY(model.open_at_login());
        QVERIFY(QFile::exists(autostart::path()));
        model.setOpenAtLogin(false);
        QVERIFY(!model.open_at_login());
        QVERIFY(!QFile::exists(autostart::path()));
        QCOMPARE(changed.size(), 2);
    }

    // ==========================================================================
    // Updates: a stand-in for GitHub's release, and for the machine's package manager
    // ==========================================================================

    void checkingForUpdatesIsOnUntilSwitchedOff() {
        const QTemporaryDir dir;
        Parts parts(dir.path());
        SettingsModel& model = *parts.model;
        QVERIFY(model.check_for_updates());
        QCOMPARE(model.update_status(), QString());
        QCOMPARE(model.update_action(), Action::None);

        QSignalSpy changed(&model, &SettingsModel::updateChanged);
        model.setCheckForUpdates(false);
        QCOMPARE(changed.count(), 1);
        QVERIFY(!model.check_for_updates());
        QCOMPARE(conf().value("updates/check").toBool(), false);
    }

    void aNewerReleaseShowsWithWhoInstallsIt_data() {
        QTest::addColumn<QString>("installed");
        QTest::addColumn<QString>("status");
        QTest::addColumn<Action>("action");
        QTest::newRow("from a downloaded package")
            << "file" << "Sendspin 9.0.0 is available." << Action::Install;
        QTest::newRow("from the repository")
            << "repository" << "Sendspin 9.0.0 is available. Your system's updates install it."
            << Action::None;
        QTest::newRow("as the Flatpak")
            << "flatpak" << "Sendspin 9.0.0 is available. Flatpak installs it." << Action::Open;
        QTest::newRow("from a package, with no signing key here")
            << "no key" << "Sendspin 9.0.0 is available." << Action::Open;
    }
    void aNewerReleaseShowsWithWhoInstallsIt() {
        QFETCH(QString, installed);
        const ReleaseServer server;
        UpdateMachine machine("exit 0",
                              installed == "repository" ? REPOSITORY_POLICY : LOCAL_POLICY);
        machine.system.sandboxed = installed == "flatpak";
        if (installed == "no key") {
            machine.system.key.clear();
        }
        const QTemporaryDir dir;
        Parts parts(dir.path(), 0, server.url(), machine.system);
        SettingsModel& model = *parts.model;
        parts.updates.start();
        parts.installer.probe();
        QTRY_VERIFY(parts.installer.probed() && parts.updates.update());
        QTEST(model.update_status(), "status");
        QTEST(model.update_action(), "action");
        QCOMPARE(model.update_page(), server.url("page"));
    }

    void installingAnUpdateShowsHowItGoes() {
        if (QStandardPaths::findExecutable("gpgv").isEmpty()) {
            SKIP_UNLESS_REQUIRED("no gpgv to verify a signature with");
        }
        const ReleaseServer server;
        const UpdateMachine machine;
        const QTemporaryDir dir;
        Parts parts(dir.path(), 0, server.url(), machine.system);
        SettingsModel& model = *parts.model;
        parts.updates.start();
        parts.installer.probe();
        QTRY_COMPARE(model.update_action(), Action::Install);

        QStringList shown;
        connect(&model, &SettingsModel::updateChanged, this, [&] {
            if (shown.isEmpty() || shown.last() != model.update_status()) {
                shown << model.update_status();
            }
            // Nothing to click while it runs, or once it is in
            QCOMPARE(model.update_action(), Action::None);
        });
        model.installUpdate();
        QTRY_COMPARE_WITH_TIMEOUT(parts.installer.state(), UpdateInstaller::State::INSTALLED,
                                  10000);
        QVERIFY(shown.first().startsWith("Downloading Sendspin 9.0.0… "));
        QCOMPARE(shown.mid(shown.indexOf("Checking the download…")),
                 (QStringList{"Checking the download…", "Installing Sendspin 9.0.0…",
                              "Sendspin 9.0.0 is installed. Restarting…"}));
        QVERIFY(!machine.ran().isEmpty());
    }

    void anUpdateThatIsRefusedSaysWhy() {
        if (QStandardPaths::findExecutable("gpgv").isEmpty()) {
            SKIP_UNLESS_REQUIRED("no gpgv to verify a signature with");
        }
        ReleaseServer server;
        server.files[ReleaseServer::PACKAGE] = "Something else entirely.\n";
        const UpdateMachine machine;
        const QTemporaryDir dir;
        Parts parts(dir.path(), 0, server.url(), machine.system);
        SettingsModel& model = *parts.model;
        parts.updates.start();
        parts.installer.probe();
        QTRY_COMPARE(model.update_action(), Action::Install);
        model.installUpdate();
        QTRY_COMPARE_WITH_TIMEOUT(parts.installer.state(), UpdateInstaller::State::FAILED, 10000);
        QCOMPARE(model.update_status(), "The download of Sendspin 9.0.0 isn't signed by Sendspin's "
                                        "key, so it was not installed.");
        QVERIFY(machine.ran().isEmpty());
        // To try again
        QCOMPARE(model.update_action(), Action::Install);
    }

    // ==========================================================================
    // Speakers: a real sendspin-cpp player on the loopback interface
    // ==========================================================================

    void pairsASpeakerByWhatItOffers_data() {
        QTest::addColumn<Peer::Offer>("offer");
        QTest::addColumn<Step>("asks");
        QTest::addColumn<int>("digits");
        QTest::newRow("dynamic code") << Peer::Offer::DynamicCode << Step::EnterCode << 6;
        QTest::newRow("static code") << Peer::Offer::StaticCode << Step::EnterCode << 8;
        QTest::newRow("token") << Peer::Offer::TokenOnly << Step::EnterToken << 6;
    }

    void pairsASpeakerByWhatItOffers() {
        QFETCH(Peer::Offer, offer);
        QFETCH(Step, asks);
        QFETCH(int, digits);

        const QTemporaryDir dir;
        const uint16_t port = free_port();
        Peer peer(dir.path() + "/peer", port, false, "Kitchen", offer);
        QVERIFY(peer.started);
        Parts parts(dir.path(), port);
        SettingsModel& model = *parts.model;
        QVERIFY(parts.cast->start(free_port(), false));

        QTRY_VERIFY(parts.row(peer.id()) != nullptr && parts.row(peer.id())->can_pair);
        QCOMPARE(model.paired_speaker_count(), 0);
        QCOMPARE(model.unpaired_speaker_count(), 1);

        model.pair(peer.id());
        QCOMPARE(model.pairing_name(), "Kitchen");
        QTRY_COMPARE(model.pairing_step(), asks);
        if (asks == Step::EnterCode) {
            QCOMPARE(model.pairing_digits(), digits);
        }
        if (offer == Peer::Offer::StaticCode) {
            // A code anyone could have read off it: the speaker wants its own button pressed,
            // and the dialog says so beside the field
            QTRY_VERIFY(peer.waits_for_button);
            QTRY_VERIFY(!model.pairing_message().isEmpty());
            QCOMPARE(model.pairing_step(), Step::EnterCode);
            peer.allow_pairing();
        }
        switch (offer) {
            case Peer::Offer::DynamicCode:
                QTRY_VERIFY(!peer.pairing_code.isEmpty());
                model.submitPairingCode(peer.pairing_code);
                break;
            case Peer::Offer::StaticCode:
                model.submitPairingCode(Peer::STATIC_CODE);
                break;
            case Peer::Offer::TokenOnly:
                model.submitPairingToken(peer.token());
                break;
        }
        QCOMPARE(model.pairing_step(), Step::Checking);

        QTRY_COMPARE(model.pairing_step(), Step::Idle);
        QTRY_VERIFY(parts.row(peer.id())->paired);
        QTRY_COMPARE(model.paired_speaker_count(), 1);
        QCOMPARE(model.unpaired_speaker_count(), 0);
        // Paired from Settings, it is not added to the group
        QVERIFY(!parts.row(peer.id())->in_group);
        QCOMPARE(parts.row(peer.id())->row.check, speaker_menu::Check::EMPTY);

        model.forget(peer.id());
        QTRY_VERIFY(parts.row(peer.id()) != nullptr && !parts.row(peer.id())->paired);
        QTRY_COMPARE(model.paired_speaker_count(), 0);
    }

    void aWrongCodeIsAskedForAgain() {
        const QTemporaryDir dir;
        const uint16_t port = free_port();
        Peer peer(dir.path() + "/peer", port, false);
        Parts parts(dir.path(), port);
        SettingsModel& model = *parts.model;
        QVERIFY(parts.cast->start(free_port(), false));
        QTRY_VERIFY(parts.row(peer.id()) != nullptr && parts.row(peer.id())->can_pair);

        model.pair(peer.id());
        QTRY_COMPARE(model.pairing_step(), Step::EnterCode);
        QVERIFY(model.pairing_message().isEmpty());
        QTRY_COMPARE(peer.pairing_code.size(), 6);
        QString wrong = peer.pairing_code;
        wrong[0] = wrong[0] == u'0' ? u'1' : u'0';
        model.submitPairingCode(wrong);
        QCOMPARE(model.pairing_step(), Step::Checking);
        QTRY_COMPARE(model.pairing_step(), Step::EnterCode);
        QVERIFY(!model.pairing_message().isEmpty());

        // Closing the dialog ends the pairing, and shows no failure for it
        model.cancelPairing();
        QCOMPARE(model.pairing_step(), Step::Idle);

        // Pair… again at once: the end of the pairing just cancelled is not this one's
        model.pair(peer.id());
        QCOMPARE(model.pairing_step(), Step::Starting);
        QTRY_COMPARE(model.pairing_step(), Step::EnterCode);
        QTest::qWait(300);
        QCOMPARE(model.pairing_step(), Step::EnterCode);

        model.cancelPairing();
        QTest::qWait(300);
        QCOMPARE(model.pairing_step(), Step::Idle);
        QVERIFY(!parts.row(peer.id())->paired);
    }

    void aPastedTokenPairsTheSpeakerItIsFor() {
        const QTemporaryDir dir;
        const uint16_t port = free_port();
        Peer peer(dir.path() + "/peer", port, false);
        // Another speaker's token, which is none here
        const Peer stranger(dir.path() + "/stranger", free_port(), false, "Elsewhere");
        Parts parts(dir.path(), port);
        SettingsModel& model = *parts.model;
        QVERIFY(parts.cast->start(free_port(), false));
        QTRY_VERIFY(parts.row(peer.id()) != nullptr && parts.row(peer.id())->can_pair);

        QVERIFY(!model.pairByToken("not a token"));
        QVERIFY(!model.pairByToken(stranger.token()));
        QCOMPARE(model.pairing_step(), Step::Idle);

        // As pasted: with the space that came along
        QVERIFY(model.pairByToken(peer.token() + " \n"));
        QCOMPARE(model.pairing_name(), "Kitchen");
        QTRY_COMPARE(model.pairing_step(), Step::Idle);
        QTRY_VERIFY(parts.row(peer.id())->paired);
        QVERIFY(!parts.row(peer.id())->in_group);
    }

    void aSpeakerThatCannotBePairedSaysSo() {
        const QTemporaryDir dir;
        Parts parts(dir.path());
        SettingsModel& model = *parts.model;
        model.pair("no-such-speaker");
        QCOMPARE(model.pairing_step(), Step::Failed);
        QVERIFY(!model.pairing_message().isEmpty());
        model.cancelPairing();
        QCOMPARE(model.pairing_step(), Step::Idle);
    }

    // ==========================================================================
    // Music Assistant: a real sendspin-server-cpp pairs with the PC
    // ==========================================================================

    void showsTheCodeAServerAsksForThenTheServer_data() {
        QTest::addColumn<bool>("qr");
        QTest::newRow("digits") << false;
        QTest::newRow("QR code") << true;
    }

    void showsTheCodeAServerAsksForThenTheServer() {
        QFETCH(bool, qr);
        const QTemporaryDir dir;
        Parts parts(dir.path());
        SettingsModel& model = *parts.model;
        QCOMPARE(model.server_status(), "No server is connected");
        QVERIFY(model.paired_servers().isEmpty());
        QVERIFY(parts.client.start());

        server::ServerListener listener;
        ServerPersistence persistence((dir.path() + "/their-server").toStdString());
        server::ServerConfig config;
        config.name = "Music Assistant";
        config.listen_port = 0;
        config.advertise = false;
        config.browse = false;
        config.operator_can_scan_qr = qr;
        server::SendspinServer their_server;
        QCOMPARE(their_server.start(config, &persistence, &listener), server::StartResult::OK);
        QVERIFY(their_server.connect_url("ws://127.0.0.1:" + std::to_string(parts.client.port()) +
                                         "/sendspin"));
        QTRY_COMPARE(model.server_status(), "Connected to Music Assistant");

        const std::string id = parts.client.client_id().toStdString();
        QVERIFY(their_server.begin_pairing(
            id, {server::PairMethod::DYNAMIC_PAIRING_CODE,
                 qr ? server::CodeFormat::QR_CODE : server::CodeFormat::DIGITS}));
        QTRY_VERIFY(!parts.client.pairing_code().isEmpty());
        QCOMPARE(model.server_status(), "Music Assistant is pairing with this PC");
        if (qr) {
            // The token is drawn, never spelled out
            QVERIFY(model.pairing_code().isEmpty());
            QVERIFY(model.pairing_qr().startsWith("image://qr/"));
            QVERIFY(!model.pairing_qr().contains(parts.client.pairing_code()));
            QVERIFY(!model.pairing_qr_image().isNull());
        } else {
            const QString code = parts.client.pairing_code();
            QCOMPARE(model.pairing_code(), code.first(3) + "-" + code.last(3));
            QVERIFY(model.pairing_qr().isEmpty());
            QVERIFY(model.pairing_qr_image().isNull());
        }

        QSignalSpy listed(&model, &SettingsModel::pairedServersChanged);
        QVERIFY(their_server.submit_pairing_code(id, parts.client.pairing_code().toStdString()));
        QTRY_COMPARE(listed.size(), 1);
        QVERIFY(model.pairing_code().isEmpty());
        QVERIFY(model.pairing_qr().isEmpty());
        QCOMPARE(model.paired_servers().size(), 1);
        QCOMPARE(model.paired_servers().first().toMap().value("name").toString(),
                 "Music Assistant");

        // The record is the app's own, and is there at the next start
        const PairedServers kept(dir.path().toStdString());
        QCOMPARE(kept.list().size(), 1);
        QCOMPARE(kept.list().first().name, "Music Assistant");
        QVERIFY(!kept.list().first().id.isEmpty());

        their_server.stop();
    }

    void copiesThePairingTokenToTheClipboard() {
        const QTemporaryDir dir;
        Parts parts(dir.path());
        // Before the client's first start there is none
        QVERIFY(!parts.model->copyPairingToken());

        QVERIFY(parts.client.start());
        QVERIFY(parts.model->copyPairingToken());
        const QString copied = QGuiApplication::clipboard()->text();
        QVERIFY(copied.startsWith("SP:0"));
        QCOMPARE(copied, parts.client.pairing_token());
        // Nowhere else: no property of the model carries it
        const QMetaObject* meta = parts.model->metaObject();
        for (int i = 0; i < meta->propertyCount(); i++) {
            QVERIFY(!meta->property(i).read(parts.model.get()).toString().contains(copied));
        }
    }

    void unpairedAccessReachesTheRunningClient() {
        const QTemporaryDir dir;
        Parts parts(dir.path());
        QVERIFY(parts.client.start());

        // Changed while it runs: the next server to connect is told so in the client's hello
        parts.model->setUnpairedAccess(false);
        QCOMPARE(conf().value("client/unpaired_access").toBool(), false);

        server::ServerListener listener;
        ServerPersistence persistence((dir.path() + "/their-server").toStdString());
        server::ServerConfig config;
        config.listen_port = 0;
        config.advertise = false;
        config.browse = false;
        server::SendspinServer their_server;
        QCOMPARE(their_server.start(config, &persistence, &listener), server::StartResult::OK);
        QVERIFY(their_server.connect_url("ws://127.0.0.1:" + std::to_string(parts.client.port()) +
                                         "/sendspin"));
        const auto offered = [&their_server]() -> std::optional<bool> {
            const auto clients = their_server.clients();
            return clients.size() == 1 && clients.front().connected
                       ? std::optional(clients.front().unpaired_access)
                       : std::nullopt;
        };
        QTRY_COMPARE(offered(), std::optional(false));

        their_server.stop();
    }

    void aQrCodeIsDrawnWithItsQuietZone() {
        // Version 1, 21 modules, and 4 clear ones around
        const QImage image = SettingsModel::qr_image("SENDSPIN");
        QCOMPARE(image.size(), QSize(29, 29));
        QCOMPARE(image.pixel(0, 0), qRgb(255, 255, 255));
        QCOMPARE(image.pixel(3, 3), qRgb(255, 255, 255));
        // The corner of a finder pattern, and the gap inside its ring
        QCOMPARE(image.pixel(4, 4), qRgb(0, 0, 0));
        QCOMPARE(image.pixel(5, 5), qRgb(255, 255, 255));
        QCOMPARE(image.pixel(6, 6), qRgb(0, 0, 0));
        // A pairing token fits
        QVERIFY(!SettingsModel::qr_image(QString(107, 'A')).isNull());
        QVERIFY(SettingsModel::qr_image(QString(5000, 'A')).isNull());
    }

private:
    QTemporaryDir config_;
};

QTEST_MAIN(SettingsModelTest)
#include "settings_model_test.moc"
