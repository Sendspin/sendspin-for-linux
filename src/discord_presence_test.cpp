#include "discord_presence.h"

#include "discord_test_server.h"
#include "persistence.h"

#include <QDateTime>
#include <QTemporaryDir>
#include <QTest>

#include <sendspin/server/server.h>

#include <vector>

namespace server = sendspin::server;
using namespace std::chrono_literals;

namespace {

const Track TRACK{.title = "Blue in Green",
                  .artist = "Miles Davis",
                  .album = "Kind of Blue",
                  .duration_ms = 337'000};
constexpr qint64 NOW_MS = 1'800'000'000'000;
constexpr DiscordIpc::Timing QUICK{.retry = 50ms, .pace = 10ms};
/// Long enough that nothing more would come
constexpr int QUIET_MS = 150;

}  // namespace

class DiscordPresenceTest : public QObject {
    Q_OBJECT

private slots:
    void aTrackIsListeningToItsTitleArtistAndAlbum() {
        const QJsonObject activity = discord_activity(TRACK, {}, 60'000, NOW_MS);
        QCOMPARE(activity.value("type").toInt(), 2);
        QCOMPARE(activity.value("details"), "Blue in Green");
        QCOMPARE(activity.value("state"), "Miles Davis");
        QCOMPARE(activity.value("assets").toObject(),
                 (QJsonObject{{"large_image", "sendspin"}, {"large_text", "Kind of Blue"}}));
        // The bar: began a minute ago, ends when the track does
        const QJsonObject timestamps = activity.value("timestamps").toObject();
        QCOMPARE(timestamps.value("start").toInteger(), NOW_MS - 60'000);
        QCOMPARE(timestamps.value("end").toInteger(), NOW_MS - 60'000 + 337'000);
    }

    void whatTheServerLeavesOutIsLeftOut() {
        const QJsonObject activity =
            discord_activity({.title = "Blue in Green", .duration_ms = 337'000}, {}, 0, NOW_MS);
        QVERIFY(!activity.contains("state"));
        QCOMPARE(activity.value("assets").toObject(), (QJsonObject{{"large_image", "sendspin"}}));
    }

    void aLiveStreamCountsUpWithNoEnd() {
        const QJsonObject timestamps =
            discord_activity({.title = "Radio"}, {}, 5'000, NOW_MS).value("timestamps").toObject();
        QCOMPARE(timestamps.value("start").toInteger(), NOW_MS - 5'000);
        QVERIFY(!timestamps.contains("end"));
    }

    void aPositionPastTheEndIsTheEnd() {
        const QJsonObject timestamps =
            discord_activity(TRACK, {}, 400'000, NOW_MS).value("timestamps").toObject();
        QCOMPARE(timestamps.value("end").toInteger(), NOW_MS);
    }

    // Discord refuses a text shorter than 2 or longer than 128
    void textsAreMadeToFitDiscord() {
        const QJsonObject activity = discord_activity(
            {.title = "7", .artist = QString(200, 'a'), .album = "X"}, {}, 0, NOW_MS);
        QCOMPARE(activity.value("details"), "7 ");
        QCOMPARE(activity.value("state").toString().size(), 128);
        QCOMPARE(activity.value("assets").toObject().value("large_text"), "X ");
    }

    void theCoverTakesTheLogosPlaceAndTheLogoGoesSmall() {
        const QString cover = "https://covers.example.com/kind-of-blue.jpg";
        const QJsonObject activity = discord_activity(TRACK, cover, 0, NOW_MS);
        QCOMPARE(activity.value("assets").toObject(),
                 (QJsonObject{{"large_image", cover},
                              {"small_image", "sendspin"},
                              {"large_text", "Kind of Blue"}}));
    }

    // Discord's servers fetch the cover: only what they can reach is handed over
    void onlyACoverDiscordCanFetchIsShown_data() {
        QTest::addColumn<QString>("url");
        QTest::addColumn<bool>("shown");

        QTest::newRow("public https") << "https://covers.example.com/a/b.jpg?size=512" << true;
        QTest::newRow("public https, port") << "https://covers.example.com:8443/b.jpg" << true;
        QTest::newRow("public http") << "http://covers.example.com/b.jpg" << false;
        QTest::newRow("LAN http") << "http://192.168.1.20:8097/imageproxy?path=abc&size=512"
                                  << false;
        QTest::newRow("LAN https") << "https://192.168.1.20:8097/imageproxy?path=abc" << false;
        QTest::newRow("public IP") << "https://93.184.216.34/b.jpg" << false;
        QTest::newRow("10/8") << "https://10.0.0.5/b.jpg" << false;
        QTest::newRow("loopback") << "https://127.0.0.1/b.jpg" << false;
        QTest::newRow("link-local") << "https://169.254.10.1/b.jpg" << false;
        QTest::newRow("IPv6") << "https://[fd00::20]/b.jpg" << false;
        QTest::newRow("mDNS name") << "https://homeassistant.local:8097/b.jpg" << false;
        QTest::newRow(".lan") << "https://music.lan/b.jpg" << false;
        QTest::newRow("upper case, trailing dot") << "https://Music.LAN./b.jpg" << false;
        QTest::newRow("one label") << "https://homeassistant/b.jpg" << false;
        QTest::newRow("localhost") << "https://localhost/b.jpg" << false;
        QTest::newRow("a password") << "https://user:secret@covers.example.com/b.jpg" << false;
        QTest::newRow("256 long") << "https://covers.example.com/" + QString(229, 'a') << true;
        QTest::newRow("too long") << "https://covers.example.com/" + QString(230, 'a') << false;
        QTest::newRow("none") << "" << false;
        QTest::newRow("not a URL") << "cover.jpg" << false;
    }

    void onlyACoverDiscordCanFetchIsShown() {
        QFETCH(QString, url);
        QFETCH(bool, shown);
        QCOMPARE(discord_artwork_url(url), shown ? url : QString());
    }

    void withoutAnApplicationThereIsNothingToSwitchOn() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        ClientPersistence persistence(dir.path().toStdString());
        Client client({.name = "Test PC", .port = Client::free_port(0), .advertise = false},
                      &persistence);
        DiscordPresence presence(client, QString(), dir.path(), QUICK);
        QVERIFY(!presence.available());
        presence.set_enabled(true);
        QTest::qWait(QUIET_MS);
        QCOMPARE(discord.connections, 0);
    }

    // sendspin-server-cpp in this process stands in for Music Assistant
    void showsWhatAServerPlaysHere() {
        const QTemporaryDir dir;
        DiscordTestServer discord;
        QVERIFY(discord.listen(dir.filePath("discord-ipc-0")));
        const auto shown = [&] { return discord.activities().last().toObject(); };
        const auto start_of = [](const QJsonObject& activity) {
            return activity.value("timestamps").toObject().value("start").toInteger();
        };

        ServerPersistence server_persistence(dir.path().toStdString());
        server::SendspinServer server;
        server::ServerConfig server_config;
        server_config.listen_port = 0;
        server_config.advertise = false;
        server_config.browse = false;
        QCOMPARE(server.start(server_config, &server_persistence, nullptr),
                 server::StartResult::OK);

        ClientPersistence persistence(dir.path().toStdString());
        Client client({.name = "Test PC", .port = Client::free_port(0), .advertise = false},
                      &persistence);
        DiscordPresence presence(client, "1234", dir.path(), QUICK);
        QVERIFY(presence.available());

        QVERIFY(client.start());
        QVERIFY(
            server.connect_url("ws://127.0.0.1:" + std::to_string(client.port()) + "/sendspin"));
        QTRY_COMPARE(client.state(), Client::State::Connected);
        // The roles need pairing
        const std::string id = client.client_id().toStdString();
        QVERIFY(server.begin_pairing(id, {}));
        QTRY_COMPARE(client.pairing_code().size(), 6);
        QTRY_VERIFY(server.submit_pairing_code(id, client.pairing_code().toStdString()));
        QTRY_VERIFY(client.is_paired());
        QTRY_COMPARE(client.state(), Client::State::Connected);

        const auto group = server.default_group();
        QVERIFY(group.has_value());
        server::Metadata metadata;
        metadata.title = "Blue in Green";
        metadata.artist = "Miles Davis";
        metadata.album = "Kind of Blue";
        // Music Assistant's image proxy, on the LAN: not for Discord
        metadata.artwork_url = "http://192.168.1.20:8097/imageproxy?path=abc";
        metadata.progress = {.track_progress_ms = 60'000, .track_duration_ms = 337'000};
        QVERIFY(server.set_metadata(*group, metadata, server.now_us()));
        const server::OpenedInput opened =
            server.open_input(*group, {.format = {48000, 2, server::SampleFormat::S16}});
        QCOMPARE(opened.result, server::OpenInputResult::OK);
        const std::vector<int16_t> silence(48000 * 2, 0);
        const auto play = [&] {
            size_t written = 0;
            QTRY_VERIFY((written += opened.input->write(silence.data(), 48000 - written).frames) ==
                        48000);
        };
        play();
        QTRY_VERIFY(client.now_playing().playing());

        // Off until switched on: it plays, and Discord hears nothing of it
        QTest::qWait(QUIET_MS);
        QCOMPARE(discord.connections, 0);

        presence.set_enabled(true);
        QTRY_COMPARE(discord.activities().size(), 1);
        QCOMPARE(discord.frames.at(0).payload.value("client_id"), "1234");
        const QJsonObject first = shown();
        QCOMPARE(first.value("type").toInt(), 2);
        QCOMPARE(first.value("details"), "Blue in Green");
        QCOMPARE(first.value("state"), "Miles Davis");
        QCOMPARE(first.value("assets").toObject(),
                 (QJsonObject{{"large_image", "sendspin"}, {"large_text", "Kind of Blue"}}));
        const QJsonObject timestamps = first.value("timestamps").toObject();
        QCOMPARE(timestamps.value("end").toInteger() - timestamps.value("start").toInteger(),
                 337'000);
        const qint64 began_ms = QDateTime::currentMSecsSinceEpoch() - start_of(first);
        QVERIFY2(began_ms >= 60'000 && began_ms < 90'000, qPrintable(QString::number(began_ms)));

        // A seek moves the bar
        metadata.progress = {.track_progress_ms = 200'000, .track_duration_ms = 337'000};
        QVERIFY(server.set_metadata(*group, metadata, server.now_us()));
        QTRY_COMPARE(discord.activities().size(), 2);
        QCOMPARE(shown().value("details"), "Blue in Green");
        QVERIFY(start_of(shown()) < start_of(first) - 100'000);

        // The next track
        metadata.title = "All Blues";
        metadata.artwork_url = "https://covers.example.com/kind-of-blue.jpg";
        metadata.progress = {.track_progress_ms = 0, .track_duration_ms = 693'000};
        QVERIFY(server.set_metadata(*group, metadata, server.now_us()));
        QTRY_COMPARE(discord.activities().size(), 3);
        QCOMPARE(shown().value("details"), "All Blues");
        QCOMPARE(shown().value("assets").toObject().value("large_image"),
                 "https://covers.example.com/kind-of-blue.jpg");
        QCOMPARE(shown().value("assets").toObject().value("small_image"), "sendspin");

        // Paused: nothing is listened to
        opened.input->stop();
        QTRY_COMPARE(discord.activities().size(), 4);
        QVERIFY(discord.activities().last().isNull());
        play();
        QTRY_COMPARE(discord.activities().size(), 5);
        QCOMPARE(shown().value("details"), "All Blues");

        // Off in Settings: the connection goes, and the activity with it. On again.
        presence.set_enabled(false);
        QTRY_VERIFY(!discord.connected());
        presence.set_enabled(true);
        QTRY_COMPARE(discord.activities().size(), 6);
        QCOMPARE(discord.connections, 2);
        QCOMPARE(shown().value("details"), "All Blues");

        // The server goes
        server.stop();
        QTRY_COMPARE(discord.activities().size(), 7);
        QVERIFY(discord.activities().last().isNull());
    }
};

QTEST_GUILESS_MAIN(DiscordPresenceTest)
#include "discord_presence_test.moc"
