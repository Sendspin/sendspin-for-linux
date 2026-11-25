#pragma once

#include <QObject>
#include <QString>

#include <functional>

/// @brief Open at login. An XDG autostart entry of the user's own
/// (`$XDG_CONFIG_HOME/autostart/<app id>.desktop`), over the one a package installs for every
/// user (`/etc/xdg/autostart/`); under Flatpak, the Background portal's. Either starts the app
/// with `--background`: the tray alone.
namespace autostart {

/// @brief The user's own entry
QString path();

/// @brief Under Flatpak, where the host's entries are out of reach: set_enabled() asks the
/// portal there, and enabled() is what it last answered
bool sandboxed();

bool enabled();

/// @brief Switches it on or off. The user's entry is written only where it differs from the
/// package's: on with none installed, off against one that is.
/// @param program what it starts; the running executable by default
/// @param context cancels done when it is gone
/// @param done runs once enabled() is up to date: before this returns, or under Flatpak when
/// the portal has answered
void set_enabled(bool enabled, const QString& program = {}, QObject* context = nullptr,
                 const std::function<void()>& done = {});

/// @brief Writes the user's entry again while it is enabled and starts something else than
/// program: the app moved, or starts differently than when the entry was written
void refresh(const QString& program = {});

/// @brief program as a desktop entry's Exec value
QString exec_value(const QString& program);

/// @brief Asks the Background portal for autostart, or to end it
/// @param service the portal's bus name; a test's stand-in
/// @param done runs with what was granted: false too when there is no portal to ask
void ask_portal(bool enabled, const QString& service, QObject* context,
                const std::function<void(bool granted)>& done);

}  // namespace autostart
