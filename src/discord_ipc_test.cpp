#include "discord_ipc.h"

#include "discord_test_server.h"

#include <QCoreApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace std::chrono_literals;

namespace {

const QJsonObject FIRST{{"type", 2}, {"details", "Blue in Green"}};
const QJsonObject SECOND{{"type", 2}, {"details", "So What"}};
const QJsonObject THIRD{{"type", 2}, {"details", "All Blues"}};
/// Long enough that nothing more would come
constexpr int QUIET_MS = 150;

}  // namespace

class DiscordIpcTest : public QObject {
    Q_OBJECT

private slots:
    void socketsAreTriedInOrder() {
        const QStringList paths = discord_socket_paths("/run/user/1000");
        QCOMPARE(paths.size(), 40);
        QCOMPARE(paths.at(0), "/run/user/1000/discord-ipc-0");
        QCOMPARE(paths.at(9), "/run/user/1000/discord-ipc-9");
        QCOMPARE(paths.at(10), "/run/user/1000/app/com.discordapp.Discord/discord-ipc-0");
        QCOMPARE(paths.at(19), "/run/user/1000/app/com.discordapp.Discord/discord-ipc-9");
        QCOMPARE(paths.at(20), "/run/user/1000/app/com.discordapp.DiscordCanary/discord-ipc-0");
        QCOMPARE(paths.at(30), "/run/user/1000/snap.discord/discord-ipc-0");
        QCOMPARE(paths.at(39), "/run/user/1000/snap.discord/discord-ipc-9");
    }

    void nothingConnectsWithoutAnActivity() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
        ipc.set_activity({});
        QTest::qWait(QUIET_MS);
        QCOMPARE(discord.connections, 0);
    }

    void shakesHandsThenSetsTheActivity() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
        ipc.set_activity(FIRST);
        QTRY_COMPARE(discord.frames.size(), 2);

        QCOMPARE(discord.frames.at(0).opcode, 0);
        QCOMPARE(discord.frames.at(0).payload, (QJsonObject{{"v", 1}, {"client_id", "1234"}}));
        const DiscordTestServer::Frame& set = discord.frames.at(1);
        QCOMPARE(set.opcode, 1);
        QCOMPARE(set.payload.value("cmd"), "SET_ACTIVITY");
        QVERIFY(!set.payload.value("nonce").toString().isEmpty());
        const QJsonObject args = set.payload.value("args").toObject();
        QCOMPARE(args.value("pid").toInteger(), QCoreApplication::applicationPid());
        QCOMPARE(args.value("activity").toObject(), FIRST);

        // The same again is nothing to tell Discord
        ipc.set_activity(FIRST);
        QTest::qWait(QUIET_MS);
        QCOMPARE(discord.frames.size(), 2);
    }

    void anEmptyActivityClearsItAndKeepsTheConnection() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
        ipc.set_activity(FIRST);
        QTRY_COMPARE(discord.activities().size(), 1);
        ipc.set_activity({});
        QTRY_COMPARE(discord.activities().size(), 2);
        QVERIFY(discord.activities().at(1).isNull());
        const QJsonObject clear = discord.frames.last().payload;
        QVERIFY(clear.value("args").toObject().contains("activity"));
        QVERIFY(clear.value("nonce") != discord.frames.at(1).payload.value("nonce"));

        ipc.set_activity(SECOND);
        QTRY_COMPARE(discord.activities().size(), 3);
        QCOMPARE(discord.connections, 1);
    }

    void updatesArePacedAndTheLatestWins() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 300ms});
        ipc.set_activity(FIRST);
        QTRY_COMPARE(discord.activities().size(), 1);
        ipc.set_activity(SECOND);
        ipc.set_activity(THIRD);
        QTest::qWait(100);
        QCOMPARE(discord.activities().size(), 1);
        QTRY_COMPARE(discord.activities().size(), 2);
        QCOMPARE(discord.activities().at(1).toObject(), THIRD);
        QTest::qWait(400);
        QCOMPARE(discord.activities().size(), 2);
    }

    void goingAwayDropsTheActivityWithTheConnection() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        {
            DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
            ipc.set_activity(FIRST);
            QTRY_COMPARE(discord.activities().size(), 1);
            QVERIFY(discord.connected());
        }
        QTRY_VERIFY(!discord.connected());
    }

    void waitsForADiscordThatStartsLater() {
        const QTemporaryDir dir;
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
        ipc.set_activity(FIRST);
        QTest::qWait(QUIET_MS);
        DiscordTestServer discord;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        QTRY_COMPARE(discord.activities().size(), 1);
        QCOMPARE(discord.activities().at(0).toObject(), FIRST);
    }

    void stopsLookingOnceThereIsNothingToShow() {
        const QTemporaryDir dir;
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
        ipc.set_activity(FIRST);
        ipc.set_activity({});
        DiscordTestServer discord;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        QTest::qWait(QUIET_MS);
        QCOMPARE(discord.connections, 0);
    }

    void reconnectsAfterDiscordRestarts() {
        const QTemporaryDir dir;
        const QString path = dir.filePath("discord-ipc-0");
        DiscordTestServer discord;
        QVERIFY(discord.listen(path));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
        ipc.set_activity(FIRST);
        QTRY_COMPARE(discord.activities().size(), 1);

        discord.close();
        QVERIFY(!QFile::exists(path));
        QTest::qWait(QUIET_MS);
        // What plays by the time it is back
        ipc.set_activity(SECOND);
        QVERIFY(discord.listen(path));
        QTRY_COMPARE(discord.activities().size(), 2);
        QCOMPARE(discord.connections, 2);
        // A handshake of its own, and the activity it missed
        QCOMPARE(discord.frames.at(2).opcode, 0);
        QCOMPARE(discord.activities().at(1).toObject(), SECOND);
    }

    void aDiscordThatWentAwayWhileNothingShowedIsFoundAgain() {
        const QTemporaryDir dir;
        const QString path = dir.filePath("discord-ipc-0");
        DiscordTestServer discord;
        QVERIFY(discord.listen(path));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
        ipc.set_activity(FIRST);
        QTRY_COMPARE(discord.activities().size(), 1);
        ipc.set_activity({});
        QTRY_COMPARE(discord.activities().size(), 2);

        discord.close();
        QVERIFY(discord.listen(path));
        QTest::qWait(QUIET_MS);
        QCOMPARE(discord.connections, 1);
        ipc.set_activity(SECOND);
        QTRY_COMPARE(discord.activities().size(), 3);
        QCOMPARE(discord.connections, 2);
    }

    void passesOverASocketNothingListensOn() {
        const QTemporaryDir dir;
        // What a Discord that crashed leaves behind
        QFile stale(dir.filePath("discord-ipc-0"));
        QVERIFY(stale.open(QIODevice::WriteOnly));
        stale.close();
        DiscordTestServer flatpak;
        QVERIFY(flatpak.listen(dir.filePath("app/com.discordapp.Discord/discord-ipc-0")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
        ipc.set_activity(FIRST);
        QTRY_COMPARE(flatpak.activities().size(), 1);
    }

    void theFirstSocketInOrderIsTheOneUsed() {
        const QTemporaryDir dir;
        DiscordTestServer snap;
        DiscordTestServer flatpak;
        DiscordTestServer second;
        QVERIFY(snap.listen(dir.filePath("snap.discord/discord-ipc-0")));
        QVERIFY(flatpak.listen(dir.filePath("app/com.discordapp.Discord/discord-ipc-0")));
        QVERIFY(second.listen(dir.filePath("discord-ipc-1")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
        ipc.set_activity(FIRST);
        QTRY_COMPARE(second.activities().size(), 1);
        QCOMPARE(flatpak.connections, 0);
        QCOMPARE(snap.connections, 0);

        // Gone, the next in order takes its place
        second.close();
        QTRY_COMPARE(flatpak.activities().size(), 1);
        QCOMPARE(snap.connections, 0);
    }

    void answersAPing() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
        ipc.set_activity(FIRST);
        QTRY_COMPARE(discord.frames.size(), 2);
        discord.send(3, {{"seq", 7}});
        QTRY_COMPARE(discord.frames.size(), 3);
        QCOMPARE(discord.frames.at(2).opcode, 4);
        QCOMPARE(discord.frames.at(2).payload, (QJsonObject{{"seq", 7}}));
    }

    void aSocketThatNeverAnswersIsGivenUpOn() {
        const QTemporaryDir dir;
        DiscordTestServer silent;
        silent.mute = true;
        DiscordTestServer flatpak;
        QVERIFY(silent.listen(dir.filePath("discord-ipc-0")));
        QVERIFY(flatpak.listen(dir.filePath("app/com.discordapp.Discord/discord-ipc-0")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
        ipc.set_activity(FIRST);
        QTRY_COMPARE(flatpak.activities().size(), 1);
        QCOMPARE(silent.connections, 1);
        QVERIFY(silent.activities().isEmpty());
    }

    void hangsUpOnceNothingShowedForAWhile() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms, .idle = 200ms});
        ipc.set_activity(FIRST);
        QTRY_COMPARE(discord.activities().size(), 1);
        ipc.set_activity({});
        QTRY_COMPARE(discord.activities().size(), 2);
        QVERIFY(discord.connected());
        QTRY_VERIFY(!discord.connected());
        // And stays away until something plays
        QTest::qWait(QUIET_MS);
        QCOMPARE(discord.connections, 1);
        ipc.set_activity(SECOND);
        QTRY_COMPARE(discord.activities().size(), 3);
        QCOMPARE(discord.connections, 2);
    }

    void playingAgainInTimeKeepsTheConnection() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms, .idle = 200ms});
        ipc.set_activity(FIRST);
        ipc.set_activity({});
        ipc.set_activity(SECOND);
        QTRY_COMPARE(discord.activities().size(), 1);
        QTest::qWait(400);
        QVERIFY(discord.connected());
        QCOMPARE(discord.connections, 1);
    }

    void aFrameTooLargeToBeDiscordsEndsTheConnection() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        discord.mute = true;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 500ms, .pace = 10ms});
        ipc.set_activity(FIRST);
        QTRY_COMPARE(discord.frames.size(), 1);
        QByteArray header(8, '\0');
        qToLittleEndian<qint32>(1, header.data());
        qToLittleEndian<qint32>(1 << 20, header.data() + 4);
        discord.write_raw(header + "{");
        QTRY_VERIFY(!discord.connected());
        // Tried again like any other
        discord.mute = false;
        QTRY_COMPARE(discord.activities().size(), 1);
        QCOMPARE(discord.connections, 2);
    }

    void whatIsNoJsonIsPassedOver() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
        ipc.set_activity(FIRST);
        QTRY_COMPARE(discord.activities().size(), 1);
        discord.write_raw(DiscordTestServer::frame(1, "\xff\xfe not JSON"));
        discord.write_raw(DiscordTestServer::frame(1, ""));
        discord.write_raw(DiscordTestServer::frame(77, "{}"));
        ipc.set_activity(SECOND);
        QTRY_COMPARE(discord.activities().size(), 2);
        QCOMPARE(discord.connections, 1);
    }

    void aFrameThatComesInPiecesIsPutTogether() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        discord.mute = true;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 2s, .pace = 10ms});
        ipc.set_activity(FIRST);
        QTRY_COMPARE(discord.frames.size(), 1);
        const QByteArray ready =
            DiscordTestServer::frame(1, R"({"cmd":"DISPATCH","evt":"READY","data":{}})");
        // Inside the header, then inside the JSON
        discord.write_raw(ready.left(5));
        QTest::qWait(30);
        discord.write_raw(ready.mid(5, 12));
        QTest::qWait(30);
        QVERIFY(discord.activities().isEmpty());
        // The rest, with the start of a ping behind it
        const QByteArray ping = DiscordTestServer::frame(3, R"({"seq":7})");
        discord.write_raw(ready.mid(17) + ping.left(10));
        QTRY_COMPARE(discord.activities().size(), 1);
        discord.write_raw(ping.mid(10));
        QTRY_COMPARE(discord.frames.size(), 3);
        QCOMPARE(discord.frames.at(2).opcode, 4);
        QCOMPARE(discord.connections, 1);
    }

    void aCloseFromDiscordIsADiscordThatWentAway() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
        ipc.set_activity(FIRST);
        QTRY_COMPARE(discord.activities().size(), 1);
        discord.send(2, {{"code", 1000}, {"message", "bye"}});
        // Back, with what plays
        QTRY_COMPARE(discord.activities().size(), 2);
        QCOMPARE(discord.connections, 2);
        QCOMPARE(discord.activities().at(1).toObject(), FIRST);
    }

    void anActivityDiscordRefusesIsSaidAndNotSentAgainAsItIs() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
        QSignalSpy refused(&ipc, &DiscordIpc::refused);
        ipc.set_activity(FIRST);
        QTRY_COMPARE(discord.activities().size(), 1);
        discord.send(1, {{"cmd", "SET_ACTIVITY"},
                         {"evt", "ERROR"},
                         {"data", QJsonObject{{"code", 4000}, {"message", "details too short"}}},
                         {"nonce", discord.frames.last().payload.value("nonce")}});
        QTRY_COMPARE(refused.size(), 1);
        QCOMPARE(refused.at(0).at(0).toString(), "details too short");
        ipc.set_activity(FIRST);
        QTest::qWait(QUIET_MS);
        QCOMPARE(discord.activities().size(), 1);
        QCOMPARE(discord.connections, 1);

        // Discord still shows what it showed before: the next one goes out, a clear too
        ipc.set_activity({});
        QTRY_COMPARE(discord.activities().size(), 2);
        QVERIFY(discord.activities().at(1).isNull());
        ipc.set_activity(SECOND);
        QTRY_COMPARE(discord.activities().size(), 3);
    }

    void anApplicationDiscordRefusesIsTriedAgainLater() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        discord.accepts = false;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        DiscordIpc ipc("1234", dir.path(), {.retry = 50ms, .pace = 10ms});
        ipc.set_activity(FIRST);
        QTRY_VERIFY(discord.connections >= 2);
        QVERIFY(discord.activities().isEmpty());
        discord.accepts = true;
        QTRY_COMPARE(discord.activities().size(), 1);
    }
};

QTEST_GUILESS_MAIN(DiscordIpcTest)
#include "discord_ipc_test.moc"
