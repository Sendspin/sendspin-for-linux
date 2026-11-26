#include "pairing_clipboard.h"

#include <QClipboard>
#include <QGuiApplication>
#include <QMimeData>
#include <QTest>

class PairingClipboardTest : public QObject {
    Q_OBJECT

private slots:
    void init() {
        QGuiApplication::clipboard()->clear();
    }

    void copyPutsTheCodeOnTheClipboard() {
        QVERIFY(!pairing_clipboard::holds("123456"));
        pairing_clipboard::copy("123456");
        QCOMPARE(QGuiApplication::clipboard()->text(), "123456");
        QVERIFY(pairing_clipboard::holds("123456"));
        QVERIFY(!pairing_clipboard::holds("654321"));
    }

    void copyIsMarkedASecret() {
        pairing_clipboard::copy("123456");
        const QMimeData* data = QGuiApplication::clipboard()->mimeData();
        QCOMPARE(data->data("x-kde-passwordManagerHint"), QByteArray("secret"));
        QCOMPARE(data->text(), "123456");
    }

    void clearTakesTheCodeOff() {
        pairing_clipboard::copy("123456");
        pairing_clipboard::clear("123456");
        QCOMPARE(QGuiApplication::clipboard()->text(), QString());
    }

    void clearLeavesWhatWasCopiedSince() {
        pairing_clipboard::copy("123456");
        QGuiApplication::clipboard()->setText("the user's own");
        pairing_clipboard::clear("123456");
        QCOMPARE(QGuiApplication::clipboard()->text(), "the user's own");
    }

    // No code was shown: an empty clipboard is not ours to clear, nor does it hold a code
    void noCodeIsNeverHeld() {
        QVERIFY(!pairing_clipboard::holds(QString()));
        QGuiApplication::clipboard()->setText("the user's own");
        pairing_clipboard::clear(QString());
        QCOMPARE(QGuiApplication::clipboard()->text(), "the user's own");
    }
};

QTEST_MAIN(PairingClipboardTest)
#include "pairing_clipboard_test.moc"
