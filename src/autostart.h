#pragma once

#include <QString>

/// @brief Open at login, as an XDG autostart entry of the user's own
/// (`$XDG_CONFIG_HOME/autostart/<app id>.desktop`)
namespace autostart {

QString path();

/// @brief The entry is there and not switched off
bool enabled();

/// @brief Writes the entry, or removes it
/// @param program what it starts; the running executable by default
/// @return false when the file could not be written or removed
bool set_enabled(bool enabled, const QString& program = {});

/// @brief program as a desktop entry's Exec value
QString exec_value(const QString& program);

}  // namespace autostart
