#include "picker/picker.h"

#include "picker/picker_window.h"

#include <QQuickItem>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <sendspin/client.h>
#include <sendspin/player_role.h>

Q_DECLARE_METATYPE(speaker_menu::Output)
Q_DECLARE_METATYPE(speaker_menu::Check)

using speaker_menu::Check;
using speaker_menu::Connection;
using speaker_menu::Status;

namespace {

uint16_t free_port() {
    QTcpServer server;
    server.listen(QHostAddress::LocalHost, 0);
    return server.serverPort();
}

/// The app's side: everything the picker is a view over, none of it started, so there is no
/// PipeWire, no server and no network unless a test starts them
struct Rig {
    Rig() {
        // Each test starts from the first run's choices
        QSettings(QStringLiteral("sendspin"), QStringLiteral("sendspin")).remove("picker");
    }

    /// Rows as the engine would publish them, each made by speaker_menu::row()
    void list(const std::vector<std::pair<QString, speaker_menu::Output>>& speakers) {
        std::vector<SpeakerRow> rows;
        for (const auto& [name, output] : speakers) {
            SpeakerRow row;
            row.id = name;
            row.name = name;
            row.in_group = output.in_group;
            row.plays = output.plays;
            row.can_play = speaker_menu::can_play(output);
            row.row = speaker_menu::row(output, {});
            rows.push_back(row);
        }
        this->cast.speakers()->set_rows(std::move(rows));
    }

    QQuickItem* item(const QString& name) {
        this->window.show();
        return find(this->window.window()->contentItem(), name);
    }

    /// Through the items as they are drawn: a Repeater's are not its QObject children
    static QQuickItem* find(QQuickItem* under, const QString& name) {
        if (under == nullptr || under->objectName() == name) {
            return under;
        }
        for (QQuickItem* child : under->childItems()) {
            if (QQuickItem* found = find(child, name)) {
                return found;
            }
        }
        return nullptr;
    }

    QTemporaryDir dir;
    ClientPersistence persistence{dir.path().toStdString()};
    Outputs outputs;
    Client client{{.name = "Desk", .port = Client::free_port(), .advertise = false}, &persistence};
    SendspinOutput output;
    CastEngine cast{dir.path().toStdString(), output};
    Picker picker{cast, client, outputs};
    PickerWindow window{picker, cast};
};

speaker_menu::Output added(Connection connection, bool plays = false) {
    return {.in_group = true, .plays = plays, .connection = connection};
}

speaker_menu::Output found(Connection connection) {
    return {.connection = connection};
}

/// A Sendspin speaker on the loopback interface: sendspin-cpp's client with a player role
class Speaker : public sendspin::PlayerRoleListener,
                public sendspin::SendspinClientListener,
                public sendspin::SendspinNetworkProvider {
public:
    Speaker(const QString& dir, uint16_t port) : persistence_(dir.toStdString()) {
        sendspin::SendspinClientConfig config;
        config.name = "Kitchen";
        config.server_port = port;
        this->client_ = std::make_unique<sendspin::SendspinClient>(config);
        this->client_->set_persistence_provider(&this->persistence_);
        this->client_->set_network_provider(this);
        this->client_->set_listener(this);
        this->client_->set_unpaired_access_enabled(true);
        sendspin::PlayerRoleConfig player;
        player.audio_formats = {{sendspin::SendspinCodecFormat::PCM, 2, 48000, 16}};
        this->client_->add_player(std::move(player)).set_listener(this);
        this->started = this->client_->start();
        QObject::connect(&this->pump_, &QTimer::timeout, [this] { this->client_->loop(); });
        this->pump_.start(2);
    }
    ~Speaker() override {
        this->pump_.stop();
        this->client_->stop();
    }

    QString id() const {
        return QString::fromStdString(this->client_->client_id());
    }

    size_t on_audio_output(std::span<const uint8_t> pcm, const sendspin::AudioFormat& /*format*/,
                           uint32_t /*timeout_ms*/) override {
        return pcm.size();
    }
    bool is_network_ready() override {
        return true;
    }

    bool started{false};

private:
    ClientPersistence persistence_;
    std::unique_ptr<sendspin::SendspinClient> client_;
    QTimer pump_;
};

}  // namespace

class PickerTest : public QObject {
    Q_OBJECT

private slots:
    // ==========================================================================
    // The speakers' rows: docs/SPEC.md, "A speaker's status"
    // ==========================================================================

    void aSpeakersRowShowsItsState_data() {
        QTest::addColumn<speaker_menu::Output>("output");
        QTest::addColumn<Check>("check");
        QTest::addColumn<QString>("status");
        QTest::addColumn<QString>("heard");

        QTest::newRow("plays") << added(Connection::CONNECTED, true) << Check::FILLED << ""
                               << "Kitchen, playing";
        QTest::newRow("not added") << found(Connection::CONNECTED) << Check::EMPTY << ""
                                   << "Kitchen, not added";
        QTest::newRow("waits, not added")
            << found(Connection::WAITING) << Check::EMPTY << "" << "Kitchen, not added";
        QTest::newRow("connecting") << added(Connection::CONNECTING) << Check::SPINNER << ""
                                    << "Kitchen, added, connecting";
        QTest::newRow("unavailable") << added(Connection::UNAVAILABLE) << Check::GREY
                                     << "Unavailable" << "Kitchen, added, can't play, Unavailable";
        QTest::newRow("gone from the network")
            << speaker_menu::Output{.in_group = true, .is_discovered = false} << Check::GREY
            << "Unavailable" << "Kitchen, added, can't play, Unavailable";
        QTest::newRow("in use elsewhere")
            << added(Connection::IN_USE_ELSEWHERE) << Check::GREY << "In use elsewhere"
            << "Kitchen, added, can't play, In use elsewhere";
        QTest::newRow("in use elsewhere, not added")
            << found(Connection::IN_USE_ELSEWHERE) << Check::EMPTY << "In use elsewhere"
            << "Kitchen, not added, In use elsewhere";
        QTest::newRow("disconnected")
            << added(Connection::DISCONNECTED) << Check::GREY << "Disconnected"
            << "Kitchen, added, can't play, Disconnected";
        QTest::newRow("can't connect")
            << added(Connection::CANT_CONNECT) << Check::GREY << "Can't connect"
            << "Kitchen, added, can't play, Can't connect";
        QTest::newRow("can't play") << added(Connection::CANT_PLAY) << Check::GREY << "Can't play"
                                    << "Kitchen, added, can't play, Can't play";
        QTest::newRow("needs pairing")
            << added(Connection::NEEDS_PAIRING) << Check::GREY << "Needs pairing"
            << "Kitchen, added, can't play, Needs pairing";
        QTest::newRow("needs pairing, not added")
            << found(Connection::NEEDS_PAIRING) << Check::EMPTY << "Needs pairing"
            << "Kitchen, not added, Needs pairing";
        QTest::newRow("needs pairing again")
            << added(Connection::NEEDS_PAIRING_AGAIN) << Check::GREY << "Needs pairing again"
            << "Kitchen, added, can't play, Needs pairing again";
        QTest::newRow("identity changed")
            << added(Connection::IDENTITY_CHANGED) << Check::GREY << "Identity changed"
            << "Kitchen, added, can't play, Identity changed";
    }

    void aSpeakersRowShowsItsState() {
        QFETCH(speaker_menu::Output, output);
        QFETCH(Check, check);
        QFETCH(QString, status);
        QFETCH(QString, heard);

        Rig rig;
        rig.list({{"Kitchen", output}});
        QQuickItem* row = rig.item("speakerRow");
        QVERIFY(row != nullptr);
        QCOMPARE(row->property("check").toInt(), static_cast<int>(check));
        QCOMPARE(row->property("statusText").toString(), status);
        QCOMPARE(row->property("accessibleName").toString(), heard);

        QQuickItem* label = Rig::find(row, "status");
        QCOMPARE(label->isVisible(), !status.isEmpty() && rig.picker.has_outputs());
        // A fault is marked; another server's choice, or what adding will ask for, is not
        const bool fault = speaker_menu::row(output, {}).warning;
        QCOMPARE(label->property("text").toString(), fault ? "⚠ " + status : status);

        QVERIFY(Rig::find(row, "check")->isEnabled());
    }

    void aRowFollowsItsSpeaker() {
        Rig rig;
        rig.list({{"Kitchen", added(Connection::CONNECTING)}});
        QQuickItem* row = rig.item("speakerRow");
        QCOMPARE(row->property("check").toInt(), static_cast<int>(Check::SPINNER));
        rig.list({{"Kitchen", added(Connection::CONNECTED, true)}});
        QCOMPARE(rig.item("speakerRow")->property("check").toInt(),
                 static_cast<int>(Check::FILLED));
    }

    // ==========================================================================
    // The other rows: "Rows"
    // ==========================================================================

    void withoutPipeWireThereIsANoticeAndNoRows() {
        Rig rig;
        QVERIFY(!rig.picker.has_outputs());
        QVERIFY(rig.picker.notice().contains("PipeWire"));
        QVERIFY(rig.item("notice")->isVisible());
        QVERIFY(!rig.item("rows")->isVisible());
    }

    void looksForSpeakersUntilOneIsFound() {
        Rig rig;
        QVERIFY(rig.picker.looking());
        rig.list({{"Kitchen", found(Connection::WAITING)}});
        QVERIFY(!rig.picker.looking());
    }

    void thisPcsOutputIsTickedAndStaysWhenItIsAllThatPlays() {
        Rig rig;
        QCOMPARE(rig.picker.local_check(), Picker::Check::Grey);  // no PipeWire: nothing to play on
        QVERIFY(!rig.picker.local_clickable());
        // Beside a speaker it can go
        rig.list({{"Kitchen", added(Connection::CONNECTED, true)}});
        QVERIFY(rig.picker.local_clickable());
    }

    void theDotSaysWhichInWords() {
        QCOMPARE(Picker::dot_text(Picker::Dot::Plays), "Sendspin is playing");
        QCOMPARE(Picker::dot_text(Picker::Dot::InUse),
                 "Sendspin is the output, and nothing is playing");
        QCOMPARE(Picker::dot_text(Picker::Dot::NotInUse), "This PC plays directly");

        Rig rig;
        QCOMPARE(rig.picker.dot(), Picker::Dot::NotInUse);
    }

    // "The server row"
    void theServersCaptionSaysWhatToDo() {
        Picker::Server server{.name = "Music Assistant", .connected = true};
        QCOMPARE(Picker::server_caption(server, "Desk"),
                 "Pair this PC in Music Assistant, then enter the code that appears here.");
        server.code = "123456";
        QCOMPARE(Picker::server_caption(server, "Desk"), "Enter 123-456 in Music Assistant.");
        server.code.clear();
        server.paired = true;
        QCOMPARE(Picker::server_caption(server, "Desk"), "");
        server.chosen = true;
        QCOMPARE(Picker::server_caption(server, "Desk"),
                 "Select Desk as an input source in Music Assistant to start playing.");
        server.streaming = true;
        QCOMPARE(Picker::server_caption(server, "Desk"),
                 "Choose speakers, groups and volume in Music Assistant.");
        server.connected = false;
        QCOMPARE(Picker::server_caption(server, "Desk"),
                 "It reconnects by itself when it's back on the network.");
    }

    void noServerRowUntilOneConnectsOrPaired() {
        Rig rig;
        QVERIFY(!rig.picker.server_shown());
        QVERIFY(!rig.item("serverRow")->isVisible());
        QVERIFY(!rig.picker.server_choosable());
    }

    // ==========================================================================
    // The group's volume: "Volume"
    // ==========================================================================

    void theSliderShowsWithASpeakerInTheGroup() {
        Rig rig;
        rig.list({{"Kitchen", found(Connection::CONNECTED)}});
        QVERIFY(!rig.item("volume")->isVisible());
        rig.list({{"Kitchen", added(Connection::CONNECTED, true)}});
        QVERIFY(rig.picker.volume_shown());
    }

    void theSliderIsTheGroupsVolume() {
        Rig rig;
        QQuickItem* slider = rig.item("volumeSlider");
        // A change from elsewhere shows
        rig.cast.setGroupVolume(40);
        QCOMPARE(slider->property("value").toInt(), 40);
        // Moving it sets the group's
        slider->setProperty("value", 65);
        QVERIFY(QMetaObject::invokeMethod(slider, "moved"));
        QCOMPARE(rig.cast.group_volume(), 65);
        QCOMPARE(slider->property("value").toInt(), 65);

        QQuickItem* mute = rig.item("mute");
        rig.cast.setGroupMuted(true);
        QVERIFY(mute->property("checked").toBool());
        QVERIFY(QMetaObject::invokeMethod(mute, "toggle"));
        QVERIFY(QMetaObject::invokeMethod(mute, "toggled"));
        QVERIFY(!rig.cast.group_muted());
    }

    // ==========================================================================
    // Choices: "What ticking does"
    // ==========================================================================

    // A real speaker on the loopback interface, and the engine's own server
    void tickingAFoundSpeakerPutsItInTheGroup() {
        const QTemporaryDir speaker_dir;
        const uint16_t port = free_port();
        Speaker speaker(speaker_dir.path(), port);
        QVERIFY(speaker.started);

        Rig rig;
        QSettings settings(rig.dir.path() + "/cast.ini", QSettings::IniFormat);
        settings.setValue("speaker_urls",
                          QStringList{QStringLiteral("ws://127.0.0.1:%1/sendspin").arg(port)});
        settings.sync();
        CastEngine cast(rig.dir.path().toStdString(), rig.output);
        Picker picker(cast, rig.client, rig.outputs);
        QVERIFY(cast.start(free_port(), false));

        const auto row = [&cast]() -> const SpeakerRow* {
            const auto& rows = cast.speakers()->rows();
            return rows.empty() ? nullptr : &rows.front();
        };
        QTRY_VERIFY(row() != nullptr && row()->id == speaker.id());
        QCOMPARE(row()->row.check, Check::EMPTY);
        QCOMPARE(picker.route(), speaker_menu::Route::DIRECT);

        picker.toggle(speaker.id());
        QTRY_COMPARE(row()->row.check, Check::FILLED);
        QCOMPARE(picker.route(), speaker_menu::Route::SPEAKERS);
        QVERIFY(picker.volume_shown());

        // The last speaker out: this PC's own output takes over, played directly
        picker.toggle(speaker.id());
        QTRY_COMPARE(row()->row.check, Check::EMPTY);
        QCOMPARE(picker.route(), speaker_menu::Route::DIRECT);
        QVERIFY(picker.local_check() != Picker::Check::Empty);
    }

    void theSourceIsSilencedUnlessTheServerIsChosen() {
        Rig rig;
        QCOMPARE(rig.picker.route(), speaker_menu::Route::DIRECT);
        // Not paired with a server: there is none to choose
        rig.picker.chooseServer();
        QCOMPARE(rig.picker.route(), speaker_menu::Route::DIRECT);
        QVERIFY(!rig.picker.server_chosen());
    }

    void theChoiceIsKept() {
        {
            Rig rig;
            rig.list({{"Kitchen", added(Connection::CONNECTED, true)}});
            // This PC's own output leaves the group
            rig.picker.toggle(rig.picker.local_id());
            QCOMPARE(rig.picker.local_check(), Picker::Check::Empty);
            const Picker again(rig.cast, rig.client, rig.outputs);
            QCOMPARE(again.local_check(), Picker::Check::Empty);
        }
    }

    // A speaker's own button took the last one out of the group
    void thisPcsOutputTakesOverWhenTheLastSpeakerLeavesByItself() {
        Rig rig;
        rig.list({{"Kitchen", added(Connection::CONNECTED, true)}});
        rig.picker.toggle(rig.picker.local_id());
        QCOMPARE(rig.picker.local_check(), Picker::Check::Empty);
        rig.list({{"Kitchen", found(Connection::CONNECTED)}});
        QVERIFY(rig.picker.local_check() != Picker::Check::Empty);
        QCOMPARE(rig.picker.route(), speaker_menu::Route::DIRECT);
    }

    // ==========================================================================
    // The tray: "UI"
    // ==========================================================================

    void theStatusLine() {
        QCOMPARE(Picker::status_line({}, {}, {}), "Not in use");
        QCOMPARE(Picker::status_line({"Kitchen"}, {}, {}), "Playing on Kitchen");
        QCOMPARE(Picker::status_line({"Kitchen", "Office", "Patio"}, {}, {}),
                 "Playing on Kitchen and 2 more");
        QCOMPARE(Picker::status_line({}, "Music Assistant", {}), "Sending to Music Assistant");
        QCOMPARE(Picker::status_line({}, {}, {.title = "So What", .artist = "Miles Davis"}),
                 "So What — Miles Davis");
        QCOMPARE(Picker::status_line({}, {}, {.title = "Radio"}), "Radio");
        // The PC's own sound comes first
        QCOMPARE(Picker::status_line({"Kitchen"}, {}, {.title = "So What"}), "Playing on Kitchen");
    }

    void nothingPlaysAtRest() {
        Rig rig;
        QVERIFY(!rig.picker.sendspin_plays());
        QCOMPARE(rig.picker.status_line(), "Not in use");
    }

    // ==========================================================================
    // The window
    // ==========================================================================

    void togglingShowsAndHides() {
        Rig rig;
        rig.window.toggle();
        QVERIFY(rig.window.window()->isVisible());
        rig.window.toggle();
        QVERIFY(!rig.window.window()->isVisible());
        // The click that took its focus away does not open it again
        rig.window.toggle();
        QVERIFY(!rig.window.window()->isVisible());
        QTest::qWait(400);
        rig.window.toggle();
        QVERIFY(rig.window.window()->isVisible());
    }
};

QTEST_MAIN(PickerTest)
#include "picker_test.moc"
