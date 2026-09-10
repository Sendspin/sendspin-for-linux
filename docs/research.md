# Research notes

Collected 2026-09-10 for [SPEC.md](SPEC.md). Spec references are to
[Sendspin/spec](https://github.com/Sendspin/spec) `main` at `f4edee9`.

## Scope

- [backlog#42](https://github.com/Sendspin/backlog/issues/42) — desktop tray app / driver (parent).
- [backlog#46](https://github.com/Sendspin/backlog/issues/46) — rich client for Linux; GUI
  distros only, headless stays `sendspin-cli`.
- [backlog#43](https://github.com/Sendspin/backlog/issues/43) — one app for all OSes vs one per
  OS; reuse MA's desktop app. Open, no discussion yet.
- [backlog#56](https://github.com/Sendspin/backlog/issues/56) — extend `source@v1` with
  metadata, controls and artwork. Open.
- Siblings: [#44](https://github.com/Sendspin/backlog/issues/44) macOS,
  [#45](https://github.com/Sendspin/backlog/issues/45) Windows.

## Protocol

- **Roles:** `roles/*/v1.md`. `source@v1` merged in
  [spec#105](https://github.com/Sendspin/spec/pull/105), refined in
  [#225](https://github.com/Sendspin/spec/pull/225) and
  [#256](https://github.com/Sendspin/spec/pull/256).
- **The server picks the target.** spec#105 removed `select_source`; no wire field lets a
  source choose.
- **Source streaming** (`roles/source/v1.md`):
  - starts only on `server/command {source: start}`
  - chunks are binary type 12 with capture time in server clock
  - servers accept opus, flac and pcm
  - no `available: true` before convergence
  - a player+source device must not play its input locally
- **`signal` is a hint.** The server MAY use it to start or stop; the decision is server policy.
- **Encryption** (`connection.md`, `messaging.md`):
  - Noise `KKpsk2`, server is always the initiator
  - suites `25519_ChaChaPoly_SHA256` / `25519_AESGCM_SHA256`
  - `client_id` is the client's Curve25519 public key
- **Pairing** (`pairing.md`):
  - dynamic pairing code merged via [#155](https://github.com/Sendspin/spec/pull/155),
    [#189](https://github.com/Sendspin/spec/pull/189),
    [#194](https://github.com/Sendspin/spec/pull/194),
    [#237](https://github.com/Sendspin/spec/pull/237),
    [#241](https://github.com/Sendspin/spec/pull/241)
  - clients always offer Pairing PSK plus at most one code method
  - keep ≥5 pairing records
- **Unpaired access** ("guest mode" in MA and the backlog):
  - [spec#170](https://github.com/Sendspin/spec/pull/170),
    [#242](https://github.com/Sendspin/spec/pull/242)
  - `source@v1` needs explicit approval
- **v1.0 has no tag.** The "1.0.0" milestone still has
  [#198](https://github.com/Sendspin/spec/issues/198) and
  [#200](https://github.com/Sendspin/spec/issues/200) open, plus pairing fix PRs #245–#263.
- **Discovery** (`connection.md`):
  - client advertises `_sendspin._tcp` (port 8928, TXT `path`, `name`)
  - one admitted connection; priority `playback` > `pairing` > idle

## Music Assistant

- **Where local audio input belongs.** Marvin places it outside MA, in the existing local audio
  add-on
  ([server#5079](https://github.com/music-assistant/server/pull/5079#issuecomment-5436798710)).
  The add-on is `music-assistant/local-audio-addon`, which packages `sendspin-cli`.
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

## SDKs (as of 2026-09-10)

| SDK | Encryption | Source | Linux audio |
|---|---|---|---|
| aiosendspin (Python) | yes | yes | none |
| sendspin-dotnet | main only, 10.x unreleased | main only | none |
| sendspin-rs | PR #101 open | no | cpal |
| **sendspin-cpp** | `encryption-support` branch, no PR | PR stack #112–#117 (tracker #95) | none (callbacks) |
| **sendspin-cpp-cli** | no | no | ALSA, PulseAudio, PipeWire, PortAudio sinks |

- **sendspin-cpp** — static library, Apache-2.0, roles as CMake options, no mDNS. Builds for
  ESP-IDF and host.
- **sendspin-cpp-cli** — sinks and dns_sd mDNS live in the `sendspin-cli-core` static library
  alongside the CLI, daemon and control socket. `src/pipewire_sink.cpp` uses `pw_stream_new_simple`
  with `PW_STREAM_FLAG_RT_PROCESS` and `pw_stream_get_time_n`.

## Linux desktop

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
  ([docs](https://docs.pipewire.org/page_module_snapcast_discover.html)).
- **Self-capture avoidance** in [OBS pipewire-audio-capture](https://github.com/dimtpap/obs-pipewire-audio-capture)
  and [venmic](https://github.com/Vencord/venmic).

**MPRIS**
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

**Flatpak**
- Precedent for creating PipeWire sinks from a sandbox: EasyEffects
  ([manifest](https://github.com/flathub/com.github.wwmm.easyeffects)).
- WirePlumber client permissions for sandboxed apps
  ([client_access](https://github.com/PipeWire/wireplumber/blob/master/docs/rst/policies/client_access.rst)).
