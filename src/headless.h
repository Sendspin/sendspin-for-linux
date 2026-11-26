#pragma once

#include <optional>

/// @brief The cast engine without the UI, for development: `--list` prints the speakers as they
/// change, `--add <name>` and `--remove <name>` put a speaker in the group or take it out,
/// `--play-along` adds this PC's own output. Runs, and casts, until interrupted. `--update`
/// checks for a newer release now, installs it where the app may (docs/SPEC.md, "Updates"), and
/// exits.
/// @param version this build's
/// @return The exit code when the arguments ask for it; nullopt for the normal app
std::optional<int> run_headless(int argc, char** argv, const char* version);
