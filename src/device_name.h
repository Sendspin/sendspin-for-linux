#pragma once

#include <QString>

/// @brief The name this PC goes by on the network: its pretty hostname ("Chris's Laptop") when
/// one is set, else its hostname.
/// @param machine_info the file that holds PRETTY_HOSTNAME; tests pass their own
QString device_name(const QString& machine_info = QStringLiteral("/etc/machine-info"));
