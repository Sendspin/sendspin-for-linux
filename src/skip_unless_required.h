#pragma once

#include <QTest>

/// Skips a test that needs something the machine may lack: a PipeWire daemon, a tool, a session
/// bus. Under ci/start-pipewire.sh all of them are there, so a skip would be a silent pass and
/// fails instead.
#define SKIP_UNLESS_REQUIRED(why)                                                       \
    do {                                                                                \
        QVERIFY2(!qEnvironmentVariableIsSet("SENDSPIN_REQUIRE_PIPEWIRE_TESTS"), (why)); \
        QSKIP(why);                                                                     \
    } while (false)
