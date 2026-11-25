#pragma once

/// @brief Sets what the `sendspin.*` categories log: info and up, and their debug lines too
/// when verbose. The one place the default is set; QT_LOGGING_RULES overrides it either way.
/// Call it once the application object exists, before anything logs.
void set_log_rules(bool verbose);
