#include "notifier.h"

#include "skip_unless_required.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QImage>
#include <QSignalSpy>
#include <QTest>

namespace {

const QString SERVICE = QStringLiteral("com.sendspin_audio.Test.NotificationPortal");
const QString PATH = QStringLiteral("/org/freedesktop/portal/desktop");

/// The notification portal, as far as the app uses it: keeps what is showing, by id
class FakePortal : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.portal.Notification")

public:
    QMap<QString, QVariantMap> showing_;
    int added_ = 0;
    int removed_ = 0;

    /// As the desktop does when the notification, or a button on it, is clicked
    void invoke(const QString& id, const QString& action, const QVariantList& parameter = {}) {
        QDBusMessage signal =
            QDBusMessage::createSignal(PATH, QStringLiteral("org.freedesktop.portal.Notification"),
                                       QStringLiteral("ActionInvoked"));
        signal << id << action << QVariant(parameter);
        QDBusConnection::sessionBus().send(signal);
    }

    QList<QVariantMap> buttons(const QString& id) const {
        return qdbus_cast<QList<QVariantMap>>(
            this->showing_.value(id).value("buttons").value<QDBusArgument>());
    }

public slots:
    void AddNotification(const QString& id, const QVariantMap& notification) {
        ++this->added_;
        this->showing_.insert(id, notification);
    }
    void RemoveNotification(const QString& id) {
        ++this->removed_;
        this->showing_.remove(id);
    }
};

const QString DESKTOP = QStringLiteral("com.sendspin_audio.Test.Notifications");
const QString DESKTOP_PATH = QStringLiteral("/org/freedesktop/Notifications");

/// org.freedesktop.Notifications, as far as the app uses it: keeps what is showing, by id
class FakeDesktop : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.freedesktop.Notifications")

public:
    struct Shown {
        QString summary;
        QString body;
        QStringList actions;
        QVariantMap hints;
        int expire_timeout;
    };
    QMap<quint32, Shown> showing_;
    int notified_ = 0;
    int closed_ = 0;

    /// The `image-data` hint of one showing, as the image it carries; null without one
    QImage image(quint32 id) const {
        const QVariant hint = this->showing_.value(id).hints.value("image-data");
        if (!hint.canConvert<QDBusArgument>()) {
            return {};
        }
        const auto data = hint.value<QDBusArgument>();
        int width = 0, height = 0, stride = 0, bits = 0, channels = 0;
        bool alpha = false;
        QByteArray pixels;
        data.beginStructure();
        data >> width >> height >> stride >> alpha >> bits >> channels >> pixels;
        data.endStructure();
        if (bits != 8 || channels != 4 || !alpha || pixels.size() != stride * height) {
            return {};
        }
        return QImage(reinterpret_cast<const uchar*>(pixels.constData()), width, height, stride,
                      QImage::Format_RGBA8888)
            .copy();
    }

public slots:
    uint Notify(const QString& /*app_name*/, uint replaces_id, const QString& /*app_icon*/,
                const QString& summary, const QString& body, const QStringList& actions,
                const QVariantMap& hints, int expire_timeout) {
        ++this->notified_;
        const quint32 id = replaces_id != 0 ? replaces_id : ++this->last_id_;
        this->showing_.insert(id, {summary, body, actions, hints, expire_timeout});
        return id;
    }
    void CloseNotification(uint id) {
        ++this->closed_;
        this->showing_.remove(id);
    }

private:
    quint32 last_id_ = 0;
};

/// Lets a notifier have the id of what it showed: the stand-in has it a moment sooner
void let_the_id_arrive() {
    QTest::qWait(100);
}

QImage cover(int width, int height) {
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(Qt::darkBlue);
    return image;
}

}  // namespace

class NotifierTest : public QObject {
    Q_OBJECT

private slots:
    void init() {
        QDBusConnection bus = QDBusConnection::sessionBus();
        if (!bus.isConnected()) {
            SKIP_UNLESS_REQUIRED("no session bus");
        }
        this->portal_ = std::make_unique<FakePortal>();
        QVERIFY(bus.registerService(SERVICE));
        QVERIFY(bus.registerObject(PATH, this->portal_.get(), QDBusConnection::ExportAllSlots));
        this->desktop_ = std::make_unique<FakeDesktop>();
        QVERIFY(bus.registerService(DESKTOP));
        QVERIFY(bus.registerObject(DESKTOP_PATH, this->desktop_.get(),
                                   QDBusConnection::ExportAllSlots));
    }

    void cleanup() {
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.unregisterObject(PATH);
        bus.unregisterService(SERVICE);
        this->portal_.reset();
        bus.unregisterObject(DESKTOP_PATH);
        bus.unregisterService(DESKTOP);
        this->desktop_.reset();
    }

    void showsThroughThePortal() {
        Notifier notifier("pairing", SERVICE, true);
        notifier.show("Pairing code: 123-456", "Enter it in the server to pair this PC.", true);
        QTRY_COMPARE(this->portal_->added_, 1);
        QCOMPARE(this->portal_->showing_.size(), 1);
        const QVariantMap shown = this->portal_->showing_.first();
        QCOMPARE(shown.value("title").toString(), "Pairing code: 123-456");
        QCOMPARE(shown.value("body").toString(), "Enter it in the server to pair this PC.");
        QCOMPARE(shown.value("priority").toString(), "high");
        QCOMPARE(shown.value("default-action").toString(), "default");
        QVERIFY(!shown.contains("buttons"));
    }

    void aNewOneTakesThePlaceOfTheOneShowing() {
        Notifier notifier("pairing", SERVICE, true);
        notifier.show("Pairing code: 123-456", {}, true);
        notifier.show("Paired with Music Assistant");
        QTRY_COMPARE(this->portal_->added_, 2);
        QCOMPARE(this->portal_->showing_.size(), 1);
        const QVariantMap shown = this->portal_->showing_.first();
        QCOMPARE(shown.value("title").toString(), "Paired with Music Assistant");
        QCOMPARE(shown.value("priority").toString(), "normal");
        QVERIFY(!shown.contains("body"));
    }

    void closeRemovesIt() {
        Notifier notifier("pairing", SERVICE, true);
        notifier.show("Pairing code: 123-456", {}, true);
        notifier.close();
        QTRY_COMPARE(this->portal_->removed_, 1);
        QCOMPARE(this->portal_->added_, 1);
        QVERIFY(this->portal_->showing_.isEmpty());
    }

    void aClickAndTheButtonAreToldApart() {
        Notifier notifier("pairing", SERVICE, true);
        QSignalSpy clicked(&notifier, &Notifier::clicked);
        QSignalSpy action(&notifier, &Notifier::actionInvoked);
        notifier.show("A server wants to pair with this PC", {}, true, "Allow pairing");
        QTRY_COMPARE(this->portal_->added_, 1);
        const QString id = this->portal_->showing_.firstKey();
        const QList<QVariantMap> buttons = this->portal_->buttons(id);
        QCOMPARE(buttons.size(), 1);
        QCOMPARE(buttons.first().value("label").toString(), "Allow pairing");

        this->portal_->invoke(id, "default");
        QTRY_COMPARE(clicked.count(), 1);
        QCOMPARE(clicked.first().first().toString(), QString());
        QCOMPARE(action.count(), 0);

        this->portal_->invoke(id, buttons.first().value("action").toString());
        QTRY_COMPARE(action.count(), 1);
        QCOMPARE(clicked.count(), 1);
    }

    // Version 2 of the portal: what lets the window that opens come to the front on Wayland
    void aClickBringsItsActivationToken() {
        Notifier notifier("pairing", SERVICE, true);
        QSignalSpy clicked(&notifier, &Notifier::clicked);
        notifier.show("Pairing code: 123-456", {}, true);
        QTRY_COMPARE(this->portal_->added_, 1);
        this->portal_->invoke(this->portal_->showing_.firstKey(), "default",
                              {QVariantMap{{"activation-token", "token-1"}}});
        QTRY_COMPARE(clicked.count(), 1);
        QCOMPARE(clicked.first().first().toString(), "token-1");
    }

    // The portal has no timeout to ask for
    void aPassingOneIsRemovedAfterAWhile() {
        Notifier notifier("pairing", SERVICE, true);
        auto* expiry = notifier.findChild<QTimer*>();
        QVERIFY(expiry != nullptr);
        notifier.show("Pairing code: 123-456", {}, true);
        QVERIFY(!expiry->isActive());
        notifier.show("Paired with Music Assistant");
        QVERIFY(expiry->isActive());
        QVERIFY(expiry->interval() >= 5000);
        // Not the ten seconds
        expiry->setInterval(1);
        expiry->start();
        QTRY_COMPARE(this->portal_->removed_, 1);
        QVERIFY(this->portal_->showing_.isEmpty());

        // A code that takes its place stays
        notifier.show("Paired with Music Assistant");
        notifier.show("Pairing code: 123-456", {}, true);
        QVERIFY(!expiry->isActive());
    }

    void anotherNotificationsActionIsNotOurs() {
        Notifier notifier("pairing", SERVICE, true);
        QSignalSpy clicked(&notifier, &Notifier::clicked);
        notifier.show("Pairing code: 123-456", {}, true);
        QTRY_COMPARE(this->portal_->added_, 1);
        this->portal_->invoke("someone-elses", "default");
        this->portal_->invoke(this->portal_->showing_.firstKey(), "default");
        // In order on the one connection: the first would have arrived by now
        QTRY_COMPARE(clicked.count(), 1);
    }

    // ==========================================================================
    // A track
    // ==========================================================================

    void aTrackShowsWithItsCoverAndStaysOutOfTheList() {
        Notifier notifier("now-playing", DESKTOP, false);
        notifier.show_track("Blue in Green", "Miles Davis — Kind of Blue", cover(512, 512));
        QTRY_COMPARE(this->desktop_->notified_, 1);
        const quint32 id = this->desktop_->showing_.firstKey();
        const FakeDesktop::Shown shown = this->desktop_->showing_.value(id);
        QCOMPARE(shown.summary, "Blue in Green");
        QCOMPARE(shown.body, "Miles Davis — Kind of Blue");
        QCOMPARE(shown.hints.value("transient"), QVariant(true));
        // Low urgency would have no banner on GNOME, and a sound is the desktop's to choose
        QVERIFY(!shown.hints.contains("urgency"));
        QVERIFY(!shown.hints.contains("sound-file"));
        QVERIFY(!shown.hints.contains("sound-name"));
        // Goes when the desktop says, and a click is all there is on it
        QCOMPARE(shown.expire_timeout, -1);
        QCOMPARE(shown.actions.size(), 2);
        QCOMPARE(shown.actions.first(), "default");
        // The cover, smaller than it came
        const QImage image = this->desktop_->image(id);
        QCOMPARE(image.size(), QSize(256, 256));
        QCOMPARE(image.pixelColor(100, 100), QColor(Qt::darkBlue));
    }

    void aTrackWithoutACoverShowsWithoutOne() {
        Notifier notifier("now-playing", DESKTOP, false);
        notifier.show_track("Blue in Green", {}, {});
        QTRY_COMPARE(this->desktop_->notified_, 1);
        QVERIFY(!this->desktop_->showing_.first().hints.contains("image-data"));
    }

    void theNextTrackTakesThePlaceOfTheLast() {
        Notifier notifier("now-playing", DESKTOP, false);
        notifier.show_track("So What", {}, {});
        QTRY_COMPARE(this->desktop_->notified_, 1);
        // Before the first one's id is back, as on a skip: it is closed when its id is known
        notifier.show_track("Freddie Freeloader", {}, {});
        QTRY_COMPARE(this->desktop_->notified_, 2);
        QTRY_COMPARE(this->desktop_->showing_.size(), 1);
        QCOMPARE(this->desktop_->showing_.first().summary, "Freddie Freeloader");
        // With its id known, the next one is put in its place
        let_the_id_arrive();
        notifier.show_track("Blue in Green", {}, {});
        QTRY_COMPARE(this->desktop_->notified_, 3);
        QCOMPARE(this->desktop_->showing_.size(), 1);
        QCOMPARE(this->desktop_->showing_.first().summary, "Blue in Green");
        QCOMPARE(this->desktop_->closed_, 1);

        notifier.close();
        QTRY_VERIFY(this->desktop_->showing_.isEmpty());
    }

    void aTrackLeavesThePairingNotificationAlone() {
        Notifier pairing("pairing", DESKTOP, false);
        Notifier track("now-playing", DESKTOP, false);
        QSignalSpy pairing_clicked(&pairing, &Notifier::clicked);
        QSignalSpy track_clicked(&track, &Notifier::clicked);
        pairing.show("Pairing code: 123-456", {}, true);
        QTRY_COMPARE(this->desktop_->notified_, 1);
        const quint32 pairing_id = this->desktop_->showing_.firstKey();
        track.show_track("So What", {}, {});
        QTRY_COMPARE(this->desktop_->showing_.size(), 2);
        let_the_id_arrive();
        track.show_track("Freddie Freeloader", {}, {});
        QTRY_COMPARE(this->desktop_->notified_, 3);
        // Both show, and the code is as it was: there until closed, with nothing of a track's
        QCOMPARE(this->desktop_->showing_.size(), 2);
        const FakeDesktop::Shown code = this->desktop_->showing_.value(pairing_id);
        QCOMPARE(code.summary, "Pairing code: 123-456");
        QCOMPARE(code.expire_timeout, 0);
        QVERIFY(code.hints.isEmpty());

        // A click on one is not a click on the other
        quint32 track_id = 0;
        for (auto it = this->desktop_->showing_.begin(); it != this->desktop_->showing_.end();
             ++it) {
            if (it.key() != pairing_id) {
                track_id = it.key();
            }
        }
        QDBusMessage signal = QDBusMessage::createSignal(
            DESKTOP_PATH, "org.freedesktop.Notifications", "ActionInvoked");
        signal << track_id << "default";
        QDBusConnection::sessionBus().send(signal);
        QTRY_COMPARE(track_clicked.count(), 1);
        QCOMPARE(pairing_clicked.count(), 0);

        track.close();
        QTRY_COMPARE(this->desktop_->closed_, 1);
        QCOMPARE(this->desktop_->showing_.size(), 1);
        QVERIFY(this->desktop_->showing_.contains(pairing_id));
    }

    void aTrackShowsThroughThePortalBesideThePairingNotification() {
        Notifier pairing("pairing", SERVICE, true);
        Notifier track("now-playing", SERVICE, true);
        QSignalSpy pairing_clicked(&pairing, &Notifier::clicked);
        QSignalSpy track_clicked(&track, &Notifier::clicked);
        pairing.show("Pairing code: 123-456", {}, true);
        // Not square: the portal's icon is
        track.show_track("So What", "Miles Davis — Kind of Blue", cover(512, 384));
        track.show_track("Blue in Green", "Miles Davis — Kind of Blue", cover(512, 384));
        QTRY_COMPARE(this->portal_->added_, 3);
        QCOMPARE(this->portal_->showing_.size(), 2);
        QCOMPARE(this->portal_->showing_.value("pairing").value("title").toString(),
                 "Pairing code: 123-456");
        QVERIFY(!this->portal_->showing_.value("pairing").contains("icon"));

        const QVariantMap shown = this->portal_->showing_.value("now-playing");
        QCOMPARE(shown.value("title").toString(), "Blue in Green");
        QCOMPARE(shown.value("body").toString(), "Miles Davis — Kind of Blue");
        // "low" has no banner on GNOME
        QCOMPARE(shown.value("priority").toString(), "normal");
        QCOMPARE(shown.value("default-action").toString(), "default");
        // Only keys version 1 of the portal knows: it turns down a notification with any other
        for (auto it = shown.begin(); it != shown.end(); ++it) {
            QVERIFY2(QStringList({"title", "body", "icon", "priority", "default-action"})
                         .contains(it.key()),
                     qPrintable(it.key()));
        }
        const auto icon = shown.value("icon").value<QDBusArgument>();
        QCOMPARE(icon.currentSignature(), "(sv)");
        QString kind;
        QDBusVariant bytes;
        icon.beginStructure();
        icon >> kind >> bytes;
        icon.endStructure();
        QCOMPARE(kind, "bytes");
        const QImage image = QImage::fromData(bytes.variant().toByteArray(), "PNG");
        QCOMPARE(image.size(), QSize(192, 192));

        this->portal_->invoke("now-playing", "default",
                              {QVariantMap{{"activation-token", "token-2"}}});
        QTRY_COMPARE(track_clicked.count(), 1);
        QCOMPARE(track_clicked.first().first().toString(), "token-2");
        QCOMPARE(pairing_clicked.count(), 0);

        // It goes by itself, and the code stays
        auto* expiry = track.findChild<QTimer*>();
        QVERIFY(expiry->isActive());
        expiry->setInterval(1);
        expiry->start();
        QTRY_COMPARE(this->portal_->removed_, 1);
        QCOMPARE(this->portal_->showing_.keys(), QStringList{"pairing"});
    }

private:
    std::unique_ptr<FakePortal> portal_;
    std::unique_ptr<FakeDesktop> desktop_;
};

QTEST_GUILESS_MAIN(NotifierTest)
#include "notifier_test.moc"
