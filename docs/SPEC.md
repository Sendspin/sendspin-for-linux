# Sendspin for Linux — spec

**Status:** draft, 2026-11-24. Tracks [Sendspin/backlog#46](https://github.com/Sendspin/backlog/issues/46),
the Linux child of [#42](https://github.com/Sendspin/backlog/issues/42). Modelled on
[Sendspin for Mac](https://github.com/Sendspin/sendspin-mac): #42's "AVP" (built-in server, cast
to speakers) is now the core of the MVP, and the player role comes on top of what the Mac does.
Sources for every claim are in [research.md](research.md).

Library references are to `main` as read on 2026-11-24:
[sendspin-server-cpp](https://github.com/Sendspin/sendspin-server-cpp) `f217a96` (**server**),
[sendspin-cpp](https://github.com/Sendspin/sendspin-cpp) `f49a5bf` (**client**) and
[sendspin-mac](https://github.com/Sendspin/sendspin-mac) `32444d6` (**mac**). A path such as
`server: include/sendspin/server/server.h` is a file in that repository.

## What it is

A background app for Linux desktops. It adds an output device named **Sendspin** to the OS sound
settings and a small picker to choose where that sound goes:

- **Cast** — to the Sendspin speakers the user ticks, with no other server on the network. The
  app is the Sendspin server.
- **Play along** — this PC's own output joins those speakers, in sync.
- **Music Assistant input** — MA uses the PC's sound as an input source and plays it on anything
  it controls.
- **Player** — MA plays to this PC's speakers, with the desktop's media controls.

It installs, autostarts and works with no configuration. Headless machines are not a target; they
use `sendspin-cli` (#46).

## How it fits together

```
apps ─► "Sendspin" output (PipeWire Audio/Sink node, created by the app)
           │ captured in the app, on the graph clock
           ▼
   Sendspin for Linux
     ├─ sendspin-server-cpp ── ws:// ──► Sendspin speakers (the ticked ones)
     │        └─ same audio, same timeline ──► this PC's output (play-along, played by the app)
     └─ sendspin-cpp, one client connection
              ├─ source@v1 ──► Music Assistant (the PC as an input)
              └─ player@v1 ◄── Music Assistant ──► this PC's output, MPRIS
```

| Part | What |
|---|---|
| Sendspin output | A PipeWire `Audio/Sink` node the app owns. What apps play to it is the app's only audio input. |
| Server | sendspin-server-cpp, embedded. Finds, pairs and plays to speakers. Own identity, port 8927. |
| Client | sendspin-cpp, embedded. One connection, to one server at a time (Music Assistant). Own identity, port 8928. |
| Local output | PipeWire playback streams to the hardware output: one for play-along, one for the player role. |
| UI | Qt 6: tray icon (StatusNotifierItem), the picker (QML), one Settings window, MPRIS over QtDBus. |

One process. The server and the client are two Sendspin devices with two key pairs that happen to
share it.

The sink's audio has one destination at a time: the ticked speakers, or Music Assistant. The two
are not in sync with each other (mac: `README.md`, "Already running Music Assistant?"). The
player role is independent of both.

## MVP scope

- The Sendspin output device, always present while the app runs.
- Embedded server: discovery both ways, unpaired playback to speakers the user ticks, pairing by
  token, static code and dynamic code, group volume and mute.
- This PC's output in the group, in sync.
- Client roles `source@v1`, `player@v1`, `controller@v1`, `metadata@v1`, `artwork@v1`, paired by
  dynamic pairing code, plus the Pairing PSK every client must offer.
- Sendspin 1 only: Noise on every connection. Both libraries make it mandatory.
- The picker, the tray icon and menu, one Settings window, MPRIS for what MA plays here.

### Not in the MVP

- The pre-1.0 unencrypted protocol.
- Opus, either way. Off by default in the server (`ServerConfig::enable_opus`) and
  patent-encumbered.
- Choosing sample rate or bit depth. The sink runs at one format; see [Formats](#formats).
- Metadata, artwork, transport commands, colors or visualizer data sent to speakers. The server
  library implements the roles; the app feeds them nothing yet.
- Routing a speaker's own source (line-in) into the group (`SendspinServer::route_source()`).
- A line-in or microphone as the app's input. The Sendspin output is the only input.
- Scanning a speaker's QR code with a camera. Tokens are pasted.
- Showing or controlling what MA does with the PC's input (mac: `TODO.md` item 5).
- More than one server row; see [Open questions](#open-questions).
- One OS output per speaker, and capturing what plays without choosing the Sendspin output.
- A now-playing window. What MA plays here shows in the desktop's own media controls.
- Systems without PipeWire, and a daemon/headless mode.

## UI

Four surfaces, and no others:

1. **The picker** — a small Qt Quick window: the Sendspin row, this PC's output, the speakers,
   the server. See [The picker](#the-picker).
2. **The tray icon and its menu** (StatusNotifierItem). Activating the icon opens the picker. The
   menu holds a status line, Settings… and Quit. No sliders, no speakers.
3. **One Settings window.**
4. **The desktop's media controls** (MPRIS) — only for what MA plays to this PC.

The picker also opens from the app launcher and on a second launch. On stock GNOME, which has no
tray, that is the only way in; its gear button opens Settings.

**Tray icon:** the Sendspin mark with its waves while the PC's sound goes out through Sendspin
(to a connected speaker, or to a server with its stream open, and there was sound in the last
3 s); the mark alone otherwise. Symbolic, in the panel's colour.

**Tray status line:** `Playing on Kitchen and 2 more`, `Sending to Music Assistant`,
`Title — Artist` (MA playing here), or `Not in use`.

**Settings window**, every setting live, no Save button:

- *This PC:* device name (MA's own custom player name still wins in MA); output device (default:
  follow the system default); open at login.
- *Speakers:* the paired speakers, each with Forget; the speakers on the network that aren't
  paired, each with Pair…; a field to paste a pairing token.
- *Music Assistant:* which server is connected, or none; the pairing code while pairing; the
  servers this PC paired with, from the app's own record of `on_paired()`; Copy pairing token;
  allow playback without pairing (on by default); media controls
  on/off.

One instance per user session.

## The picker

Adapted from mac `docs/menu-states.md`. Where that file says "the Mac", read "this PC"; what
differs on Linux is called out. Keep this section in step with the picker.

### Rules

1. **What you chose stays; what you didn't, leaves.** A ticked speaker or a paired server that
   goes away stays in the list as **Unavailable** and comes back by itself. An unticked speaker,
   or a server that was never paired, leaves the list.
2. **One word for gone: Unavailable.** For a speaker and for a server.
3. **Playing shows on the tray icon and the dot, not in the rows.** Rows don't say Playing or
   Idle.
4. **A row only says why it can't play.** A status on the right, or a caption saying what to set
   up. There is no "Ready".
5. **No automatic fallback.** When every ticked speaker is gone, the system output stays on
   Sendspin. The output only changes when the user adds, removes or picks something.
6. **Show only what exists.** A destination appears once it's found or chosen. Until anything is
   found, the picker says what it's looking for.
7. **Choosing is all there is.** No on/off switch: where the PC plays follows from what's ticked.
8. **One indicator per row.** Where an output is groupable, the check on the right is the group.
   Where it isn't, there is no check and the icon is highlighted while it plays.
9. **Every output is groupable or not.** Every Sendspin speaker and this PC's output are
   groupable. A server (Music Assistant) isn't: while it plays, nothing can join it.
10. **Something always plays after a choice.** When nothing left in the group could play, this
    PC's own output takes over.

### Rows

From top to bottom.

| Row | Shown when |
|---|---|
| Sendspin, and the dot | Always. The dot is blue while Sendspin plays (when the tray icon has its waves), grey while the system output is Sendspin but nothing goes out (silence, or every ticked speaker unavailable), an empty ring while the PC plays directly. Tooltip and accessible name say which. With a speaker ticked the row carries the picker's one volume slider, the group's volume; see [Volume](#volume). |
| Notice | PipeWire can't be reached, or mDNS is unavailable (`ErrorKind::DISCOVERY_UNAVAILABLE`: avahi-daemon isn't running). One at a time, in that order. Without PipeWire there are no rows below. |
| This PC's output | Always. First. Named as the desktop names the device. |
| The Sendspin speakers | Each speaker found or ticked. |
| Looking for speakers… | No speaker is found or ticked, and no server is connected or paired. Links to [which speakers work](https://www.sendspin-audio.com/#speakers). |
| Divider, then the server | A server is connected to the client, or paired with it. Named after the server (`SendspinClient::server_information()`). |

Each speaker row: icon, name, then a status if it can't play, then a round check — filled while
it plays, grey while it's ticked but can't play, a spinner while it connects, an empty ring when
it isn't ticked. No row has a slider of its own in v1.

### Sendspin or direct follows what's ticked

| Ticked | System output | Dot | Delay |
|---|---|---|---|
| Only this PC's output | The hardware device | Empty | None |
| Any Sendspin speaker, with or without this PC's output | Sendspin | Blue, or grey while nothing goes out | The speaker buffer; see [Timing](#timing) |
| A server | Sendspin | Blue while its stream is open | Whatever the server adds |

- Ticking the first speaker, or choosing a server, makes Sendspin the system's default output and
  remembers the one it replaced. Going back to this PC's output alone restores it.
- **Linux differs:** an app can be routed to the Sendspin output by hand while the default is
  something else. That audio still goes to whatever is ticked. With only this PC's output ticked
  it is played there directly, without the speaker buffer.
- The app never makes Sendspin the default on its own, and gives the node a low
  `priority.session` so WirePlumber doesn't either.

### A speaker's status

| Shows | Means | From the server library | Until |
|---|---|---|---|
| A filled check, no status | It plays the PC's sound. | Connected, in the cast group, lists `player@v1`. | – |
| Connecting… / spinner | A dial is under way. | `ClientInfo::connected` false after `connect()`, or `reconnecting`. | It's connected. |
| Unavailable | It can't be reached, or it shut down. | `on_client_disconnected()` with `CONNECTION_LOST`, or a goodbye with `SHUTDOWN` or `RESTART`; or no longer in `clients()`. | It's back. A browsed speaker is redialed by the library while it advertises. |
| In use elsewhere | Another server has it. Ticked: it was taken over. Unticked: it refused our idle connection. | A goodbye with `GoodbyeReason::ANOTHER_SERVER`, or `on_error()` with `GOODBYE_BEFORE_ACTIVATION` and `ANOTHER_SERVER` or `CONCURRENT_ATTEMPT`. | The user clicks its name or ticks it: `connect()`, which dials for playback. The library does not take it back by itself. |
| Disconnected | Its user disconnected it from this PC. | A goodbye with `USER_REQUEST`. | The user unticks it and ticks it again. |
| Can't connect | It answers, but the connection fails. | `on_error()` with `DIAL_FAILED` or `HANDSHAKE_FAILED`, `DisconnectReason::CLOSED_BY_SERVER`, or a goodbye with `UNAUTHORIZED`. | It works. The library retries a browsed speaker with backoff. |
| Can't play | It lists no format the server can produce. | `on_error()` with `NO_PLAYABLE_FORMAT`. | – |
| Needs pairing | It only plays for paired servers. | `on_pairing_required()`, or a goodbye with `PAIRING_REQUIRED`. | Ticking it starts pairing. |
| Needs pairing again | It lost its pairing with this PC. | `on_credential_mismatch()`. | It's paired again. |
| Identity changed | A different device presents itself under a ticked speaker's name. | App logic, not the library's: a `client_id` the app hasn't seen, under a ticked speaker's name or URL. It arrives as any new speaker does (`on_approval_needed()` or `on_pairing_required()`). | The user clicks it, accepting the new identity. |
| nothing | Not ticked; also a speaker that connected to this PC and waits. | In the lobby, or held alone (`on_approval_needed()`). | The user ticks it. |

The events are in `server: include/sendspin/server/server.h` (`ServerListener`) and `types.h`
(`ClientInfo`, `DisconnectReason`, `GoodbyeReason`, `ErrorKind`).

- A goodbye with `UNPAIRED` is the answer to our own Forget, not a status: the speaker is a new
  speaker again.
- `connect()` only reaches a speaker the server can dial. One that connected to us and left is
  gone from `clients()`; it shows again when it connects again.

A speaker keeps the name from its hello while it's away; mDNS often has only an ID.

### What ticking does

The server library has one group that every client that may play (paired or approved) joins by
itself (`SendspinServer::default_group()`). A client that may not play is held alone, with no
roles. The app uses the default group as a **lobby** that never gets an input, and casts into a
group of its own, so the app never plays on a speaker the user didn't tick.

| Action | App does |
|---|---|
| Tick a speaker with unpaired access | `approve(client_id, ApprovalScope::PLAYBACK)`, then move it into the cast group (`create_group()` for the first, `move_client()` after). Never `PLAYBACK_AND_SOURCE`. |
| Tick a paired speaker | Move it into the cast group. |
| Tick a speaker that needs pairing | Pair it (below); on `PairingResult::PAIRED`, move it in. |
| Tick a speaker that isn't connected | `connect()`; move it in on `on_client_connected()`. |
| Untick a paired speaker | Move it to the lobby. |
| Untick a speaker that was only approved | `revoke_approval()`: its stream ends and it is held alone again. |
| Tick this PC's output | Start play-along. Alone, switch the system output back to the hardware device. |
| Untick the last thing that could play | This PC's output is ticked instead (rule 10). |
| Choose the server | Untick the speakers, release the cast input, feed the source role. |
| Tick a speaker while the server is chosen | The server is no longer chosen; the source gets silence. |

- The ticked set is the app's own setting, kept by `client_id`. The library's approval and
  pairing records say who *may* play, not who is ticked.
- The library keeps a speaker's place in the cast group only after a drop or a `restart`
  goodbye, and only for `ServerTiming::group_rejoin_grace_us` (180 s). In every other case a
  ticked speaker comes back into the lobby, so the app moves each ticked speaker into the cast
  group on every `on_client_connected()`. The same at startup, for the stored set.
- `create_group()` and `move_client()` do nothing for a speaker that isn't connected and holds
  no place, which is why a tick may have to wait for the connection.
- **A speaker can move itself.** The server always offers a controller the `switch` command and
  carries it out (`types.h`, `TransportCommand`; `server: src/server_core.cpp`,
  `switch_group()`). A speaker whose own button switches it into or out of the cast group is
  ticked or unticked to match (`on_group_changed()`): the person at the speaker chose.
- `ServerConfig::approve_all_clients` stays off.

### This PC's output

The output the system uses, or used last before Sendspin; when that's gone, the next hardware
sink. Named and drawn as the desktop does.

- With a speaker ticked it plays along in sync: the app plays the group's audio on it itself, on
  the server's timeline. It is not a Sendspin player and makes no connection.
- Alone, the system plays on it directly.
- Playing along, it follows the group's volume and mute like the speakers. Alone, the desktop's
  own volume control is all there is.
- Our own Sendspin output and the monitor of any sink are never offered here.

### The server row

The client is a source and a player for one server at a time, usually Music Assistant. The row is
the same for any server. It is Music Assistant when a `_mass._tcp` record on the network carries
the server's name; then it gets MA's logo and links into MA's pages. Not groupable: no check, the
icon is highlighted while its stream is open.

| When | Caption |
|---|---|
| Not paired yet | Pair this PC in **[server]**, then enter the code that appears here. Clicking the row opens the same page, for MA. |
| A code is showing | Enter 123-456 in **[server]**. |
| Chosen, not the input yet | Select [PC name] as an input source in **[server]** to start playing. |
| Chosen, the PC is its input | Choose speakers, groups and volume in [server]. |
| Paired, not connected | Unavailable. It reconnects by itself when it's back on the network. |

The row says nothing about MA playing *to* this PC. That needs no choice here and shows in MPRIS
and the tray status.

### When something goes away, and comes back

| What happens | The picker shows | When it's back |
|---|---|---|
| A ticked speaker loses power or the network | Unavailable. | It reconnects and plays by itself. |
| A ticked speaker shuts down properly | Unavailable while it's gone. | It reconnects once it announces itself again, and the app moves it back into the group. |
| An unticked speaker loses power or the network | It leaves the list once mDNS drops it. | It appears when it announces itself. |
| An unticked speaker shuts down properly | It leaves the list at once. | It appears when it announces itself. |
| A paired server goes offline | Unavailable, chosen or not. | The server connects again by itself. |
| A server that was never paired goes away | Its row leaves. | It appears again, ready to pair. |
| The server unpairs this PC | Unavailable. The client library drops its record and closes the connection without telling the app; see [Depends on](#depends-on). | The server pairs again, with the code. |

How fast each of these shows is the libraries' doing, not the app's, and the Mac's figures don't
carry over. The server library has a write-stall limit (15 s) and a dial backoff (1–60 s)
(`server: include/sendspin/server/config.h`); its public headers name no ping. The client drops
a silent connection after 30 s (`SendspinClientConfig::liveness_timeout_ms`). Whether a speaker
that loses power shows Unavailable fast enough is open question 7.

### Where this differs from the Mac's menu

Beyond the rows above (no driver or Local Network notice, no AirPlay, no QR scanning):

- **Needs pairing.** `menu-states.md` still says the app can't pair speakers; the Mac's README
  and this app pair on tick.
- **One volume slider, the group's.** The Mac has one per speaker. See [Volume](#volume).
- **The Sendspin output's own volume scales the audio**, like any output's. On the Mac it drives
  the speakers instead.
- **The speaker buffer doesn't grow for this PC's output** — open question 2.
- **The pairing token** is offered in Settings (Copy pairing token). The Mac shows it nowhere.
- **A speaker can tick itself** with its `switch` button; see above.
- **No format on hover.** The library doesn't report it yet.
- Switching between Sendspin and direct costs a moment of silence here too.

## Connection

### As a server, to speakers

`server: include/sendspin/server/config.h`, `server.h`.

- Both directions (`ServerConfig::browse`, `advertise`): the library browses `_sendspin._tcp` and
  dials what it finds, and advertises `_sendspin-server._tcp` on port 8927 (`listen_port`) with
  path `/sendspin` for speakers that look for a server. If the port is taken
  (`StartResult::BIND_FAILED`), the app starts again with port 0, which lets the system pick.
- A browsed speaker is dialed idle: it is held without taking it from another server
  (`server: src/discovery_policy.h`, `DialIntent::IDLE`). It only declares playback once its
  group plays, so an unticked speaker is never disturbed.
- A speaker that connects to us waits, unticked, until the user ticks it.
- mDNS is avahi-client, built into the library (`platform_discovery()`), and needs avahi-daemon
  at run time. Without it the server still runs; the picker shows the notice.
- Speakers mDNS can't find: a hidden setting with a list of `ws://host:port/path` URLs, dialed
  with `connect_url()` and `ConnectOptions::retry_until_connected`. Without it a failed dial is
  not retried.
- Identity: the library creates the server key on first start and stores it through the app's
  `PersistenceProvider`, with the client records (approvals, long-term PSKs).

### As a client, to Music Assistant

`client: include/sendspin/config.h`, `client.h`.

- The library listens on port 8928 (`SendspinClientConfig::server_port`) and servers connect in.
  The app never calls `connect_to()`.
- **The client library has no mDNS.** The app advertises `_sendspin._tcp.local.` itself through
  Avahi: the port, TXT `path=/sendspin`, `name=<device name>`. It skips docker, VM and VPN
  interfaces and temporary IPv6 addresses.
- Identity: a Curve25519 key pair the library generates on first start; `client_id` is its
  public key. Losing it makes the PC a new device to every server.
- Unpaired access: the app stores the setting and calls `set_unpaired_access_enabled()` before
  `start()`; the library never persists it.

### One client connection, two roles

The client library admits one connection at a time (`client: docs/internals.md`, "Slots":
`MAX_ADMITTED` is one today), and that connection owns every role. So:

- Source and player always belong to the same server. The server that plays to this PC is the
  one that may use it as an input.
- A second server displaces the first by the spec's order, `playback` over `pairing` over idle
  (`client: src/admission.h`). The displaced one gets a goodbye.
- Both streams need the server to declare `playback`; without it the player discards audio and
  the source ignores a start (`client: docs/integration-guide.md`).
- `set_available()` and `leave()` act on the whole client, both roles. The app uses neither in
  the MVP.
- A device that is a player and a source plays only what its group streams it, its own input
  included (`server: server.h`, `route_source()`). When MA plays the PC's input on a group the PC
  is in, the PC hears it through MA, delayed like every other member.

Our own server must never be the one that connects: see [Loop guards](#loop-guards).

### Storage

Two persistence providers, one per library, each a directory of blobs with mode 0600 under
`$XDG_DATA_HOME/<app-id>/`: `server/` and `client/`. No Secret Service: it isn't there on every
desktop, and both libraries' examples use files. Never log keys, PSKs or tokens.

## Pairing

### Speakers (we are the server)

`SendspinServer::begin_pairing()`, `submit_pairing_token()`, `submit_pairing_code()`.

- All three methods, whichever the speaker offers (`ClientInfo::pairing_offers`), preferring the
  dynamic code, then the static code, then the token.
- Dynamic code: the speaker shows or speaks six digits; the user types them. Asked again after a
  wrong one (`on_pairing_code_needed()`).
- Static code: eight digits from the device or its leaflet.
- Token: `SP:0…`, pasted in Settings or in the pairing dialog.
- `on_pairing_pending()`: the speaker waits for its own button; show its message as plain text.
- `ServerConfig::operator_can_scan_qr` stays off, so speakers are asked for digits.
- Ticking a speaker that needs pairing pairs it. Pair… in Settings pairs without ticking.
- Forget in Settings: `unpair()`, and untick. It's a new speaker again.

### Music Assistant (we are the client)

- Offer `dynamic_pairing_code` on `DISPLAY` as `DIGITS`, and `pairing_psk`
  (`SendspinClientConfig::pairing_code_out_channels`, `pairing_code_formats`). Never a static
  code: a client offers one code method.
- No `QR_CODE` format: the server picks one format, and the person pairing usually sits at this
  PC typing into MA's page, where a QR code can't be entered.
- When a server starts pairing, `on_pairing_code()` delivers the code. Show it as `123-456` in
  the server row's caption, in Settings, and in a notification; copy it to the clipboard. Take it
  down on `on_clear_pairing_code()`.
- Result: "Paired with [server]" on `on_paired()`, "Pairing ended" on `on_pairing_aborted()`.
- The pairing token (`client_pairing_token()`) is only behind Copy pairing token in Settings.
- The source role needs pairing: MA only accepts paired sources. Unpaired access covers the
  player role alone.
- The library keeps up to 12 pairing records (`DEFAULT_MAX_PAIRING_RECORDS`; the spec floor is 5)
  and evicts the least recently used.

## The Sendspin output

- A PipeWire node with `media.class=Audio/Sink`, created at startup, gone when the app exits or
  crashes.
- `node.description=Sendspin`, `node.network=true`, `node.virtual` unset — Plasma's applet hides
  virtual sinks by default.
- Low `priority.session`.
- Its volume and mute work like any output's: PipeWire scales what the app captures. They do not
  drive the speakers; that is the picker's slider. See [Volume](#volume).
- It reports the speaker buffer as its latency; see [Timing](#timing).

## Casting to speakers

- One live input into the cast group: `open_input()` with `InputKind::LIVE` in the sink's format
  (`server: include/sendspin/server/types.h`). `GroupInput::write()` is real-time safe and is
  called from the PipeWire process callback. What it refuses is dropped, never resent.
- **Idle:** after 3 s of digital silence, `GroupInput::stop()`; the speakers' streams end. The
  next sound starts a new run. The library asks for this of a live source that can go quiet.
- The input is held only while a speaker is ticked. The cast group goes away with its last member
  and its input, and is created again with the next tick.
- No transport commands are offered to speakers with a controller (`set_controller_caps()` is
  never called).

### Volume

Two controls, independent of each other:

| Control | Sets | Where |
|---|---|---|
| The picker's slider and mute | The volume of the group being played to | The speakers, each on its own hardware |
| The Sendspin output's volume and mute (OS mixer, volume keys) | The level of the audio the app captures | PipeWire, before the app sees it |

**The group's volume**

- `set_group_volume()` and `set_group_mute()` on the cast group
  (`server: include/sendspin/server/server.h`).
- The library implements the spec's group-volume algorithm
  ([spec `README.md`, "Setting group volume"](https://github.com/Sendspin/spec/blob/main/README.md),
  line 1674 at `2cfe197`; `server: src/controller.h`): the group's volume is the average of its
  players'; a change shifts every player by the same amount, clamps at 0 and 100 and hands what
  was clamped to the others; each player gets one command. Players without volume support are
  left out. Relative levels are kept.
- Mute goes to each player that supports it. A volume change never clears a mute.
- One slider and one mute, on the picker's Sendspin row, shown while a speaker is ticked.
- No per-speaker sliders. A speaker's level relative to the others is set on the speaker.
- **Volume is perceived loudness, 0–100, not amplitude** (spec `README.md`, "Player messages",
  line 1333): 50 should sound half as loud as 100. Wherever the app turns a Sendspin volume into
  a gain itself, it uses `amplitude = (volume / 100)^1.5` and ramps the change:
  - this PC's output playing along, which takes the group's volume and mute;
  - the player role's stream, below.

**The Sendspin output's volume**

- PipeWire applies it to the captured audio, so it reaches everything downstream alike: the
  speakers, this PC's output playing along, and Music Assistant.
- Muting it sends silence. After 3 s that is idle, as any silence is.
- Below 100% the audio is no longer the app's samples. For lossless playback leave it at 100%
  and use the picker's slider.
- With a server chosen it is the only volume the app has; the server sets its speakers' own.
- A speaker with a controller can change the group's volume and mute too; the server carries
  that out itself (`types.h`, `TransportCommand`). The library reports no group volume back to
  the host, so the slider shows the last value set from this PC.
- The value is kept across restarts and applied when the cast group is created.

## Playing along on this PC

- A PipeWire playback stream to the hardware output, fed the same captured audio.
- Each frame is scheduled for when the group plays it: `GroupInput::time_of(sample_pos)`, minus
  the delay PipeWire reports for the stream. There is no clock to sync: the server's clock is
  this machine's.
- The device's clock drifts from that timeline. Follow it by dropping or repeating single frames
  inside a small dead band, with a resync at the start, after an underrun and past 1 ms (mac:
  `README.md`, "The Mac's own output").
- When its own latency is longer than the group's, it cannot be on time. See
  [Open questions](#open-questions).

## Sending to Music Assistant

- `add_source()` with FLAC, 48 kHz, 16-bit stereo, 20 ms chunks, `line_sense` on
  (`client: include/sendspin/config.h`, `SourceRoleConfig`). The server library accepts PCM and
  FLAC from a source (`server: src/server.cpp`); FLAC is lossless and smaller, and what the Mac
  sends.
- Stream only between `on_streaming_started()` and `on_streaming_stopped()`: the server decides
  when (`client: include/sendspin/source_role.h`).
- `write_audio()` from the capture thread, each write stamped with the capture time of its first
  sample on the client's clock. The library maps it into server time and drops a backlog rather
  than bursting it.
- `set_signal()`: present while there was sound in the last 3 s, absent otherwise, so MA can
  start the input by itself.
- With speakers ticked instead, the source gets silence and reports no signal. The stream stays
  open, like a quiet line-in, and resumes when the server is chosen again.
- Expect a noticeable delay. Fine for music; video will not be in sync.

## Playing on this PC

- `add_player()`, `add_controller()`, `add_metadata()`, `add_artwork()`.
- `on_audio_output()` writes to a PipeWire playback stream named "Sendspin" (`media.role=Music`)
  that follows the default output, or the device chosen in Settings.
  `notify_audio_played()` reports each buffer with the time it finishes playing
  (`client: include/sendspin/player_role.h`).
- Advertise only formats that play without a resampler: the sink's `EnumFormat` intersected with
  the daemon's `clock.allowed-rates`, as FLAC and PCM.
- The player's volume and mute are that stream's volume in the OS mixer, both ways
  (`on_volume_changed()`, `update_volume()`), converted with the spec's curve; see
  [Volume](#volume).
- Transport commands go through the controller role and act on the group. Offer only what
  `supported_commands` lists.

### MPRIS

- Bus name `org.mpris.MediaPlayer2.<app-id>`, owned by this process (WirePlumber pairs MPRIS
  players with streams by process).
- Registered while a server is connected; `CanPlay=false` while the group is idle, so GNOME hides
  the card; unregistered on disconnect.
- Metadata from `on_track_metadata()`. Artwork written as a `file://` path named by a hash of the
  image bytes, under `$XDG_RUNTIME_DIR/app/<app-id>/`.
- `Volume` is the group volume from the controller state.
- Can be switched off in Settings.
- MPRIS never describes what the PC casts: the app playing into the Sendspin output keeps its own
  card and controls.

## Timing

- **The speaker buffer is the library's.** The server computes the group's lead from what each
  member reports (`min_buffer_ms`, `output_delay_ms`), its pipeline and the input's write jitter
  (`server: src/sched/leads.h`). The app does not compute it; it reads
  `GroupInput::latency_us()`.
- It rises as soon as a joiner needs more. It comes back down by itself, inaudibly: the library
  plays 300 ppm fast until the excess is gone (`types.h`, `InputKind::LIVE`).
- **Sink latency.** The Sendspin output reports `latency_us()` plus its own capture delay as the
  node's latency, and updates it when it changes, so players that honour output latency keep lip
  sync.
- **Write jitter.** Declare what the PipeWire quantum needs (`InputOptions::write_jitter_us`,
  default 20 ms). A write later than that is heard as a gap.
- **Clocks.** Both libraries count `CLOCK_MONOTONIC_RAW` on Linux (`server: src/clock.h`,
  `client: src/platform/time.h`). PipeWire stamps in `CLOCK_MONOTONIC`. Convert every PipeWire
  time before handing it to `write_audio()` or comparing it with `time_of()`; the two clocks
  differ in rate as well as offset.
- **Source.** The capture time is the graph time of the buffer's first sample, from
  `pw_stream_get_time_n()`, converted as above.
- **Player.** The library aligns decoded audio to server time; the app's part is an honest
  `notify_audio_played()` timestamp, which includes the delay PipeWire reports.

## Formats

- The Sendspin output is 32-bit float stereo at 48 kHz. PipeWire converts what apps play.
- To speakers: the input is `SampleFormat::F32`. The library gives each speaker PCM or FLAC in a
  format it lists, resampled and channel-mixed per speaker: the speaker's preferred format when
  the server can produce it, else its first producible one at the input's rate
  (`server: src/playback_policy.h`). A speaker isn't held back by another in the group.
- To Music Assistant: FLAC 48 kHz 16-bit, converted from the float capture with TPDF dither.
- From Music Assistant: whatever the output plays natively, as above.
- Other rates, and following what the playing app outputs, come later.

## Loop guards

1. **The player stream never plays into our own Sendspin output.** If Sendspin is the system
   default, the player stream and the play-along stream both target the remembered hardware
   output. Otherwise MA's audio would come back in as the PC's input.
2. **Never capture our own output.** The only capture is the Sendspin sink's own input. No
   monitor of the default sink, no monitor of the hardware output.
3. **Our server never connects to our client.** The server browses `_sendspin._tcp` and would
   find the client's own advertisement. The app wraps the discovery provider
   (`ServerConfig::discovery`) and drops its own instance, and never approves, pairs or lists
   its own `client_id`. Our server taking the one client connection would also take it from MA.
4. **Captured audio is never played directly while a speaker is ticked or a server is chosen.**
   This PC hears it only as a group member: through play-along, or through MA.
5. **Speakers never get source approval.** `ApprovalScope::PLAYBACK` only.
6. MA's auto-play default can still send a desktop's audio back to itself, delayed — open
   question 1.

## Platform

- **C++20**, on sendspin-server-cpp and sendspin-cpp, both linked statically.
- **Qt 6** (floor 6.8): Qt Quick for the picker, QtDBus for the tray and MPRIS.
- **PipeWire required** (floor 1.4.2). No PulseAudio or ALSA fallback:
  - Every supported release uses PipeWire by default.
  - The Sendspin output depends on it: a node that disappears with the app, can be marked as a
    network device, and stamps each buffer from the graph clock.
  - A PulseAudio version would leak its sink on a crash, need a second timing path, and rely on
    module loading, which is unsafe from a Flatpak.
- **Avahi** for mDNS, both for the server library and the client's advertisement.
- **App ID** `com.sendspin_audio.Sendspin`: the reverse of sendspin-audio.com, with the hyphen as
  an underscore, as D-Bus names and Flatpak IDs require. The Mac's bundle ID is
  `com.sendspin-audio.mac`.

### Install methods

Listed in this order in the README and on the website:

1. **Distro packages** — `.deb` for Debian and Ubuntu, `.rpm` for Fedora, attached to each GitHub
   release, with package repositories later. Arch through the AUR. Same problem as
   [backlog#52](https://github.com/Sendspin/backlog/issues/52) for `sendspin-cli`; solve it once.
   - Built against the distro's Qt 6, libpipewire, avahi-client and OpenSSL 3. Everything else
     comes from the source tree; see [Building offline](#building-offline).
   - Installs an XDG autostart entry.
2. **Flatpak** (Flathub), on the KDE runtime.
   - `--share=network`
   - `--filesystem=xdg-run/pipewire-0` (not `--socket=pulseaudio`)
   - `--talk-name=org.kde.StatusNotifierWatcher`
   - `--system-talk-name=org.freedesktop.Avahi`
   - MPRIS name is auto-granted — do not declare it
   - Background portal for autostart. Only this build gets GNOME's Background Apps entry.

### Building offline

Distro and Flathub builds have no network, and both libraries fetch their dependencies with
CMake FetchContent. So the release source tree carries them:

- sendspin-server-cpp and sendspin-cpp at their pinned commits.
- What they fetch: noise-c, ArduinoJson, micro-flac, micro-opus, pffft, IXWebSocket, and
  Boost.Beast and Boost.Asio (`server: cmake/dependencies.cmake`, `cmake/noise_c.cmake`;
  `client: cmake/host.cmake`).
  The server uses a system Boost 1.90 or later when there is one.
- The build runs with FetchContent disconnected and pointed at the vendored copies. A build that
  reaches for the network is a bug.
- Each release lists the vendored sources, their versions and licenses, as the server's
  `NOTICES.md` does.
- Qt, PipeWire, Avahi and OpenSSL always come from the distro or the Flatpak runtime.

### Supported releases

As of 2026-09-10:

| Distro | Releases | Qt 6 | PipeWire |
|---|---|---|---|
| Fedora | current + one back: 44, 43 | 6.11 / 6.10 | 1.6 / 1.4 |
| Ubuntu | 26.04 LTS and later | 6.10 | 1.6 |
| Debian | 13 and later | 6.8 | 1.4 |
| Arch | rolling | current | current |

- **Floor: Qt 6.8 and PipeWire 1.4.2**, both set by Debian 13.
- Ubuntu 24.04 and Debian 12 are left out on purpose: both ship Qt 6.4.

## Depends on

- **sendspin-server-cpp** — early development, no release tags, API changes without deprecation:
  pin a commit. Needs C++20, CMake 3.25, OpenSSL 3, avahi-client. Missing for this spec:
  - **A floor on a live input's lead**, as `SourceRouteOptions::min_latency_us` gives a routed
    source, for when this PC's output is slower than every speaker.
  - **The format a speaker receives**, to show on hover as the Mac does. Nice to have.
- **sendspin-cpp** — v0.10.0 is the latest tag; pin a commit until the next one. Encryption,
  pairing, unpaired access and
  `source@v1` are all on `main`. Missing:
  - **Its pairing records, to the host.** No call lists them, none forgets one, and no callback
    reports a `server/unpair` or an eviction. The app keeps its own list from `on_paired()`,
    which can go stale, and Settings cannot offer Forget for a server.
  - mDNS, by design. The app advertises.
- **Both** fetch their dependencies with CMake FetchContent. The server's tests build the two
  libraries in one tree, so linking both works. The sources are vendored; see
  [Building offline](#building-offline).
- **sendspin-cpp-cli** — `src/pipewire_sink.cpp` and `src/pipewire_source.cpp` (with their
  headers) are copied into this repository for round 1, Apache-2.0, with a note of the commit
  they came from. The sink carries the two playback streams; the source is the starting point
  for the Sendspin output's capture side. Extracting a library both projects use comes later.
- **Music Assistant** — the line-in auto-play default, question 1.

## Open questions

Answered since the last draft, by the Mac or by the libraries: encryption in sendspin-cpp
(merged); key storage (files, 0600); source codec (FLAC); inputs (the Sendspin output only); the
app ID; where source audio plays (the app's own server lets the user choose; with MA, MA
chooses); per-speaker volume (not in v1: the group's volume only, which the server library
already sets); the sink's volume (PipeWire scales the captured audio, and it does not drive the
speakers); reuse from sendspin-cpp-cli (copy its PipeWire sink and source now, a shared library
later); offline builds (vendor the FetchContent dependencies).

Still open:

1. **MA's line-in auto-play default.** For a client that is both player and source, MA
   pre-selects "This device". On a desktop that plays your own audio back out of your own
   speakers, delayed, until the user changes it. Ask MA to default it to Off or to ask during
   pairing; or point the user at the setting after pairing. Not re-checked since 2026-08-26.
2. **This PC's output slower than the group.** A Bluetooth output can need more than the speaker
   buffer. Raise the lead (needs the library change above), or show the row as unable to play
   along?
3. **Who honours the sink's latency.** Check mpv, Firefox, Chromium and GStreamer players through
   pipewire-pulse and natively, and what PipeWire does when the latency changes mid-stream.
4. **Where the picker appears on Wayland.** A window can't place itself next to the tray icon.
   Plasma has layer-shell; GNOME does not. Centre it, or accept the compositor's placement?
5. **Switching the default output.** Ticking a speaker changes the system default, as the Mac
   does. Fedora users objected when AirPlay outputs took the default uninvited; this is the
   user's own click, but confirm it reads that way.
6. **More than one server.** One row, for the server that holds the connection. A second paired
   server displaces an idle first one (mac: `TODO.md` item 6). Show them all?
7. **How fast a dead speaker is noticed.** Measure it against real speakers; if it takes the
   write-stall limit, ask for a liveness check in sendspin-server-cpp.
