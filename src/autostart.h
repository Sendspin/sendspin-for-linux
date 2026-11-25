#pragma once

#include <QString>

/// @brief Open at login, as an XDG autostart entry of the user's own
/// (`$XDG_CONFIG_HOME/autostart/<app id>.desktop`)
namespace autostart {

QString path();

/// @brief False under Flatpak, where the entry would start a path that only exists inside the
/// sandbox; there it is the Background portal's to do
bool available();

/// @brief The entry is there and not switched off
bool enabled();

/// @brief Writes the entry, or removes it
/// @param program what it starts; the running executable by default
/// @return false when the file could not be written or removed
bool set_enabled(bool enabled, const QString& program = {});

/// @brief Writes the entry again while it is enabled and starts something else than program:
/// the app moved, or starts differently than when the entry was written
void refresh(const QString& program = {});

/// @brief program as a desktop entry's Exec value
QString exec_value(const QString& program);

}  // namespace autostart
