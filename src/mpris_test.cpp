#include "mpris.h"

#include "persistence.h"

#include <QBuffer>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>
#include <QUrl>

#include <sendspin/server/server.h>

#include <mutex>
#include <vector>

namespace server = sendspin::server;

namespace {

/// The transport commands the server got; it calls on its own thread
class Commands final : public server::ServerListener {
public:
    std::vector<server::TransportCommand> get() {
        const std::lock_guard lock(this->mutex_);
        return this->commands_;
    }

private:
    void on_transport_command(server::GroupId /*group*/, server::TransportCommand command,
                              const server::CommandArgs& /*args*/) override {
        const std::lock_guard lock(this->mutex_);
        this->commands_.push_back(command);
    }

    std::mutex mutex_;
    std::vector<server::TransportCommand> commands_;
};

const Track TRACK{.title = "Blue in Green",
                  .artist = "Miles Davis",
                  .album = "Kind of Blue",
                  .album_artist = "Miles Davis Sextet",
                  .number = 3,
                  .duration_ms = 337'000};

}  // namespace

class MprisTest : public QObject {
    Q_OBJECT

private slots:
    void metadataMapsToTheMprisKeys() {
        const QVariantMap metadata = mpris_metadata(TRACK, "file:///run/user/1000/cover.jpg");
        QCOMPARE(metadata.value("xesam:title"), QVariant("Blue in Green"));
        // Lists, as MPRIS has them
        QCOMPARE(metadata.value("xesam:artist"), QVariant(QStringList{"Miles Davis"}));
        QCOMPARE(metadata.value("xesam:album"), QVariant("Kind of Blue"));
        QCOMPARE(metadata.value("xesam:albumArtist"), QVariant(QStringList{"Miles Davis Sextet"}));
        QCOMPARE(metadata.value("xesam:trackNumber"), QVariant(3));
        // Microseconds, a 64-bit integer on the bus
        QCOMPARE(metadata.value("mpris:length").typeId(), QMetaType::LongLong);
        QCOMPARE(metadata.value("mpris:length").toLongLong(), 337'000'000);
        QCOMPARE(metadata.value("mpris:artUrl"), QVariant("file:///run/user/1000/cover.jpg"));
        QCOMPARE(metadata.size(), 8);
    }

    void theTrackIdIsAnObjectPathThatFollowsTheTrack() {
        const auto id = [](const Track& track) {
            const QVariant value = mpris_metadata(track, {}).value("mpris:trackid");
            return value.typeId() == qMetaTypeId<QDBusObjectPath>()
                       ? value.value<QDBusObjectPath>().path()
                       : QString();
        };
        QVERIFY(id(TRACK).startsWith("/com/sendspin_audio/Sendspin/track/"));
        QCOMPARE(id(TRACK), id(TRACK));
        Track next = TRACK;
        next.title = "All Blues";
        QVERIFY(id(next) != id(TRACK));
        QCOMPARE(id({}), "/org/mpris/MediaPlayer2/TrackList/NoTrack");
    }

    void whatTheServerLeavesOutIsLeftOut() {
        const QVariantMap metadata = mpris_metadata({.title = "Radio"}, {});
        QCOMPARE(metadata.keys(), (QStringList{"mpris:trackid", "xesam:title"}));
        QCOMPARE(mpris_metadata({}, "file:///stale.jpg").keys(), QStringList{"mpris:trackid"});
    }

    void anIdleGroupCannotPlaySoTheCardHides() {
        const QVariantMap idle = mpris_properties({.can_play = true});
        QCOMPARE(idle.value("PlaybackStatus"), QVariant("Stopped"));
        QCOMPARE(idle.value("CanPlay"), QVariant(false));
    }

    void playingAndPausedShowTheCard() {
        MprisState state{.playing = true, .track = TRACK, .volume = 40, .can_pause = true};
        QVariantMap properties = mpris_properties(state);
        QCOMPARE(properties.value("PlaybackStatus"), QVariant("Playing"));
        QCOMPARE(properties.value("CanPlay"), QVariant(true));
        QCOMPARE(properties.value("CanPause"), QVariant(true));
        QCOMPARE(properties.value("Volume"), QVariant(0.4));

        // Stopped on a track: it can go on if the server says so
        state.playing = false;
        state.can_play = true;
        properties = mpris_properties(state);
        QCOMPARE(properties.value("PlaybackStatus"), QVariant("Paused"));
        QCOMPARE(properties.value("CanPlay"), QVariant(true));
        state.can_play = false;
        QCOMPARE(mpris_properties(state).value("CanPlay"), QVariant(false));
    }

    void onlyWhatTheServerListsIsOffered() {
        MprisState state{.playing = true, .track = TRACK};
        QVariantMap properties = mpris_properties(state);
        for (const char* key : {"CanPause", "CanGoNext", "CanGoPrevious", "CanSeek"}) {
            QCOMPARE(properties.value(key), QVariant(false));
        }
        state.can_next = state.can_previous = state.can_seek = true;
        properties = mpris_properties(state);
        for (const char* key : {"CanGoNext", "CanGoPrevious", "CanSeek"}) {
            QCOMPARE(properties.value(key), QVariant(true));
        }
        // Nothing to seek in on a stream with no length
        state.track.duration_ms = 0;
        QCOMPARE(mpris_properties(state).value("CanSeek"), QVariant(false));
    }

    void theCoverIsAFileNamedByItsBytes() {
        const QTemporaryDir dir;
        const QString art_dir = dir.filePath("app/test");
        QString first;
        {
            ArtworkFile artwork(art_dir);
            QCOMPARE(artwork.set({}), QString());
            QVERIFY(!QDir(art_dir).exists());

            first = artwork.set("first cover");
            const QString path = QUrl(first).toLocalFile();
            QVERIFY(first.startsWith("file:///"));
            QVERIFY(path.startsWith(art_dir + "/"));
            QVERIFY(path.endsWith(".jpg"));
            QFile file(path);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), "first cover");
            QCOMPARE(artwork.set("first cover"), first);

            // A new cover is a new URL, and the old file goes
            const QString second = artwork.set("second cover");
            QVERIFY(second != first);
            QVERIFY(!QFile::exists(path));
            QCOMPARE(QDir(art_dir).entryList(QDir::Files).size(), 1);
            QCOMPARE(artwork.set({}), QString());
            QCOMPARE(QDir(art_dir).entryList(QDir::Files).size(), 0);
            first = artwork.set("first cover");
        }
        QVERIFY(!QFile::exists(QUrl(first).toLocalFile()));
    }

    // Needs a session bus. sendspin-server-cpp in this process stands in for Music Assistant.
    void isAPlayerOnTheBusWhileAServerIsConnected() {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) {
            QSKIP("no session bus");
        }
        // A name of the test's own: the app may be running on this desktop
        const QString instance = QStringLiteral(".test%1").arg(QCoreApplication::applicationPid());
        const QString service = "org.mpris.MediaPlayer2.com.sendspin_audio.Sendspin" + instance;
        const auto on_the_bus = [&] {
            return bus.interface()->isServiceRegistered(service).value();
        };

        const QTemporaryDir dir;
        Commands commands;
        ServerPersistence server_persistence(dir.path().toStdString());
        server::SendspinServer server;
        server::ServerConfig server_config;
        server_config.listen_port = 0;
        server_config.advertise = false;
        server_config.browse = false;
        QCOMPARE(server.start(server_config, &server_persistence, &commands),
                 server::StartResult::OK);

        ClientPersistence persistence(dir.path().toStdString());
        Client client({.name = "Test PC", .port = Client::free_port(0), .advertise = false},
                      &persistence);
        Mpris mpris(client, dir.filePath("art"), instance);
        QDBusInterface properties(service, "/org/mpris/MediaPlayer2",
                                  "org.freedesktop.DBus.Properties", bus);
        QDBusInterface player(service, "/org/mpris/MediaPlayer2", "org.mpris.MediaPlayer2.Player",
                              bus);
        const auto get = [&](const char* name) {
            const QDBusReply<QVariant> reply =
                properties.call("Get", "org.mpris.MediaPlayer2.Player", name);
            return reply.value();
        };

        QVERIFY(client.start());
        QVERIFY(!on_the_bus());
        QVERIFY(
            server.connect_url("ws://127.0.0.1:" + std::to_string(client.port()) + "/sendspin"));
        QTRY_COMPARE(client.state(), Client::State::Connected);
        QTRY_VERIFY(on_the_bus());

        // The roles need pairing
        const std::string id = client.client_id().toStdString();
        QVERIFY(server.begin_pairing(id, {}));
        QTRY_COMPARE(client.pairing_code().size(), 6);
        QTRY_VERIFY(server.submit_pairing_code(id, client.pairing_code().toStdString()));
        QTRY_VERIFY(client.is_paired());
        QTRY_COMPARE(client.state(), Client::State::Connected);
        QTRY_VERIFY(on_the_bus());

        const QDBusReply<QVariantMap> root = properties.call("GetAll", "org.mpris.MediaPlayer2");
        QVERIFY2(root.isValid(), qPrintable(root.error().message()));
        QCOMPARE(root.value().value("Identity"), QVariant("Sendspin"));
        QCOMPARE(root.value().value("DesktopEntry"), QVariant("com.sendspin_audio.Sendspin"));
        // Idle: nothing to show
        QCOMPARE(get("PlaybackStatus"), QVariant("Stopped"));
        QCOMPARE(get("CanPlay"), QVariant(false));

        // The group gets a track, a cover and commands, and plays
        const auto group = server.default_group();
        QVERIFY(group.has_value());
        server::ControllerCaps caps;
        caps.commands = {server::TransportCommand::PLAY, server::TransportCommand::PAUSE,
                         server::TransportCommand::NEXT};
        QVERIFY(server.set_controller_caps(*group, caps));
        server::Metadata metadata;
        metadata.title = "Blue in Green";
        metadata.artist = "Miles Davis";
        metadata.progress = {.track_progress_ms = 60'000, .track_duration_ms = 337'000};
        QVERIFY(server.set_metadata(*group, metadata, server.now_us()));
        QImage cover(8, 8, QImage::Format_RGB32);
        cover.fill(Qt::darkBlue);
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        QVERIFY(cover.save(&buffer, "JPEG"));
        const auto* jpeg_bytes = reinterpret_cast<const uint8_t*>(jpeg.constData());
        QVERIFY(server.set_artwork(*group, server::ArtworkSource::ALBUM,
                                   {jpeg_bytes, static_cast<size_t>(jpeg.size())},
                                   server.now_us()));
        const server::OpenedInput opened =
            server.open_input(*group, {.format = {48000, 2, server::SampleFormat::S16}});
        QCOMPARE(opened.result, server::OpenInputResult::OK);
        const std::vector<int16_t> silence(48000 * 2, 0);
        size_t written = 0;
        QTRY_VERIFY((written += opened.input->write(silence.data(), 48000 - written).frames) ==
                    48000);

        QTRY_COMPARE(get("PlaybackStatus"), QVariant("Playing"));
        QCOMPARE(get("CanPlay"), QVariant(true));
        QTRY_COMPARE(get("CanGoNext"), QVariant(true));
        QCOMPARE(get("CanGoPrevious"), QVariant(false));
        QTRY_VERIFY(!client.now_playing().artwork().isEmpty());
        QVariantMap shown;
        QTRY_VERIFY((shown = qdbus_cast<QVariantMap>(get("Metadata")),
                     shown.contains("mpris:artUrl") && shown.contains("xesam:title")));
        QCOMPARE(shown.value("xesam:title"), QVariant("Blue in Green"));
        QCOMPARE(shown.value("xesam:artist"), QVariant(QStringList{"Miles Davis"}));
        QCOMPARE(shown.value("mpris:length").toLongLong(), 337'000'000);
        QCOMPARE(shown.value("mpris:trackid").value<QDBusObjectPath>().path().left(35),
                 "/com/sendspin_audio/Sendspin/track/");
        // The cover is a file the desktop can open, the image the server sent
        const QString art_path = QUrl(shown.value("mpris:artUrl").toString()).toLocalFile();
        QVERIFY(art_path.startsWith(dir.filePath("art/")));
        QVERIFY(!QImage(art_path).isNull());
        QVERIFY(get("Position").toLongLong() >= 60'000'000);

        // A media key: the command reaches the server, and one it does not offer never leaves
        QVERIFY(player.call("Previous").type() != QDBusMessage::ErrorMessage);
        QVERIFY(player.call("Next").type() != QDBusMessage::ErrorMessage);
        QVERIFY(player.call("PlayPause").type() != QDBusMessage::ErrorMessage);
        QTRY_COMPARE(commands.get(), (std::vector{server::TransportCommand::NEXT,
                                                  server::TransportCommand::PAUSE}));

        // Off in Settings, and on again
        mpris.set_enabled(false);
        QVERIFY(!on_the_bus());
        mpris.set_enabled(true);
        QVERIFY(on_the_bus());

        // The server goes: so does the player, and the cover with it
        server.stop();
        QTRY_VERIFY(client.state() != Client::State::Connected);
        QTRY_VERIFY(!on_the_bus());
        QVERIFY(!QFile::exists(art_path));
    }
};

QTEST_MAIN(MprisTest)
#include "mpris_test.moc"
