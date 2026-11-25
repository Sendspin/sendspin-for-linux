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
        return this->announcer.findChild<QTimer*>();
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
        QImage cover(8, 8, QImage::Format_RGB32);
        cover.fill(Qt::darkBlue);
        QByteArray jpeg;
        QBuffer buffer(&jpeg);
        QVERIFY(cover.save(&buffer, "JPEG"));

        Announcer a;
        a.announcer.update(true, SO_WHAT, {});
        // The cover follows its track
        a.announcer.update(true, SO_WHAT, jpeg);
        a.wait();
        QCOMPARE(a.announced.size(), 1);
        QCOMPARE(a.announced.first().at(2).value<QImage>().size(), QSize(8, 8));

        // What is not a picture is no cover
        a.announcer.update(true, BLUE_IN_GREEN, "not a JPEG");
        a.wait();
        QCOMPARE(a.announced.size(), 2);
        QVERIFY(a.announced.last().at(2).value<QImage>().isNull());
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
        const auto* settle = announcer.findChild<QTimer*>();
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
