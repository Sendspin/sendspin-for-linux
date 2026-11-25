#include "track_announcer.h"

#include <QBuffer>
#include <QSignalSpy>
#include <QTest>

namespace {

const Track SO_WHAT{.title = "So What", .artist = "Miles Davis", .album = "Kind of Blue"};
const Track BLUE_IN_GREEN{
    .title = "Blue in Green", .artist = "Miles Davis", .album = "Kind of Blue"};

/// An announcer that is switched on, and what it announced. Its wait is cut to nothing: a
/// track is due at the next turn of the event loop.
struct Announcer {
    Announcer() {
        this->announcer.set_enabled(true);
        this->settle()->setInterval(0);
    }
    QTimer* settle() {
        return this->announcer.findChild<QTimer*>("settle");
    }
    QTimer* cover_wait() {
        return this->announcer.findChild<QTimer*>("cover");
    }
    QImage last_cover() const {
        return this->announced.last().at(2).value<QImage>();
    }
    /// Lets a track that is due be announced
    void wait() {
        QTest::qWait(20);
    }
    QStringList titles() const {
        QStringList titles;
        for (const auto& arguments : this->announced) {
            titles << arguments.first().toString();
        }
        return titles;
    }

    TrackAnnouncer announcer;
    QSignalSpy announced{&announcer, &TrackAnnouncer::announced};
};

/// A cover of one colour, as the JPEG a server sends
QByteArray jpeg(Qt::GlobalColor colour) {
    QImage cover(8, 8, QImage::Format_RGB32);
    cover.fill(colour);
    QByteArray bytes;
    QBuffer buffer(&bytes);
    cover.save(&buffer, "JPEG");
    return bytes;
}

const Track OTHER_ALBUM{.title = "Naima", .artist = "John Coltrane", .album = "Giant Steps"};

}  // namespace

class TrackAnnouncerTest : public QObject {
    Q_OBJECT

private slots:
    void aTrackThatStartsPlayingIsAnnounced() {
        Announcer a;
        a.announcer.update(false, {}, {});
        a.announcer.update(true, SO_WHAT, {});
        a.wait();
        QCOMPARE(a.announced.size(), 1);
        QCOMPARE(a.announced.first().at(0).toString(), "So What");
        QCOMPARE(a.announced.first().at(1).toString(), "Miles Davis — Kind of Blue");
        QVERIFY(a.announced.first().at(2).value<QImage>().isNull());
    }

    void theCoverComesWithIt() {
        Announcer a;
        a.announcer.update(true, SO_WHAT, {});
        // The cover follows its track
        a.announcer.update(true, SO_WHAT, jpeg(Qt::darkBlue));
        a.wait();
        QCOMPARE(a.announced.size(), 1);
        QCOMPARE(a.last_cover().size(), QSize(8, 8));

        // The next track of the album has the cover that is there
        a.announcer.update(true, BLUE_IN_GREEN, jpeg(Qt::darkBlue));
        a.wait();
        QCOMPARE(a.announced.size(), 2);
        QVERIFY(!a.last_cover().isNull());

        // What is not a picture is no cover
        a.announcer.update(true, OTHER_ALBUM, "not a JPEG");
        a.wait();
        QCOMPARE(a.announced.size(), 3);
        QVERIFY(a.last_cover().isNull());
    }

    // A wrong cover beside a new title is worse than none
    void anotherAlbumsCoverIsNotShown() {
        Announcer a;
        a.announcer.update(true, SO_WHAT, jpeg(Qt::darkBlue));
        a.wait();
        QVERIFY(!a.last_cover().isNull());
        // The track is here and its cover is not: the last album's still is
        a.announcer.update(true, OTHER_ALBUM, jpeg(Qt::darkBlue));
        a.wait();
        QCOMPARE(a.titles(), (QStringList{"So What", "Naima"}));
        QVERIFY(a.last_cover().isNull());

        // Nor for a track with no album to go by
        a.announcer.update(true, {.title = "Freddie Freeloader"}, jpeg(Qt::darkBlue));
        a.wait();
        QCOMPARE(a.announced.size(), 3);
        QVERIFY(a.last_cover().isNull());
    }

    void aCoverThatComesLateIsAddedOnce() {
        Announcer a;
        a.announcer.update(true, SO_WHAT, jpeg(Qt::darkBlue));
        a.wait();
        a.announcer.update(true, OTHER_ALBUM, jpeg(Qt::darkBlue));
        a.wait();
        QCOMPARE(a.announced.size(), 2);
        QVERIFY(a.last_cover().isNull());
        QVERIFY(a.cover_wait()->isActive());
        QVERIFY(a.cover_wait()->interval() >= 3000);

        // Its own cover: the same track again, with it
        a.announcer.update(true, OTHER_ALBUM, jpeg(Qt::darkRed));
        QCOMPARE(a.titles(), (QStringList{"So What", "Naima", "Naima"}));
        QCOMPARE(a.announced.last().at(1).toString(), "John Coltrane — Giant Steps");
        QCOMPARE(a.last_cover().size(), QSize(8, 8));
        QVERIFY(qRed(a.last_cover().pixel(4, 4)) > qBlue(a.last_cover().pixel(4, 4)));

        // Once: another cover after that, and the position moving on, change nothing
        a.announcer.update(true, OTHER_ALBUM, jpeg(Qt::darkGreen));
        a.announcer.update(true, OTHER_ALBUM, jpeg(Qt::darkGreen));
        a.wait();
        QCOMPARE(a.announced.size(), 3);
    }

    void aCoverThatComesTooLateIsNotAdded() {
        Announcer a;
        a.announcer.update(true, SO_WHAT, {});
        a.wait();
        QCOMPARE(a.announced.size(), 1);
        // The notification has gone by now
        a.cover_wait()->setInterval(1);
        a.cover_wait()->start();
        QTRY_VERIFY(!a.cover_wait()->isActive());
        a.announcer.update(true, SO_WHAT, jpeg(Qt::darkBlue));
        a.wait();
        QCOMPARE(a.announced.size(), 1);
    }

    void aLateCoverIsNotAddedToATrackThatIsNoLongerTheLatest() {
        Announcer a;
        a.settle()->setInterval(50);
        a.announcer.update(true, SO_WHAT, {});
        QTRY_COMPARE(a.announced.size(), 1);
        // Skipped on: the cover that comes now is the next track's, which is not due yet
        a.announcer.update(true, OTHER_ALBUM, {});
        a.announcer.update(true, OTHER_ALBUM, jpeg(Qt::darkRed));
        QCOMPARE(a.announced.size(), 1);
        QTRY_COMPARE(a.announced.size(), 2);
        QCOMPARE(a.titles(), (QStringList{"So What", "Naima"}));
        QVERIFY(!a.last_cover().isNull());

        // Switched off while one is awaited
        a.announcer.update(true, BLUE_IN_GREEN, {});
        QTRY_COMPARE(a.announced.size(), 3);
        a.announcer.set_enabled(false);
        a.announcer.update(true, BLUE_IN_GREEN, jpeg(Qt::darkBlue));
        QCOMPARE(a.announced.size(), 3);
    }

    void theBodyIsWhatThereIsOfArtistAndAlbum() {
        QCOMPARE(TrackAnnouncer::body(SO_WHAT), "Miles Davis — Kind of Blue");
        QCOMPARE(TrackAnnouncer::body({.title = "So What", .artist = "Miles Davis"}),
                 "Miles Davis");
        QCOMPARE(TrackAnnouncer::body({.title = "So What", .album = "Kind of Blue"}),
                 "Kind of Blue");
        QCOMPARE(TrackAnnouncer::body({.title = "So What"}), QString());
    }

    void theNextTrackIsAnnouncedWhileItPlays() {
        Announcer a;
        a.announcer.update(true, SO_WHAT, {});
        a.wait();
        a.announcer.update(true, BLUE_IN_GREEN, {});
        a.wait();
        QCOMPARE(a.titles(), (QStringList{"So What", "Blue in Green"}));
    }

    void aPauseAndAResumeAreNoNews() {
        Announcer a;
        a.announcer.update(true, SO_WHAT, {});
        a.wait();
        a.announcer.update(false, SO_WHAT, {});
        a.wait();
        a.announcer.update(true, SO_WHAT, {});
        a.wait();
        QCOMPARE(a.announced.size(), 1);
    }

    // The position, the volume, the commands, the cover and a seek all come as a change
    void theSameTrackAgainIsNoNews() {
        Announcer a;
        a.announcer.update(true, SO_WHAT, {});
        a.wait();
        Track moved = SO_WHAT;
        moved.duration_ms = 562'000;
        moved.number = 1;
        for (int i = 0; i < 5; ++i) {
            a.announcer.update(true, moved, "a cover");
            a.wait();
        }
        QCOMPARE(a.announced.size(), 1);
    }

    void aRunOfSkipsIsAnnouncedOnce() {
        Announcer a;
        a.settle()->setInterval(100);
        a.announcer.update(true, SO_WHAT, {});
        QTest::qWait(20);
        a.announcer.update(true, {.title = "Freddie Freeloader"}, {});
        QTest::qWait(20);
        // The same track again does not put it off
        a.announcer.update(true, BLUE_IN_GREEN, {});
        QTest::qWait(60);
        a.announcer.update(true, BLUE_IN_GREEN, {});
        QCOMPARE(a.announced.size(), 0);
        QTRY_COMPARE(a.announced.size(), 1);
        QCOMPARE(a.titles(), QStringList{"Blue in Green"});
        QTest::qWait(150);
        QCOMPARE(a.announced.size(), 1);
    }

    void theWaitIsAboutASecond() {
        TrackAnnouncer announcer;
        const auto* settle = announcer.findChild<QTimer*>("settle");
        QVERIFY(settle->isSingleShot());
        QCOMPARE(settle->interval(), 1000);
    }

    void aTrackChangedWhilePausedIsAnnouncedWhenItPlays() {
        Announcer a;
        a.announcer.update(true, SO_WHAT, {});
        a.wait();
        a.announcer.update(false, SO_WHAT, {});
        a.announcer.update(false, BLUE_IN_GREEN, {});
        a.wait();
        QCOMPARE(a.announced.size(), 1);
        a.announcer.update(true, BLUE_IN_GREEN, {});
        a.wait();
        QCOMPARE(a.titles(), (QStringList{"So What", "Blue in Green"}));
    }

    // Skipped to and paused before it was due: it is still news when it plays
    void aTrackPausedBeforeItWasDueIsAnnouncedWhenItPlays() {
        Announcer a;
        a.settle()->setInterval(50);
        a.announcer.update(true, SO_WHAT, {});
        a.announcer.update(false, SO_WHAT, {});
        QTest::qWait(100);
        QCOMPARE(a.announced.size(), 0);
        a.announcer.update(true, SO_WHAT, {});
        QTRY_COMPARE(a.announced.size(), 1);
    }

    void theSameTrackAfterIdleIsANewStart() {
        Announcer a;
        a.announcer.update(true, SO_WHAT, {});
        a.wait();
        // Idle: stopped, with no track
        a.announcer.update(false, {}, {});
        a.announcer.update(true, SO_WHAT, {});
        a.wait();
        QCOMPARE(a.announced.size(), 2);
    }

    void aTrackWithoutATitleIsNotAnnounced() {
        Announcer a;
        a.announcer.update(true, {.artist = "Miles Davis"}, {});
        a.wait();
        QCOMPARE(a.announced.size(), 0);
    }

    void nothingIsAnnouncedWhileSwitchedOff() {
        Announcer a;
        a.announcer.set_enabled(false);
        a.announcer.update(true, SO_WHAT, {});
        a.wait();
        QCOMPARE(a.announced.size(), 0);
        // Switched on mid-track: the next track is the first to show
        a.announcer.set_enabled(true);
        a.announcer.update(true, SO_WHAT, {});
        a.wait();
        QCOMPARE(a.announced.size(), 0);
        a.announcer.update(true, BLUE_IN_GREEN, {});
        a.wait();
        QCOMPARE(a.titles(), QStringList{"Blue in Green"});
    }
};

QTEST_GUILESS_MAIN(TrackAnnouncerTest)
#include "track_announcer_test.moc"
