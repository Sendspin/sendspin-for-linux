#include "audio/outputs.h"

#include "audio/sendspin_output.h"
#include "skip_unless_required.h"

#include <QProcess>
#include <QSettings>
#include <QTest>

namespace {

const QByteArray OWN = "sendspin";
const std::vector<OutputSink> SINKS{
    {40, "speakers", 1000}, {41, "sendspin", 0}, {42, "headphones", 1500}, {43, "hdmi", 600}};

std::optional<QByteArray> choose(const QByteArray& default_sink, const QByteArray& chosen = {},
                                 const QByteArray& last_default = {}) {
    return choose_target(SINKS, default_sink, chosen, OWN, last_default);
}

}  // namespace

class OutputsTest : public QObject {
    Q_OBJECT

private slots:
    // The private daemon and the settings are every test's: none leaves a default behind
    void cleanup() {
        if (qEnvironmentVariableIsSet("SENDSPIN_REQUIRE_PIPEWIRE_TESTS")) {
            QProcess::execute("pw-metadata", {"-d", "0", "default.configured.audio.sink"});
            QSettings(QStringLiteral("sendspin"), QStringLiteral("sendspin")).remove("outputs");
        }
    }

    void followsTheDefault() {
        QCOMPARE(choose("speakers"), "speakers");
        QCOMPARE(choose("hdmi", {}, "speakers"), "hdmi");
    }

    void theChosenOutputWins() {
        QCOMPARE(choose("speakers", "hdmi"), "hdmi");
        // Unplugged: back to the default
        QCOMPARE(choose("speakers", "usb-dac"), "speakers");
    }

    // Loop guard 1: what a server plays here must not come back in as this PC's input
    void neverTheSendspinOutput() {
        QCOMPARE(choose("sendspin", {}, "speakers"), "speakers");
        QCOMPARE(choose("sendspin", "sendspin", "hdmi"), "hdmi");
        QCOMPARE(choose("speakers", "sendspin"), "speakers");
        // No default seen before it, or that one is gone: the best of the others
        QCOMPARE(choose("sendspin"), "headphones");
        QCOMPARE(choose("sendspin", {}, "usb-dac"), "headphones");
    }

    void noDefaultTakesTheBestOutput() {
        QCOMPARE(choose({}), "headphones");
        QCOMPARE(choose("gone"), "headphones");
    }

    void nowhereToPlayWithOnlyTheSendspinOutput() {
        QCOMPARE(choose_target({{41, "sendspin", 0}}, "sendspin", {}, OWN, {}), std::nullopt);
        QCOMPARE(choose_target({}, {}, {}, OWN, {}), std::nullopt);
    }

    // Needs a PipeWire daemon with a session manager
    void listsTheGraph() {
        SendspinOutput own(nullptr, "sendspin-test-outputs");
        QString error;
        if (!own.start(&error)) {
            SKIP_UNLESS_REQUIRED(qPrintable(QStringLiteral("no PipeWire daemon: ") + error));
        }
        Outputs outputs("sendspin-test-outputs");
        QVERIFY(outputs.start());
        QVERIFY(outputs.clock().rate != 0);
        // Its own output is in the graph, and asked what it plays
        const SinkFormats formats = outputs.formats("sendspin-test-outputs");
        QVERIFY(formats.known());
        QVERIFY(formats.plays(SendspinOutput::RATE));
        QVERIFY(formats.over_16_bit);
        QVERIFY(!outputs.formats("no-such-output").known());
        // Whatever else there is, the target is never that one
        QVERIFY(outputs.target() != std::optional<QByteArray>("sendspin-test-outputs"));

        // As the only choice left it is still refused
        outputs.set_chosen("sendspin-test-outputs");
        QVERIFY(outputs.target() != std::optional<QByteArray>("sendspin-test-outputs"));
    }

    // Needs a PipeWire daemon with a session manager and another output
    void takesTheDefaultAndGivesItBack() {
        // It changes the system's default output: never on a desktop's own daemon
        if (!qEnvironmentVariableIsSet("SENDSPIN_REQUIRE_PIPEWIRE_TESTS")) {
            QSKIP("only against the private daemon of ci/start-pipewire.sh");
        }
        SendspinOutput own(nullptr, "sendspin-test-default");
        QString error;
        if (!own.start(&error)) {
            SKIP_UNLESS_REQUIRED(qPrintable(QStringLiteral("no PipeWire daemon: ") + error));
        }
        Outputs outputs("sendspin-test-default");
        QVERIFY(outputs.start());
        const std::optional<OutputSink> local = outputs.local();
        if (!local) {
            SKIP_UNLESS_REQUIRED("no output besides the Sendspin output");
        }
        QVERIFY(local->name != "sendspin-test-default");
        QVERIFY(!local->description.isEmpty());
        QVERIFY(!outputs.own_is_default());

        // The user's own choice of default is an output that is unplugged now
        QCOMPARE(QProcess::execute("pw-metadata", {"0", "default.configured.audio.sink",
                                                   R"({"name":"usb-dac"})", "Spa:String:JSON"}),
                 0);
        QTRY_COMPARE(outputs.configured_default(), "usb-dac");

        QVERIFY(outputs.set_own_default(true));
        QTRY_VERIFY(outputs.own_is_default());
        // This PC's own output is still the one it replaced
        QCOMPARE(outputs.local()->name, local->name);

        // Handing it back puts the user's choice back, not whatever plays in its absence, and a
        // restart in between doesn't lose it
        Outputs restarted("sendspin-test-default");
        QVERIFY(restarted.start());
        QVERIFY(restarted.set_own_default(false));
        QTRY_VERIFY(!outputs.own_is_default());
        QTRY_COMPARE(outputs.configured_default(), "usb-dac");
        QCOMPARE(outputs.local()->name, local->name);

        // With nothing replaced there is nothing to put back
        QVERIFY(outputs.set_own_default(false));
        QCOMPARE(outputs.configured_default(), "usb-dac");
    }

    // The user never chose a default: there is none afterwards either
    void givesBackNoChoiceWhereThereWasNone() {
        if (!qEnvironmentVariableIsSet("SENDSPIN_REQUIRE_PIPEWIRE_TESTS")) {
            QSKIP("only against the private daemon of ci/start-pipewire.sh");
        }
        SendspinOutput own(nullptr, "sendspin-test-default");
        QVERIFY(own.start());
        Outputs outputs("sendspin-test-default");
        QVERIFY(outputs.start());
        QTRY_VERIFY(outputs.local().has_value());
        QCOMPARE(outputs.configured_default(), "");

        QVERIFY(outputs.set_own_default(true));
        QTRY_VERIFY(outputs.own_is_default());
        QVERIFY(outputs.set_own_default(false));
        QTRY_VERIFY(!outputs.own_is_default());
        QTRY_COMPARE(outputs.configured_default(), "");
    }

    // Without the Sendspin output there is nothing to make the default
    void neverMakesAMissingOutputTheDefault() {
        if (!qEnvironmentVariableIsSet("SENDSPIN_REQUIRE_PIPEWIRE_TESTS")) {
            QSKIP("only against the private daemon of ci/start-pipewire.sh");
        }
        Outputs outputs("sendspin-test-missing");
        QVERIFY(outputs.start());
        QVERIFY(!outputs.set_own_default(true));
        QCOMPARE(outputs.configured_default(), "");
    }
};

QTEST_GUILESS_MAIN(OutputsTest)
#include "outputs_test.moc"
