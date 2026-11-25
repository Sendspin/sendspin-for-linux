#pragma once

#include <sendspin/server/types.h>

#include <cstdint>
#include <optional>
#include <set>
#include <string>

/// @brief The picker's rules, apart from the app: which speakers are listed, how a row shows its
/// state, what a choice leads to and what the engine does about it. Plain logic over plain
/// values; `speaker_menu_test.cpp` has one test per rule. After sendspin-mac's `SpeakerMenu`,
/// as adapted in docs/SPEC.md, "The picker".
namespace speaker_menu {

// ==============================================================================
// Rows
// ==============================================================================

/// @brief Where a speaker's connection stands
enum class Connection : uint8_t {
    CONNECTED,
    CONNECTING,
    /// Connected and held until the user adds it
    WAITING,
    UNAVAILABLE,
    IN_USE_ELSEWHERE,
    DISCONNECTED,
    CANT_CONNECT,
    CANT_PLAY,
    NEEDS_PAIRING,
    NEEDS_PAIRING_AGAIN,
    IDENTITY_CHANGED,
};

/// @brief What a row says about why it can't play; the UI words it
enum class Status : uint8_t {
    NONE,
    CONNECTING,
    UNAVAILABLE,
    IN_USE_ELSEWHERE,
    DISCONNECTED,
    CANT_CONNECT,
    CANT_PLAY,
    NEEDS_PAIRING,
    NEEDS_PAIRING_AGAIN,
    IDENTITY_CHANGED,
};

/// @brief What a row knows about its output
struct Output {
    /// This PC's own output, not a Sendspin speaker
    bool is_local{false};
    /// It can be in a group: every Sendspin speaker and this PC's output; a server can't
    bool is_groupable{true};
    /// In the group as the picker shows it: what plays, or added and not playing yet
    bool in_group{false};
    bool plays{false};
    /// Found on the network; a speaker in the group stays listed while it isn't
    bool is_discovered{true};
    Connection connection{Connection::CONNECTED};
};

/// @brief What the picker around a row knows
struct Context {
    /// What plays now is groupable: not a server
    bool plays_groupable{true};
    /// How many outputs are in the group
    int group_count{0};
};

enum class Check : uint8_t {
    /// Not groupable, or what plays isn't: nothing can join
    NONE,
    /// Not in the group
    EMPTY,
    /// In the group and playing
    FILLED,
    /// In the group, but it can't play now
    GREY,
    /// In the group and connecting
    SPINNER,
};

enum class Click : uint8_t { NONE, ADD, REMOVE };

/// @brief How a row shows its state. One indicator per row: the check where the output is
/// groupable, the highlighted icon where it isn't.
struct Row {
    Check check{Check::NONE};
    /// What clicking the check does
    Click click{Click::NONE};
    /// The icon is highlighted while it plays: only where there is no check to say it
    bool highlighted{false};
    /// Something is wrong with it
    bool warning{false};
    Status status{Status::NONE};

    bool operator==(const Row&) const = default;
};

/// @brief What you chose stays; what you didn't, leaves
bool is_listed(bool in_group, bool is_discovered);

Row row(const Output& output, const Context& context);

/// @brief It can play now, or will once connected: not gone, taken or refused
bool can_play(const Output& output);

/// @brief A server this PC is a source for, as an output: not groupable, "in the group" while
/// chosen and "found" while connected
Output server(bool chosen, bool connected);

// ==============================================================================
// From the server library's events
// ==============================================================================

/// @brief What a disconnect leaves a speaker as; nullopt when it says nothing about the speaker
/// (our own stop, disconnect or Forget)
std::optional<Connection> after_disconnect(const sendspin::server::ClientDisconnect& disconnect);

/// @brief What an error leaves a speaker as; nullopt for an error that isn't about a speaker's
/// connection
std::optional<Connection> after_error(const sendspin::server::ServerError& error);

/// @brief What the server library says about a speaker
struct Facts {
    /// The user added it
    bool ticked{false};
    bool connected{false};
    bool paired{false};
    /// The operator's approval, which lets it play unpaired
    bool approved{false};
    /// It plays for servers it isn't paired with, once approved
    bool unpaired_access{false};
    /// It lost its pairing with this PC
    bool credential_mismatch{false};
    /// A member of the group the app casts into
    bool in_cast{false};
};

/// @brief A connected speaker's connection; one that is not connected has the reason it left
Connection connection(const Facts& facts);

/// @brief What the engine does next for a speaker, so that only what is ticked plays
enum class Step : uint8_t {
    NONE,
    /// approve(PLAYBACK): adding a speaker lets it play unpaired
    APPROVE,
    /// begin_pairing(): adding a speaker that only plays for paired servers pairs it. Once per
    /// connection; what the user cancels is not asked for again.
    PAIR,
    MOVE_TO_CAST,
    /// Out of the cast group, into the server's own group, which never plays
    MOVE_TO_LOBBY,
    /// revoke_approval(): a speaker that was only approved is held alone again
    REVOKE,
};

Step next_step(const Facts& facts);

// ==============================================================================
// Choices
// ==============================================================================

/// @brief Where the PC's sound goes
enum class Route : uint8_t {
    /// Through Sendspin, to the outputs in the group
    SPEAKERS,
    /// Straight to this PC's own output, without Sendspin and its delay
    DIRECT,
    /// Through a Sendspin server, which plays it where it chooses
    SERVER,
};

/// @brief What the picker shows, as far as choosing goes
struct State {
    /// The key of this PC's own output
    std::string local;
    std::set<std::string> group;
    /// Outputs that can play now, or will once connected
    std::set<std::string> can_play;
};

struct Action {
    enum class Kind : uint8_t {
        /// Its check, empty
        ADD,
        /// Its check, filled
        REMOVE,
        /// Its name: play there only
        PLAY_ONLY,
        /// A server can't be grouped with: the group dissolves
        CHOOSE_SERVER,
    };
    Kind kind{Kind::ADD};
    std::string id;
};

/// @brief What clicking a row's name does
enum class NameClick : uint8_t {
    /// Play there only
    PLAY_ONLY,
    /// Play there only, and dial it again: another server took it, and the library doesn't take
    /// it back by itself
    PLAY_ONLY_AND_DIAL,
    /// "Identity changed": accept the new identity; the group stays as it is
    ACCEPT_IDENTITY,
};

NameClick name_click(const Row& row);

/// @brief What a choice leads to
struct Plan {
    std::set<std::string> group;
    Route route{Route::DIRECT};

    bool operator==(const Plan&) const = default;
};

/// @return nullopt when the action changes nothing
std::optional<Plan> plan(const Action& action, const State& state);

/// @brief Where the sound goes for a group as it stands. Whether its speakers can play now does
/// not come into it: there is no automatic fallback.
Route route(const State& state);

// ==============================================================================
// The tray icon and the dot
// ==============================================================================

/// @brief Sendspin plays: the PC's sound reaches a Sendspin speaker, or a server's stream. This
/// PC's own output alone doesn't count.
bool sendspin_plays(bool has_sound, Route route, bool speaker_connected, bool server_streaming);

enum class Dot : uint8_t {
    PLAYS,
    /// Sendspin is the output, but nothing goes out
    IN_USE,
    NOT_IN_USE,
};

Dot dot(bool sendspin_is_output, bool sendspin_plays);

}  // namespace speaker_menu
