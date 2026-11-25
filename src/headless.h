#pragma once

#include <optional>

/// @brief The cast engine without the UI, for development: `--list` prints the speakers as they
/// change, `--add <name>` and `--remove <name>` put a speaker in the group or take it out. It
/// keeps running, and casting, until interrupted.
/// @return The exit code when the arguments ask for it; nullopt for the normal app
std::optional<int> run_headless(int argc, char** argv);
