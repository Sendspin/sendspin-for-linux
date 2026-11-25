#pragma once

#include <QString>

/// @brief The pairing code a server asks for, on the clipboard: the six digits, never the token
namespace pairing_clipboard {

/// @brief Asks for the code to be the clipboard's text. A Wayland compositor may refuse a client
/// that hasn't the focus, and says so a moment later: holds() tells, once the event loop ran.
void copy(const QString& code);
/// @brief Whether the clipboard's text is this code, as far as this app can tell
bool holds(const QString& code);
/// @brief Takes the code off the clipboard, and nothing that was copied since
void clear(const QString& code);

}  // namespace pairing_clipboard
