# Research notes

For [SPEC.md](SPEC.md). Linux desktop and Music Assistant findings were collected 2026-09-10;
the libraries and sendspin-mac were read 2026-11-24. Spec references are to
[Sendspin/spec](https://github.com/Sendspin/spec) `main` at `f4edee9` (2026-09-10, not re-read).

## Scope

- [backlog#42](https://github.com/Sendspin/backlog/issues/42) — desktop tray app / driver
  (parent). Its MVP is a client (player + input source); its "AVP" is the built-in server that
  casts to speakers like AirPlay. Open, no comments.
- [backlog#46](https://github.com/Sendspin/backlog/issues/46) — rich client for Linux; GUI
  distros only, headless stays `sendspin-cli`. Open, no comments.
- **The Mac app built the AVP first**, and the client side only as a source. This spec follows
  it and adds #42's player role.
- [backlog#43](https://github.com/Sendspin/backlog/issues/43) — one app for all OSes vs one per
  OS. Moot here: the Mac app is native Swift, this one is native C++/Qt.
- [backlog#56](https://github.com/Sendspin/backlog/issues/56) — extend `source@v1` with
  metadata, controls and artwork. Still what it would take to show the casting app's track in MA.
- Siblings: [#44](https://github.com/Sendspin/backlog/issues/44) macOS,
  [#45](https://github.com/Sendspin/backlog/issues/45) Windows.

## What we take from sendspin-mac

[Sendspin/sendspin-mac](https://github.com/Sendspin/sendspin-mac) `main` at `32444d6`; latest
tag v0.10.1.

**Taken as is**

- The model (`README.md`, "How it fits together"): one "Sendspin" output device, an app that
  reads it back and is a Sendspin server for the speakers, and a `source@v1` client for Music
  Assistant on the side.
- The picker's rules, rows and statuses (`docs/menu-states.md`). SPEC.md restates them for
  Linux.
- One destination at a time: speakers or the server; the server's stream gets silence while
  speakers are chosen (`README.md`, "How it works", Music Assistant).
- The speaker buffer reported as the device's output latency, for lip sync ("Timing").
- Idle: `stream/end` after 3 s of digital silence ("Idle").
- The local output plays along as a host-driven output, not as a Sendspin player ("The Mac's own
  output"), following drift with single frames dropped or repeated.
- As a client: dynamic pairing code as digits, Pairing PSK offered, never a static code (`docs/menu-states.md`, "Pairing"; `TODO.md` item 1).
- Music Assistant is recognised by a `_mass._tcp` record carrying the server's name.
- A hidden list of speaker URLs for networks where mDNS fails.

**Different on Linux**

| Mac | Linux |
|---|---|
| A HAL driver, installed with a password | A PipeWire node the app creates; nothing to install |
| Server and client written in Swift (`SendspinServerKit`) | sendspin-server-cpp and sendspin-cpp |
| The app computes the speaker buffer and group volume | The server library does both |
| Menu bar menu with sliders | A Qt Quick window; SNI menus hold no sliders |
| Keychain | Files, mode 0600 |
| No player role | `player@v1` with controller, metadata, artwork, and MPRIS |
| AirPlay outputs can't be grouped | No equivalent; every hardware sink can play along |
| Local Network permission notice | Avahi-not-running notice |
| QR codes scanned with the camera | Not in the MVP |
| The pairing token is shown nowhere | Copy pairing token in Settings |
| A volume slider per speaker | One slider, the group's volume |
| The device's volume and mute drive the speakers, not the audio ("Device") | PipeWire scales the captured audio; the picker's slider drives the speakers |
| The buffer grows for the Mac's own output | Not yet; the library's lead counts Sendspin players only |

**The Mac's open items that apply here too** (`TODO.md`): feeding the server's other roles
(item 2), more under Music Assistant (5), more than one server (6), Opus and
`set_output_delay` (10). Item 8, the player's preferred format, the server library already does.

## Protocol

As of 2026-09-10 unless noted.

- **Roles:** `roles/*/v1.md`. `source@v1` merged in
  [spec#105](https://github.com/Sendspin/spec/pull/105), refined in
  [#225](https://github.com/Sendspin/spec/pull/225) and
  [#256](https://github.com/Sendspin/spec/pull/256).
- **A source cannot pick its target.** spec#105 removed `select_source`. With Music Assistant
  the user picks the speakers there. Casting from the app goes around this by being the server.
- **Source streaming** (`roles/source/v1.md`):
  - starts only on `server/command {source: start}`
  - chunks are binary type 12 with capture time in server clock, 5–150 ms
  - no `available: true` before convergence
  - a player+source device must not play its input locally
- **`signal` is a hint.** The server MAY use it to start or stop.
- **Encryption** (`connection.md`, `messaging.md`): Noise `KKpsk2`, server always the initiator,
  `25519_ChaChaPoly_SHA256` / `25519_AESGCM_SHA256`; `client_id` is the client's Curve25519
  public key.
- **Pairing** (`pairing.md`):
  - servers implement all three methods; clients offer Pairing PSK plus at most one code method
  - keep ≥5 pairing records
- **Unpaired access** ("guest mode" in MA and the backlog):
  [spec#170](https://github.com/Sendspin/spec/pull/170),
  [#242](https://github.com/Sendspin/spec/pull/242). `source@v1` needs explicit approval.
- **Sendspin 1 is a release candidate** (sendspin-mac `README.md`, October 2026).
- **Volume** (spec `README.md` at `2cfe197`, read 2026-11-24):
  - "Player messages" (line 1333): 0–100 is perceived loudness; clients SHOULD use
    `amplitude = (volume / 100)^1.5` and ramp changes. `volume` and `muted` are independent.
  - "Setting group volume" (line 1674): group volume is the average of the players that support
    volume; a change applies the same delta to each, clamps, redistributes what was clamped, and
    sends one command per player.
- **Discovery** (`connection.md`):
  - clients advertise `_sendspin._tcp` (port 8928, TXT `path`, `name`)
  - servers advertise `_sendspin-server._tcp` (port 8927)
  - one admitted connection per client; priority `playback` > `pairing` > idle

## Libraries

### sendspin-server-cpp

[Sendspin/sendspin-server-cpp](https://github.com/Sendspin/sendspin-server-cpp) `main` at
`c33d6e5`. Apache-2.0. No tags; `README.md` says the API changes without deprecation.

- **Entry point** `include/sendspin/server/server.h`: `SendspinServer`, `ServerListener`,
  `PersistenceProvider`, `GroupInput`. Every call but `start()` and the destructor returns
  without waiting; listener calls come on one dispatch thread.
- **Discovery** (`config.h`, `discovery.h`): browses `_sendspin._tcp` and dials, advertises
  `_sendspin-server._tcp`. Built-in provider is avahi-client on Linux; a host can supply its own
  `DiscoveryProvider`. Without avahi-daemon the server runs and reports
  `ErrorKind::DISCOVERY_UNAVAILABLE`.
- **Browsed clients are dialed idle** (`src/discovery_policy.h`, `.cpp`): `DialIntent::IDLE`
  holds the client without displacing another server; `connect()` / `connect_url()` dial for
  playback. A later activation declares `playback` only while the client's group plays.
- **Trust** (`server.h`, `types.h`): `approve()` / `revoke_approval()` for unpaired access, with
  `ApprovalScope::PLAYBACK` or `PLAYBACK_AND_SOURCE`; `begin_pairing()`,
  `submit_pairing_token()`, `submit_pairing_code()`, `unpair()`. Listener calls for approval
  needed, pairing required, pairing pending, code needed, pairing result, credential mismatch.
- **Groups** (`server.h`; `src/server_core.cpp`): one default group that every client that may
  play joins; a client that may not is held alone with no roles. `create_group()` needs a first
  member that is connected or holds a place; `move_client()`; a non-default group goes once it has no member, no
  input and no client keeping its place. One input per group. A place is kept for 180 s after a
  drop or a `restart` goodbye only.
- **`switch`** (`types.h`; `src/server_core.cpp`, `switch_group()`): always offered to
  controllers and carried out by the server, so a speaker can change groups by itself.
- **Dials** (`server.h`, `types.h` `ConnectOptions`): a browsed client is redialed while it
  advertises; a `connect_url()` target only with `retry_until_connected`.
- **Live input** (`types.h`, `InputKind::LIVE`): `write()` is real-time safe and never blocks;
  the writer drops what it refuses. PCM in as S16, S24 or F32, up to 384 kHz, 1–32 channels.
  `time_of()` and `latency_us()` say when a sample plays, on `now_us()`'s clock. A raised latency
  comes down at 300 ppm. A source that can go silent should `stop()` and start a new run.
- **The group's lead** (`src/sched/leads.h`): the largest of the members' leads
  (`min_buffer_ms`, `output_delay_ms`) plus pipeline latency and a margin.
- **Out to players** (`types.h`, `ErrorKind::NO_PLAYABLE_FORMAT`; `src/playback_policy.h`): PCM
  16/24/32-bit and FLAC 16/24-bit, resampled and channel-mixed per player; Opus only with
  `ServerConfig::enable_opus`. Format choice: the operator's, else the player's preferred, else
  the first producible entry at the input's rate.
- **Volume** (`server.h`): `set_group_volume()` and `set_group_mute()` for any group. Nothing
  per player, and neither `ClientInfo` nor `GroupInfo` carries a volume. v1 needs only the
  group's. The spec's group-volume algorithm is in `src/controller.h`.
- **Sources** (`server.h`, `route_source()`; `src/server.cpp`): accepts PCM and FLAC from a
  source, Opus when enabled.
- **Clock** (`src/clock.h`): `CLOCK_MONOTONIC_RAW` on Linux.
- **Liveness** (`config.h`): a write-stall limit (15 s) and a dial backoff (1–60 s). No ping or
  liveness timeout in the public headers or `docs/internals.md`.
- **Build** (`README.md`): C++20, CMake 3.25, OpenSSL 3, avahi-client. FetchContent for
  Boost.Beast/Asio (a system Boost ≥ 1.90 is used when installed), ArduinoJson, noise-c,
  micro-flac, micro-opus, pffft. Its tests fetch sendspin-cpp and build both in one tree.
- **Example host:** `examples/server_cli`.

### sendspin-cpp

[Sendspin/sendspin-cpp](https://github.com/Sendspin/sendspin-cpp) `main` at `f49a5bf`; latest
tag v0.10.0. Apache-2.0. Builds for ESP-IDF and host.

- **Everything the old draft waited for is on `main`**: mandatory Noise, Pairing PSK and
  pairing-code pairing, and the source role (`README.md`); unpaired access
  (`include/sendspin/client.h`).
- **Roles** are added before `start()` (`include/sendspin/client.h`): player, controller,
  metadata, artwork, source, color, visualizer.
- **One admitted connection** (`docs/internals.md`, "Slots": `MAX_ADMITTED`, one today), which
  owns every role. Arbitration between servers is in `src/admission.h`.
- **Inbound by default** (`include/sendspin/config.h`): a WebSocket server on port 8928;
  `connect_to()` for dialing out.
- **No mDNS.** The host advertises.
- **Pairing as a client** (`config.h`, `client.h`): `pairing_code_out_channels` and
  `pairing_code_formats` switch the dynamic code on; `on_pairing_code()` /
  `on_clear_pairing_code()` deliver and withdraw it; `client_pairing_token()` builds the
  `SP:0…` token. A static code is advertised only when no dynamic code is. 12 pairing records by
  default, least recently used evicted. No call lists the records or forgets one, and no callback
  reports an unpair; a `server/unpair` drops the record and closes with an `unpaired` goodbye
  (`src/connection_manager_pairing.cpp`).
- **Unpaired access** (`client.h`): `set_unpaired_access_enabled()`, off until called, never
  persisted by the library.
- **Source** (`source_role.h`, `config.h` `SourceRoleConfig`): PCM, FLAC or Opus; 5–150 ms
  chunks, 20 ms default; `write_audio()` from one capture thread with a capture time on the
  client's clock; `set_signal()` with `line_sense`.
- **Player** (`player_role.h`): `on_audio_output()` on a background thread;
  `notify_audio_played()` with the time the audio finishes playing; volume, mute and output
  delay both ways.
- **Clock** (`src/platform/time.h`): `CLOCK_MONOTONIC_RAW` on Linux.
- **Threads** (`client.h`): `loop()` from the main loop delivers callbacks and persistence
  writes; connection work runs on the library's own task.
- **Examples:** `examples/basic_client`, `examples/source_client`, and a file persistence
  provider in `examples/common/`.

### sendspin-cpp-cli

[Sendspin/sendspin-cpp-cli](https://github.com/Sendspin/sendspin-cpp-cli) `main` at `715f988`.
The reference Sendspin 1 player from 0.4.0 on (sendspin-mac `README.md`).

- `src/pipewire_sink.cpp` and `src/pipewire_source.cpp`: `pw_stream_new_simple` with
  `PW_STREAM_FLAG_RT_PROCESS`, timing from `pw_stream_get_time_n`. Also ALSA and PulseAudio sinks
  and sources.
- mDNS in `src/mdns_dnssd.cpp`.
- All inside the `sendspin-cli-core` static library, with the CLI and daemon.
- Apache-2.0 (`LICENSE`), so the two PipeWire files can be copied in with their notices.

Other SDKs (aiosendspin, sendspin-dotnet, sendspin-rs) are no longer relevant to this app.

## Music Assistant

Collected 2026-09-10. Applies to the client side only.

- **Line-in auto-play.** `music_assistant/providers/sendspin/player.py`,
  `_get_source_autostart_config_entries` (dev at `8e71b9d39`, 2026-08-26):
  - only offered when the source reports `line_sense`
  - defaults to the device itself when it is also a player
- **MA requires a paired source.** music-assistant.io `plugins/sendspin-source.md`.
- **OS media controls.** The web player's caused conflicts and got an off switch
  ([support#6181](https://github.com/music-assistant/support/issues/6181#issuecomment-5458519908)).
- **mDNS announcing.** Skip docker/VM/VPN interfaces
  ([server#5411](https://github.com/music-assistant/server/pull/5411#discussion_r3730412666))
  and temporary IPv6
  ([server#5293](https://github.com/music-assistant/server/pull/5293#issuecomment-5184435118)).
- **Tested with the Mac app:** Music Assistant 2.10.7, aiosendspin 10.2.1 (sendspin-mac
  `README.md`, "Status").
- Moot now: where MA's own local line-in lives
  ([server#5079](https://github.com/music-assistant/server/pull/5079)). The app no longer shares
  a source implementation with the local audio add-on.

## Linux desktop

Collected 2026-09-10.

**PipeWire outputs**
- A `pw_stream` with `media.class=Audio/Sink` is visible in the desktop UIs and dynamic.
  `object.linger` defaults to false, so the node dies with the app
  ([streams](https://docs.pipewire.org/page_streams.html),
  [props](https://docs.pipewire.org/page_man_pipewire-props_7.html)).
- pipewire-pulse flags sinks as virtual or network from `node.virtual` / `node.network`
  ([manager.c](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/master/src/modules/module-protocol-pulse/manager.c)).
  Plasma's applet hides virtual devices by default
  ([plasma-pa main.xml](https://invent.kde.org/plasma/plasma-pa/-/blob/master/applet/main.xml)).
- `pactl load-module module-null-sink` leaks the sink if the app crashes
  ([pulse-server.c](https://gitlab.freedesktop.org/pipewire/pipewire/-/blob/master/src/modules/module-protocol-pulse/pulse-server.c)).
- **One output per discovered device is unpopular.** Fedora 43's AirPlay discovery by default drew
  complaints about clutter and default-device hijacks
  ([Fedora discussion](https://discussion.fedoraproject.org/t/airplay-audio-outputs-discovery-enabled-by-default/166328)).
  Snapcast's module uses one sink per server
  ([docs](https://docs.pipewire.org/page_module_snapcast_discover.html)). One Sendspin output
  and a picker avoids both.
- **Self-capture avoidance** in [OBS pipewire-audio-capture](https://github.com/dimtpap/obs-pipewire-audio-capture)
  and [venmic](https://github.com/Vencord/venmic). Not needed while the only capture is our own
  sink's input.
- **Clock.** `pw_time.now` is `CLOCK_MONOTONIC`
  ([pw_time](https://docs.pipewire.org/structpw__time.html)); both Sendspin libraries count
  `CLOCK_MONOTONIC_RAW`.

**Not verified yet** (SPEC.md open questions 3–5)
- How a sink node reports a latency that changes while it runs, and which players delay video by
  it.
- Setting and restoring the default sink from an app (the `default` metadata's
  `default.configured.audio.sink`), and how WirePlumber treats that across restarts.
- Window placement for a tray popup under Wayland: `xdg_toplevel` has no position request.

**MPRIS** — for the player role only.
- GNOME shows players only while `CanPlay` is true
  ([mpris.js](https://gitlab.gnome.org/GNOME/gnome-shell/-/blob/main/js/ui/mpris.js)), with
  lock-screen media since GNOME 49
  ([unlockDialog.js](https://gitlab.gnome.org/GNOME/gnome-shell/-/blob/main/js/ui/unlockDialog.js)).
- WirePlumber pairs MPRIS players with streams by process
  ([mpris-pause.lua](https://github.com/PipeWire/wireplumber/blob/master/src/scripts/linking/mpris-pause.lua)).
- Bus-name, `CanControl`, artwork and Flatpak traps, measured:
  chrisuthe/sendspin-player `docs/ARCHITECTURE.md`, "Platform integration gotchas".

**Tray and background**
- GNOME has no tray; the AppIndicator extension is enabled on Ubuntu, not on Fedora
  ([extension](https://github.com/ubuntu/gnome-shell-extension-appindicator)).
- Background portal: autostart and `SetStatus`
  ([docs](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.Background.html)).

**Distro releases** (checked 2026-09-10)
- Release and end-of-life dates: [endoflife.date](https://endoflife.date/) for Fedora, Ubuntu
  and Debian.
- Package versions:
  - Fedora from [mdapi](https://mdapi.fedoraproject.org/). It lags: it reported PipeWire 1.6.2 for
    f44 while a Fedora 44 machine had 1.6.8 installed.
  - Ubuntu from the archive's madison (`ubuntu-archive-team.ubuntu.com/madison.cgi`).
  - Debian from the [sources.debian.org](https://sources.debian.org/) API. Debian 12 also has
    PipeWire 1.4.2 in bookworm-backports.
- Debian 12 uses PipeWire by default only on GNOME ([wiki](https://wiki.debian.org/PipeWire)).
- Not checked for the new dependencies: CMake ≥ 3.25, OpenSSL 3 and Boost ≥ 1.90 on each
  release. Without that Boost the server library fetches its own.

**Flatpak**
- Precedent for creating PipeWire sinks from a sandbox: EasyEffects
  ([manifest](https://github.com/flathub/com.github.wwmm.easyeffects)).
- WirePlumber client permissions for sandboxed apps
  ([client_access](https://github.com/PipeWire/wireplumber/blob/master/docs/rst/policies/client_access.rst)).
- The app now listens on two TCP ports and dials speakers: `--share=network`.
