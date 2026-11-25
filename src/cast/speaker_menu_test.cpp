#include "cast/speaker_menu.h"

#include <QTest>

using namespace speaker_menu;
using sendspin::server::ClientDisconnect;
using sendspin::server::DisconnectReason;
using sendspin::server::ErrorKind;
using sendspin::server::GoodbyeReason;
using sendspin::server::ServerError;

namespace {

const Context GROUPED{.plays_groupable = true, .group_count = 2};
const std::string PC = "pc";

Output speaker(bool in_group, bool plays, Connection connection = Connection::CONNECTED) {
    return {.in_group = in_group, .plays = plays, .connection = connection};
}

Output local(bool in_group, bool plays) {
    return {.is_local = true, .in_group = in_group, .plays = plays};
}

/// This PC plays directly, alone; the AUX and the Voice PE are found.
const State DIRECT{.local = PC, .group = {PC}, .can_play = {PC, "aux", "pe"}};
/// This PC and the AUX play together; Office is in the group, unavailable.
const State GROUPED3{.local = PC, .group = {PC, "aux", "office"}, .can_play = {PC, "aux", "pe"}};

Action adding(std::string id) {
    return {Action::Kind::ADD, std::move(id)};
}
Action removing(std::string id) {
    return {Action::Kind::REMOVE, std::move(id)};
}
Action play_only(std::string id) {
    return {Action::Kind::PLAY_ONLY, std::move(id)};
}

ClientDisconnect goodbye(GoodbyeReason reason) {
    return {DisconnectReason::GOODBYE, reason};
}

ServerError error(ErrorKind kind, std::optional<GoodbyeReason> reason = {}) {
    ServerError error;
    error.kind = kind;
    error.goodbye_reason = reason;
    return error;
}

}  // namespace

// One test per rule, named after it, with why it is so where the name doesn't say.
class SpeakerMenuTest : public QObject {
    Q_OBJECT

private slots:
    // ---- Which speakers are listed ----

    void whatYouChoseStaysWhenItGoesAway() {
        QVERIFY(is_listed(true, false));
        const Row row = speaker_menu::row({.in_group = true, .is_discovered = false}, GROUPED);
        QCOMPARE(row.status, Status::UNAVAILABLE);
    }

    void whatYouDidNotChooseLeavesWhenItGoesAway() {
        QVERIFY(!is_listed(false, false));
        QVERIFY(is_listed(false, true));
    }

    // ---- The check ----

    void aSpeakerThatPlaysHasAFilledCheckThatRemovesIt() {
        const Row row = speaker_menu::row(speaker(true, true), GROUPED);
        QCOMPARE(row.check, Check::FILLED);
        QCOMPARE(row.click, Click::REMOVE);
        QCOMPARE(row.status, Status::NONE);
        QVERIFY(!row.warning);
    }

    void aSpeakerOutOfTheGroupHasAnEmptyCheckThatAddsIt() {
        const Row row = speaker_menu::row(speaker(false, false), GROUPED);
        QCOMPARE(row.check, Check::EMPTY);
        QCOMPARE(row.click, Click::ADD);
        QCOMPARE(row.status, Status::NONE);
    }

    // The spinner says it connects; text beside it would say it twice
    void aConnectingSpeakerHasASpinnerAndNoConnectingStatus() {
        const Row row = speaker_menu::row(speaker(true, false, Connection::CONNECTING), GROUPED);
        QCOMPARE(row.check, Check::SPINNER);
        QCOMPARE(row.click, Click::REMOVE);
        QCOMPARE(row.status, Status::NONE);
    }

    // Grey keeps it clearly chosen while it does nothing; it plays again once it's back
    void anUnavailableSpeakerInTheGroupHasAGreyCheckAndAWarning() {
        const Output outputs[] = {
            speaker(true, false, Connection::UNAVAILABLE),
            {.in_group = true, .is_discovered = false},
        };
        for (const Output& output : outputs) {
            const Row row = speaker_menu::row(output, GROUPED);
            QCOMPARE(row.check, Check::GREY);
            QVERIFY(row.warning);
            QCOMPARE(row.status, Status::UNAVAILABLE);
            QCOMPARE(row.click, Click::REMOVE);
        }
    }

    void whatIsWrongWithASpeakerInTheGroupGetsAWarning_data() {
        QTest::addColumn<Connection>("connection");
        QTest::addColumn<Status>("status");
        QTest::newRow("disconnected") << Connection::DISCONNECTED << Status::DISCONNECTED;
        QTest::newRow("can't connect") << Connection::CANT_CONNECT << Status::CANT_CONNECT;
        QTest::newRow("can't play") << Connection::CANT_PLAY << Status::CANT_PLAY;
        QTest::newRow("needs pairing") << Connection::NEEDS_PAIRING << Status::NEEDS_PAIRING;
        QTest::newRow("needs pairing again")
            << Connection::NEEDS_PAIRING_AGAIN << Status::NEEDS_PAIRING_AGAIN;
        QTest::newRow("identity changed")
            << Connection::IDENTITY_CHANGED << Status::IDENTITY_CHANGED;
    }
    void whatIsWrongWithASpeakerInTheGroupGetsAWarning() {
        QFETCH(Connection, connection);
        QFETCH(Status, status);
        const Row row = speaker_menu::row(speaker(true, false, connection), GROUPED);
        QCOMPARE(row.check, Check::GREY);
        QVERIFY(row.warning);
        QCOMPARE(row.status, status);
    }

    // Another server's choice, not a fault
    void aSpeakerAnotherServerTookIsGreyWithoutAWarning() {
        const Row row =
            speaker_menu::row(speaker(true, false, Connection::IN_USE_ELSEWHERE), GROUPED);
        QCOMPARE(row.check, Check::GREY);
        QVERIFY(!row.warning);
        QCOMPARE(row.status, Status::IN_USE_ELSEWHERE);
    }

    // The app can't take it back by itself, so the row says why, and adding it takes it back
    void aSpeakerInUseElsewhereSaysSoOutOfTheGroupToo() {
        const Row row =
            speaker_menu::row(speaker(false, false, Connection::IN_USE_ELSEWHERE), GROUPED);
        QCOMPARE(row.check, Check::EMPTY);
        QCOMPARE(row.status, Status::IN_USE_ELSEWHERE);
        QVERIFY(!row.warning);
    }

    // Adding it is what pairs it: that's said before, and isn't a fault yet
    void aSpeakerThatNeedsPairingSaysSoBeforeItIsAdded() {
        const Row row =
            speaker_menu::row(speaker(false, false, Connection::NEEDS_PAIRING), GROUPED);
        QCOMPARE(row.check, Check::EMPTY);
        QCOMPARE(row.click, Click::ADD);
        QCOMPARE(row.status, Status::NEEDS_PAIRING);
        QVERIFY(!row.warning);
    }

    // A row only says why it can't play what you chose
    void aSpeakerOutOfTheGroupSaysNothingAboutItsConnection() {
        for (const Connection connection :
             {Connection::CONNECTING, Connection::WAITING, Connection::UNAVAILABLE,
              Connection::CANT_CONNECT, Connection::DISCONNECTED, Connection::IDENTITY_CHANGED}) {
            QCOMPARE(speaker_menu::row(speaker(false, false, connection), GROUPED).status,
                     Status::NONE);
        }
    }

    // ---- This PC's own output ----

    // There's nowhere else for the sound to go
    void thisPcsOutputAloneCantBeRemoved() {
        const Row row = speaker_menu::row(local(true, true), {.group_count = 1});
        QCOMPARE(row.check, Check::FILLED);
        QCOMPARE(row.click, Click::NONE);
    }

    void thisPcsOutputCanBeRemovedWhileSpeakersPlay() {
        QCOMPARE(speaker_menu::row(local(true, true), GROUPED).click, Click::REMOVE);
    }

    // This PC's own output is always there to take over
    void aSendspinSpeakerAloneCanBeRemoved() {
        QCOMPARE(speaker_menu::row(speaker(true, true), {.group_count = 1}).click, Click::REMOVE);
    }

    // ---- Groupable or not ----

    void anOutputThatIsntGroupableHasNoCheckAndAHighlightedIconWhileItPlays() {
        const Row row = speaker_menu::row(server(true, true), {.plays_groupable = false});
        QCOMPARE(row.check, Check::NONE);
        QCOMPARE(row.click, Click::NONE);
        QVERIFY(row.highlighted);
    }

    // The check says it; a highlighted icon too would say it twice
    void aGroupableOutputsIconIsNeverHighlighted() {
        QVERIFY(!speaker_menu::row(speaker(true, true), GROUPED).highlighted);
        QVERIFY(!speaker_menu::row(local(true, true), GROUPED).highlighted);
    }

    // A check would promise grouping that can't happen: a server plays where it chooses
    void whileWhatPlaysIsntGroupableNothingCanJoin() {
        const Row row = speaker_menu::row(speaker(false, false), {.plays_groupable = false});
        QCOMPARE(row.check, Check::NONE);
        QCOMPARE(row.click, Click::NONE);
    }

    void withoutACheckConnectingIsSaidInWords() {
        const Context context{.plays_groupable = false, .group_count = 1};
        QCOMPARE(speaker_menu::row(speaker(true, false, Connection::CONNECTING), context).status,
                 Status::CONNECTING);
    }

    // ---- Servers ----

    void aServerFollowsTheSameRulesAsASpeaker() {
        const Context context{.group_count = 0};
        const Row playing = speaker_menu::row(server(true, true), context);
        QCOMPARE(playing.check, Check::NONE);
        QVERIFY(playing.highlighted);
        QCOMPARE(playing.status, Status::NONE);

        const Row idle = speaker_menu::row(server(false, true), context);
        QVERIFY(!idle.highlighted);
        QCOMPARE(idle.status, Status::NONE);

        const Row gone = speaker_menu::row(server(true, false), context);
        QVERIFY(!gone.highlighted);
        QCOMPARE(gone.status, Status::UNAVAILABLE);
    }

    // One word and one look for gone, chosen or not
    void unavailableAlwaysShowsTheWarning() {
        const Output outputs[] = {
            server(false, false),
            server(true, false),
            speaker(true, false, Connection::UNAVAILABLE),
            {.in_group = false, .is_discovered = false},
        };
        for (const Output& output : outputs) {
            const Row row = speaker_menu::row(output, GROUPED);
            QCOMPARE(row.status, Status::UNAVAILABLE);
            QVERIFY(row.warning);
        }
    }

    void anUnavailableServerAndSpeakerLookTheSame() {
        const Output gone_speaker{.is_groupable = false, .in_group = true, .is_discovered = false};
        QCOMPARE(speaker_menu::row(server(true, false), GROUPED),
                 speaker_menu::row(gone_speaker, GROUPED));
    }

    // ---- What a disconnect or an error leaves a speaker as (SPEC, "A speaker's status") ----

    void aSpeakerThatCantBeReachedOrShutDownIsUnavailable() {
        QCOMPARE(after_disconnect({DisconnectReason::CONNECTION_LOST, {}}),
                 Connection::UNAVAILABLE);
        QCOMPARE(after_disconnect(goodbye(GoodbyeReason::SHUTDOWN)), Connection::UNAVAILABLE);
        QCOMPARE(after_disconnect(goodbye(GoodbyeReason::RESTART)), Connection::UNAVAILABLE);
    }

    void aSpeakerAnotherServerHasIsInUseElsewhere() {
        QCOMPARE(after_disconnect(goodbye(GoodbyeReason::ANOTHER_SERVER)),
                 Connection::IN_USE_ELSEWHERE);
        // Unticked: it refused our idle connection
        QCOMPARE(
            after_error(error(ErrorKind::GOODBYE_BEFORE_ACTIVATION, GoodbyeReason::ANOTHER_SERVER)),
            Connection::IN_USE_ELSEWHERE);
        QCOMPARE(after_error(error(ErrorKind::GOODBYE_BEFORE_ACTIVATION,
                                   GoodbyeReason::CONCURRENT_ATTEMPT)),
                 Connection::IN_USE_ELSEWHERE);
    }

    void aSpeakerItsUserDisconnectedIsDisconnected() {
        QCOMPARE(after_disconnect(goodbye(GoodbyeReason::USER_REQUEST)), Connection::DISCONNECTED);
    }

    void aSpeakerThatAnswersButFailsCantConnect() {
        QCOMPARE(after_error(error(ErrorKind::DIAL_FAILED)), Connection::CANT_CONNECT);
        QCOMPARE(after_error(error(ErrorKind::HANDSHAKE_FAILED)), Connection::CANT_CONNECT);
        QCOMPARE(after_disconnect({DisconnectReason::CLOSED_BY_SERVER, {}}),
                 Connection::CANT_CONNECT);
        QCOMPARE(after_disconnect(goodbye(GoodbyeReason::UNAUTHORIZED)), Connection::CANT_CONNECT);
    }

    void aSpeakerWithNoFormatWeCanProduceCantPlay() {
        QCOMPARE(after_error(error(ErrorKind::NO_PLAYABLE_FORMAT)), Connection::CANT_PLAY);
    }

    void aSpeakerThatOnlyPlaysForPairedServersNeedsPairing() {
        QCOMPARE(after_disconnect(goodbye(GoodbyeReason::PAIRING_REQUIRED)),
                 Connection::NEEDS_PAIRING);
        QCOMPARE(connection({.connected = true}), Connection::NEEDS_PAIRING);
        // Approval doesn't stand in for pairing where the speaker asks for it
        QCOMPARE(connection({.connected = true, .approved = true}), Connection::NEEDS_PAIRING);
    }

    void aSpeakerThatLostItsPairingNeedsPairingAgain() {
        QCOMPARE(connection({.connected = true, .paired = true, .credential_mismatch = true}),
                 Connection::NEEDS_PAIRING_AGAIN);
    }

    // The answer to our own Forget, stop or disconnect is not a status
    void ourOwnDoingSaysNothingAboutTheSpeaker() {
        QVERIFY(!after_disconnect(goodbye(GoodbyeReason::UNPAIRED)));
        QVERIFY(!after_disconnect({DisconnectReason::SERVER_STOPPED, {}}));
        QVERIFY(!after_disconnect({DisconnectReason::DISCONNECTED_BY_HOST, {}}));
        QVERIFY(!after_error(error(ErrorKind::DISCOVERY_UNAVAILABLE)));
    }

    // ---- What the engine does for a speaker (SPEC, "What ticking does") ----

    // It connected, and nobody chose it
    void aSpeakerThatDialsInWaitsOutOfTheGroupUntilAdded() {
        const Facts facts{.connected = true, .unpaired_access = true};
        QCOMPARE(connection(facts), Connection::WAITING);
        QCOMPARE(next_step(facts), Step::NONE);
        // Paired, it may play, and still waits in the lobby
        QCOMPARE(next_step({.connected = true, .paired = true}), Step::NONE);
    }

    void addingASpeakerWithUnpairedAccessApprovesItThenMovesItIn() {
        Facts facts{.ticked = true, .connected = true, .unpaired_access = true};
        QCOMPARE(next_step(facts), Step::APPROVE);
        facts.approved = true;
        QCOMPARE(next_step(facts), Step::MOVE_TO_CAST);
        facts.in_cast = true;
        QCOMPARE(next_step(facts), Step::NONE);
    }

    void addingAPairedSpeakerMovesItIn() {
        QCOMPARE(next_step({.ticked = true, .connected = true, .paired = true}),
                 Step::MOVE_TO_CAST);
    }

    // Pairing is the user's to start, and to finish
    void anAddedSpeakerThatNeedsPairingWaitsForIt() {
        QCOMPARE(next_step({.ticked = true, .connected = true}), Step::NONE);
        QCOMPARE(
            next_step(
                {.ticked = true, .connected = true, .paired = true, .credential_mismatch = true}),
            Step::NONE);
    }

    void anAddedSpeakerThatIsntConnectedWaitsForItsConnection() {
        QCOMPARE(next_step({.ticked = true, .paired = true}), Step::NONE);
    }

    void removingAPairedSpeakerMovesItToTheLobby() {
        QCOMPARE(next_step({.connected = true, .paired = true, .in_cast = true}),
                 Step::MOVE_TO_LOBBY);
    }

    // Its approval was the tick, so it is held alone again, connected or not
    void removingASpeakerThatWasOnlyApprovedRevokesIt() {
        const Facts facts{
            .connected = true, .approved = true, .unpaired_access = true, .in_cast = true};
        QCOMPARE(next_step(facts), Step::REVOKE);
        QCOMPARE(next_step({.approved = true, .unpaired_access = true}), Step::REVOKE);
    }

    // ---- Choices: adding ----

    // Adding starts from what plays now: this PC plays along
    void addingASpeakerToThisPcPlaysThemTogetherThroughSendspin() {
        QCOMPARE(plan(adding("aux"), DIRECT), (Plan{{PC, "aux"}, Route::SPEAKERS}));
    }

    void addingThisPcJoinsTheSpeakers() {
        const State speaker_alone{.local = PC, .group = {"aux"}, .can_play = {PC, "aux"}};
        QCOMPARE(plan(adding(PC), speaker_alone), (Plan{{PC, "aux"}, Route::SPEAKERS}));
    }

    // ---- Choices: removing ----

    // Alone, this PC's own output plays directly: nothing to keep in sync with, no delay
    void removingTheLastSendspinSpeakerHandsOverToThisPc() {
        const State speaker_alone{.local = PC, .group = {"aux"}, .can_play = {PC, "aux"}};
        QCOMPARE(plan(removing("aux"), speaker_alone), (Plan{{PC}, Route::DIRECT}));
        const State with_pc{.local = PC, .group = {PC, "aux"}, .can_play = {PC, "aux"}};
        QCOMPARE(plan(removing("aux"), with_pc), (Plan{{PC}, Route::DIRECT}));
    }

    // Something always plays after a choice
    void whenNothingLeftCanPlayThisPcJoinsAndTheOthersWait() {
        const State state{.local = PC, .group = {"aux", "office"}, .can_play = {PC, "aux"}};
        QCOMPARE(plan(removing("aux"), state), (Plan{{PC, "office"}, Route::SPEAKERS}));
    }

    void removingASpeakerKeepsTheRestOfTheGroup() {
        QCOMPARE(plan(removing("aux"), GROUPED3), (Plan{{PC, "office"}, Route::SPEAKERS}));
    }

    void thisPcAloneCantBeRemoved() {
        QVERIFY(!plan(removing(PC), DIRECT));
    }

    void thisPcCantLeaveOnlyUnavailableSpeakersBehind() {
        const State state{.local = PC, .group = {PC, "office"}, .can_play = {PC}};
        QCOMPARE(plan(removing(PC), state), (Plan{{PC, "office"}, Route::SPEAKERS}));
    }

    void removingWhatIsntInTheGroupChangesNothing() {
        QVERIFY(!plan(removing("pe"), GROUPED3));
    }

    // The output only changes when the user adds, removes or picks something
    void noAutomaticFallbackWhenEverySpeakerInTheGroupIsGone() {
        const State all_gone{.local = PC, .group = {"aux", "office"}, .can_play = {PC}};
        QCOMPARE(route(all_gone), Route::SPEAKERS);
        QCOMPARE(route(DIRECT), Route::DIRECT);
    }

    // ---- Choices: clicking a name ----

    void clickingThisPcsNamePlaysItDirectlyAlone() {
        QCOMPARE(plan(play_only(PC), GROUPED3), (Plan{{PC}, Route::DIRECT}));
    }

    void clickingASpeakersNamePlaysThereOnly() {
        QCOMPARE(plan(play_only("aux"), GROUPED3), (Plan{{"aux"}, Route::SPEAKERS}));
    }

    void clickingAnUnavailableSpeakersNameBringsThisPcAlong() {
        QCOMPARE(plan(play_only("office"), GROUPED3), (Plan{{PC, "office"}, Route::SPEAKERS}));
    }

    // ---- Choices: choosing a server ----

    // A server can't be grouped with; speakers left in a group nobody plays to would be stale
    void choosingAServerDissolvesTheGroupUnavailableSpeakersToo() {
        QCOMPARE(plan({Action::Kind::CHOOSE_SERVER, {}}, GROUPED3), (Plan{{}, Route::SERVER}));
    }

    void aSpeakerChosenAfterAServerStartsANewGroup() {
        const State server_chosen{.local = PC, .group = {}, .can_play = {PC, "aux"}};
        QCOMPARE(route(server_chosen), Route::SERVER);
        QCOMPARE(plan(adding("aux"), server_chosen), (Plan{{"aux"}, Route::SPEAKERS}));
    }

    // ---- The tray icon ----

    void sendspinPlaysWhileAConnectedSpeakerGetsThePcsSound() {
        QVERIFY(sendspin_plays(true, Route::SPEAKERS, true, false));
    }

    // With every speaker in the group unavailable, nothing goes out over Sendspin
    void thisPcsOwnOutputAloneIsntSendspinPlaying() {
        QVERIFY(!sendspin_plays(true, Route::SPEAKERS, false, false));
    }

    void silenceIsntPlaying() {
        QVERIFY(!sendspin_plays(false, Route::SPEAKERS, true, false));
        QVERIFY(!sendspin_plays(false, Route::SERVER, false, true));
    }

    void aServerPlaysOnceItsStreamIsOpen() {
        QVERIFY(sendspin_plays(true, Route::SERVER, false, true));
        QVERIFY(!sendspin_plays(true, Route::SERVER, true, false));
    }

    void playedDirectlySendspinDoesntPlay() {
        QVERIFY(!sendspin_plays(true, Route::DIRECT, true, true));
    }

    // ---- The dot ----

    void theDotIsBlueWhileSendspinPlays() {
        QCOMPARE(dot(true, true), Dot::PLAYS);
    }

    void theDotIsGreyWhileSendspinIsTheOutputButNothingGoesOut() {
        QCOMPARE(dot(true, false), Dot::IN_USE);
    }

    void theDotIsAnEmptyRingWhileThePcPlaysWithoutSendspin() {
        QCOMPARE(dot(false, false), Dot::NOT_IN_USE);
    }
};

QTEST_APPLESS_MAIN(SpeakerMenuTest)
#include "speaker_menu_test.moc"
