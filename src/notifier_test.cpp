#include "notifier.h"

#include "skip_unless_required.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
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
    void invoke(const QString& id, const QString& action) {
        QDBusMessage signal =
            QDBusMessage::createSignal(PATH, QStringLiteral("org.freedesktop.portal.Notification"),
                                       QStringLiteral("ActionInvoked"));
        signal << id << action << QVariant(QVariantList());
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
    }

    void cleanup() {
        QDBusConnection bus = QDBusConnection::sessionBus();
        bus.unregisterObject(PATH);
        bus.unregisterService(SERVICE);
        this->portal_.reset();
    }

    void showsThroughThePortal() {
        Notifier notifier(SERVICE);
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
        Notifier notifier(SERVICE);
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
        Notifier notifier(SERVICE);
        notifier.show("Pairing code: 123-456", {}, true);
        notifier.close();
        QTRY_COMPARE(this->portal_->removed_, 1);
        QCOMPARE(this->portal_->added_, 1);
        QVERIFY(this->portal_->showing_.isEmpty());
    }

    void aClickAndTheButtonAreToldApart() {
        Notifier notifier(SERVICE);
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
        QCOMPARE(action.count(), 0);

        this->portal_->invoke(id, buttons.first().value("action").toString());
        QTRY_COMPARE(action.count(), 1);
        QCOMPARE(clicked.count(), 1);
    }

    void anotherNotificationsActionIsNotOurs() {
        Notifier notifier(SERVICE);
        QSignalSpy clicked(&notifier, &Notifier::clicked);
        notifier.show("Pairing code: 123-456", {}, true);
        QTRY_COMPARE(this->portal_->added_, 1);
        this->portal_->invoke("someone-elses", "default");
        this->portal_->invoke(this->portal_->showing_.firstKey(), "default");
        // In order on the one connection: the first would have arrived by now
        QTRY_COMPARE(clicked.count(), 1);
    }

private:
    std::unique_ptr<FakePortal> portal_;
};

QTEST_GUILESS_MAIN(NotifierTest)
#include "notifier_test.moc"
