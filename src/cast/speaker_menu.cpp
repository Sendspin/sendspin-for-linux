#include "cast/speaker_menu.h"

namespace speaker_menu {

using sendspin::server::DisconnectReason;
using sendspin::server::ErrorKind;
using sendspin::server::GoodbyeReason;

namespace {

/// Why an output in the picker can't play now; NONE while it can
Status problem(const Output& output) {
    if (!output.is_discovered) {
        return Status::UNAVAILABLE;
    }
    switch (output.connection) {
        case Connection::UNAVAILABLE:
            return Status::UNAVAILABLE;
        case Connection::IN_USE_ELSEWHERE:
            return Status::IN_USE_ELSEWHERE;
        case Connection::DISCONNECTED:
            return Status::DISCONNECTED;
        case Connection::CANT_CONNECT:
            return Status::CANT_CONNECT;
        case Connection::CANT_PLAY:
            return Status::CANT_PLAY;
        case Connection::NEEDS_PAIRING:
            return Status::NEEDS_PAIRING;
        case Connection::NEEDS_PAIRING_AGAIN:
            return Status::NEEDS_PAIRING_AGAIN;
        case Connection::IDENTITY_CHANGED:
            return Status::IDENTITY_CHANGED;
        case Connection::CONNECTED:
        case Connection::CONNECTING:
        case Connection::WAITING:
            return Status::NONE;
    }
    return Status::NONE;
}

/// Only for what needs attention. A speaker out of the group says nothing, unless it's gone,
/// another server has it, or adding it will ask for pairing.
Status status(const Output& output, Status problem, bool has_check) {
    if (!output.is_discovered) {
        return Status::UNAVAILABLE;
    }
    if (!output.in_group) {
        const bool said_anyway =
            problem == Status::IN_USE_ELSEWHERE || problem == Status::NEEDS_PAIRING;
        return said_anyway ? problem : Status::NONE;
    }
    if (problem != Status::NONE) {
        return problem;
    }
    // Where there's a check, its spinner says it
    const bool connecting =
        output.connection == Connection::CONNECTING || output.connection == Connection::WAITING;
    return connecting && !has_check ? Status::CONNECTING : Status::NONE;
}

/// Something must be able to play after a choice. With no Sendspin speaker left this PC's own
/// output takes over, played directly. With only speakers that can't play now, it joins them,
/// and they join in once they're back.
Plan settle(std::set<std::string> group, const State& state) {
    bool any_speaker = false;
    bool any_can_play = false;
    for (const std::string& id : group) {
        if (id != state.local) {
            any_speaker = true;
            any_can_play = any_can_play || state.can_play.contains(id);
        }
    }
    if (!any_speaker) {
        return {{state.local}, Route::DIRECT};
    }
    if (!any_can_play) {
        group.insert(state.local);
    }
    return {std::move(group), Route::SPEAKERS};
}

}  // namespace

bool is_listed(bool in_group, bool is_discovered) {
    return in_group || is_discovered;
}

Row row(const Output& output, const Context& context) {
    const bool has_check = output.is_groupable && context.plays_groupable;
    const Status why = problem(output);

    Row row;
    if (!has_check) {
        row.check = Check::NONE;
    } else if (!output.in_group) {
        row.check = Check::EMPTY;
    } else if (output.plays) {
        row.check = Check::FILLED;
    } else if (why != Status::NONE) {
        row.check = Check::GREY;
    } else {
        row.check = Check::SPINNER;
    }

    if (!has_check) {
        row.click = Click::NONE;
    } else if (!output.in_group) {
        row.click = Click::ADD;
    } else {
        // A Sendspin speaker can always go: when nothing left can play, this PC's own output
        // takes over. This PC's own output can't when it's all that plays.
        row.click = !output.is_local || context.group_count > 1 ? Click::REMOVE : Click::NONE;
    }

    row.status = status(output, why, has_check);
    row.highlighted = !output.is_groupable && output.plays;
    // "In use elsewhere" is another server's choice, and "Needs pairing" before it's added is
    // what adding it will ask for: neither is a fault
    const bool fault =
        why != Status::IN_USE_ELSEWHERE && (why != Status::NEEDS_PAIRING || output.in_group);
    row.warning = row.status != Status::NONE && row.status == why && fault;
    return row;
}

bool can_play(const Output& output) {
    return problem(output) == Status::NONE;
}

Output server(bool chosen, bool connected) {
    Output output;
    output.is_groupable = false;
    output.in_group = chosen;
    output.plays = chosen && connected;
    output.is_discovered = connected;
    output.connection = connected ? Connection::CONNECTED : Connection::UNAVAILABLE;
    return output;
}

std::optional<Connection> after_disconnect(const sendspin::server::ClientDisconnect& disconnect) {
    switch (disconnect.reason) {
        case DisconnectReason::CONNECTION_LOST:
            return Connection::UNAVAILABLE;
        case DisconnectReason::CLOSED_BY_SERVER:
            return Connection::CANT_CONNECT;
        case DisconnectReason::SERVER_STOPPED:
        case DisconnectReason::DISCONNECTED_BY_HOST:
            return std::nullopt;
        case DisconnectReason::GOODBYE:
            break;
    }
    if (!disconnect.goodbye_reason) {
        // A reason this server doesn't know: it left, and that is all there is to say
        return Connection::UNAVAILABLE;
    }
    switch (*disconnect.goodbye_reason) {
        case GoodbyeReason::SHUTDOWN:
        case GoodbyeReason::RESTART:
            return Connection::UNAVAILABLE;
        case GoodbyeReason::ANOTHER_SERVER:
        case GoodbyeReason::CONCURRENT_ATTEMPT:
            return Connection::IN_USE_ELSEWHERE;
        case GoodbyeReason::USER_REQUEST:
            return Connection::DISCONNECTED;
        case GoodbyeReason::UNAUTHORIZED:
            return Connection::CANT_CONNECT;
        case GoodbyeReason::PAIRING_REQUIRED:
            return Connection::NEEDS_PAIRING;
        // The answer to our own Forget: a new speaker again
        case GoodbyeReason::UNPAIRED:
            return std::nullopt;
    }
    return Connection::UNAVAILABLE;
}

std::optional<Connection> after_error(const sendspin::server::ServerError& error) {
    switch (error.kind) {
        case ErrorKind::DIAL_FAILED:
        case ErrorKind::HANDSHAKE_FAILED:
            return Connection::CANT_CONNECT;
        case ErrorKind::NO_PLAYABLE_FORMAT:
            return Connection::CANT_PLAY;
        case ErrorKind::GOODBYE_BEFORE_ACTIVATION:
            if (error.goodbye_reason) {
                // The same goodbye says the same thing before activation as after it
                return after_disconnect({DisconnectReason::GOODBYE, error.goodbye_reason});
            }
            return Connection::CANT_CONNECT;
        case ErrorKind::DISCOVERY_UNAVAILABLE:
        case ErrorKind::PIPELINE_FAILED:
        case ErrorKind::PERSISTENCE_FAILED:
        case ErrorKind::SOURCE_FAILED:
            return std::nullopt;
    }
    return std::nullopt;
}

Connection connection(const Facts& facts) {
    if (facts.credential_mismatch) {
        return Connection::NEEDS_PAIRING_AGAIN;
    }
    if (facts.paired || (facts.approved && facts.unpaired_access)) {
        return Connection::CONNECTED;
    }
    // Approved or not, it plays only once paired
    return facts.unpaired_access ? Connection::WAITING : Connection::NEEDS_PAIRING;
}

Step next_step(const Facts& facts) {
    if (!facts.ticked) {
        // Approval is what adding gave it; a paired speaker keeps its pairing
        if (facts.approved && !facts.paired) {
            return Step::REVOKE;
        }
        return facts.connected && facts.in_cast ? Step::MOVE_TO_LOBBY : Step::NONE;
    }
    if (!facts.connected || facts.in_cast) {
        return Step::NONE;
    }
    switch (connection(facts)) {
        case Connection::CONNECTED:
            return Step::MOVE_TO_CAST;
        case Connection::WAITING:
            return Step::APPROVE;
        default:
            return Step::PAIR;
    }
}

NameClick name_click(const Row& row) {
    switch (row.status) {
        case Status::IDENTITY_CHANGED:
            return NameClick::ACCEPT_IDENTITY;
        case Status::IN_USE_ELSEWHERE:
            return NameClick::PLAY_ONLY_AND_DIAL;
        default:
            return NameClick::PLAY_ONLY;
    }
}

Route route(const State& state) {
    // An empty group is what choosing a server leaves
    for (const std::string& id : state.group) {
        if (id != state.local) {
            return Route::SPEAKERS;
        }
    }
    return state.group.empty() ? Route::SERVER : Route::DIRECT;
}

std::optional<Plan> plan(const Action& action, const State& state) {
    std::set<std::string> group = state.group;
    switch (action.kind) {
        case Action::Kind::CHOOSE_SERVER:
            return Plan{{}, Route::SERVER};
        case Action::Kind::PLAY_ONLY:
            return settle({action.id}, state);
        case Action::Kind::ADD:
            group.insert(action.id);
            return settle(std::move(group), state);
        case Action::Kind::REMOVE:
            if (!group.contains(action.id) || group == std::set<std::string>{state.local}) {
                return std::nullopt;
            }
            group.erase(action.id);
            return settle(std::move(group), state);
    }
    return std::nullopt;
}

bool local_is_late(int64_t delay_ns, int64_t buffer_ns, bool late) {
    if (delay_ns == 0 || buffer_ns == 0) {
        return false;
    }
    return delay_ns + ((late ? 2 : 1) * LOCAL_MARGIN_NS) > buffer_ns;
}

bool sendspin_plays(bool has_sound, Route route, bool speaker_connected, bool server_streaming) {
    if (!has_sound) {
        return false;
    }
    switch (route) {
        case Route::SPEAKERS:
            return speaker_connected;
        case Route::SERVER:
            return server_streaming;
        case Route::DIRECT:
            return false;
    }
    return false;
}

Dot dot(bool sendspin_is_output, bool sendspin_plays) {
    if (!sendspin_is_output) {
        return Dot::NOT_IN_USE;
    }
    return sendspin_plays ? Dot::PLAYS : Dot::IN_USE;
}

}  // namespace speaker_menu
