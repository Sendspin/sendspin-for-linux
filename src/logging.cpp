#include "logging.h"

#include <QLoggingCategory>

#include <sendspin/client.h>

// sendspin-cpp has no log callback: it writes to standard error itself, and this category only
// decides how much
Q_LOGGING_CATEGORY(lc_client_lib, "sendspin.client.lib")

void set_log_rules(bool verbose) {
    // Explicit either way: a distro's qtlogging.ini may turn debug off, and Qt's own default
    // turns it on
    QLoggingCategory::setFilterRules(verbose ? QStringLiteral("sendspin.*.debug=true")
                                             : QStringLiteral("sendspin.*.debug=false"));
    // The library's info is the app's debug: each connection, each message it acts on
    sendspin::SendspinClient::set_log_level(
        lc_client_lib().isDebugEnabled() ? sendspin::LogLevel::INFO : sendspin::LogLevel::WARN);
}
