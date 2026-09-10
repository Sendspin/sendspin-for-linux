# Sendspin for Linux — spec

**Status:** draft, 2026-09-10. Tracks [Sendspin/backlog#46](https://github.com/Sendspin/backlog/issues/46),
the Linux child of [#42](https://github.com/Sendspin/backlog/issues/42). #42's AVP (built-in
server / cast mode) is out of scope. Sources for every claim here are in [research.md](research.md).

## What it is

A background app for Linux desktops that makes the PC a Sendspin client in both directions:

- **Player** — Music Assistant plays to this PC's speakers.
- **Source** — this PC's audio goes to Sendspin speakers, through a "Sendspin" output in the
  OS sound settings or from a line-in device.

It installs, autostarts and works with no configuration. Headless machines are not a target;
they use `sendspin-cli` (#46).

## MVP scope

- Roles: `player@v1`, `controller@v1`, `metadata@v1`, `artwork@v1`, `source@v1`.
- Full v1.0: Noise encryption, Pairing PSK, dynamic pairing code. Unpaired access (guest mode)
  on by default.
- Announces itself over mDNS; servers connect in.
- Native media controls (MPRIS). Tray icon where the desktop has one.
- Settings: device name, output device, input (Sendspin output or a line-in device), guest mode,
  media controls on/off.

### Not in the MVP

- Built-in server / cast mode (#42 AVP).
- Choosing the target speaker or group from the app, or one OS output per speaker. The spec
  leaves target choice to the server; see [Where source audio plays](#where-source-audio-plays).
- Capturing whatever is playing locally without switching outputs.
- PulseAudio-only systems. PipeWire is the default everywhere we target.
- A daemon/headless mode.

## Connection

- Advertise `_sendspin._tcp.local.` on port 8928, TXT `path=/sendspin`, `name=<pretty hostname>`.
  Server-initiated only: never dial out while advertising.
- Skip docker, VM and VPN interfaces and temporary IPv6 addresses when announcing.
- The spec admits one connection per client, so player and source always use the same server.
  Arbitration between servers follows the spec (`playback` > `pairing` > idle).
- Identity is a long-term Curve25519 key pair generated on first start; `client_id` is its
  public key. Losing it makes the PC a new device to every server.
- One instance per user session. A second launch opens the running instance's settings window.

## Pairing and guest mode

- **Guest mode on:** the client advertises `unpaired_access.enabled`. The PC shows up in MA and
  can be played to after the user approves it there.
- **The source role needs pairing.** The spec requires explicit approval for `source@v1`, and MA
  only accepts paired sources, so guest mode never covers sending audio.
- Pairing methods offered: Pairing PSK (always, as the spec requires) and the dynamic pairing
  code on `display`. No static code. No spoken digits.
- When MA starts dynamic pairing, show the code (`123-456`) and its QR code in a window, raised
  through a notification so it works without a tray.
- The Pairing PSK token (`SP:0…`) is available in settings for pairing by token.
- Keep at least 5 pairing records. Never log keys, PSKs or tokens.

## Playing on this PC

- A PipeWire playback stream named "Sendspin" (`media.role=Music`) that follows the default
  output, or the device chosen in settings.
- **Never plays into our own Sendspin output.** If the user makes it the default, playback stays
  on the last hardware output.
- Advertise only formats that play without a resampler: the sink's `EnumFormat` intersected with
  the daemon's `clock.allowed-rates`. Opus only at the rates its decoder accepts.
- The PC's own player volume is the app's stream volume in the OS mixer, both ways.
- Transport commands (play/pause/next/previous/volume/mute) go through the controller role and
  act on the group. Offer only what the server lists in `supported_commands`.

## Where the playing media shows up

**MPRIS** — the primary surface on every desktop.

- Bus name `org.mpris.MediaPlayer2.<app-id>`, owned by the process that plays the audio
  (WirePlumber pairs MPRIS players with streams by process).
- Registered while connected to a server; `CanPlay=false` while the group is idle, so GNOME hides
  the card; unregistered on disconnect.
- Shows up in GNOME's notification popover and lock screen (GNOME 49+), Plasma's media applet
  and lock screen, and drives media keys on both.
- Metadata from `metadata@v1`. Artwork from `artwork@v1`, written as a `file://` path named by a
  hash of the image bytes, under `$XDG_RUNTIME_DIR/app/<app-id>/`.
- `Volume` is the group volume.
- Can be switched off in settings (MA had to add the same switch for its web player).

**Tray** — StatusNotifierItem, so KDE Plasma and GNOME with the AppIndicator extension.

- A status line: `Title — Artist · <group>`, `Sending to Sendspin`, `Waiting for Music Assistant`,
  or `Not connected`.
- Play/pause, next, previous, mute. Scrolling on the icon changes group volume. Menus hold no
  sliders or artwork.
- Settings…, Quit.

**Stock GNOME has no tray.** There the MPRIS card is the surface, the Flatpak build appears under
Background Apps in Quick Settings with a one-line status, and the app launcher opens settings.

## Sending this PC's audio

### Inputs

One input at a time, chosen in settings.

1. **Sendspin output (default).** A PipeWire node with `media.class=Audio/Sink`, created at
   startup and gone when the app exits or crashes.
   - `node.description=Sendspin`, `node.network=true`, `node.virtual` unset — Plasma's applet
     hides virtual sinks by default.
   - Low `priority.session` so WirePlumber never makes it the default on its own.
   - Visible in GNOME Settings, the Plasma applet and pavucontrol. The user selects it
     system-wide or per app.
2. **Line-in device.** Capture from a PipeWire source the user picks.

### Streaming

- Report `available: true` only once the time filter has converged.
- Stream only after the server sends `source start`; stop on `source stop`.
- Timestamp each buffer from `pw_stream_get_time_n()`, mapped into server time through the time
  filter. Chunks of 5–150 ms. After a stall, drop the backlog rather than burst it.
- Report `signal` (`line_sense`): present while non-silent audio arrives, absent otherwise.
- Never play the input locally. If this PC is in the group that plays it, it hears it through the
  server like any other player.
- Expect a noticeable delay. Fine for music; video will not be in sync.

### Where source audio plays

The server decides. The PC appears in MA as a Live Input, and the user picks the speaker there,
or sets MA's per-device "Automatically play line-in on" so that audio reaching the Sendspin
output starts playing on its own. The app says this plainly and does not pretend to choose.

Choosing from the app needs a spec change; raise it on
[backlog#56](https://github.com/Sendspin/backlog/issues/56) or a new spec issue.

### What shows while sending

Nothing new in the media widgets: the app playing into the Sendspin output keeps its own MPRIS
card and controls. The tray status changes. Publishing that app's track info to the server is
what backlog#56 proposes.

## Loop guards

- The player stream never targets our own Sendspin output.
- No capture of the default sink's monitor.
- Never play captured input locally.
- MA's auto-play default can still send a desktop's audio back to itself — open question 1.

## Platform

- **C++**, on [sendspin-cpp](https://github.com/Sendspin/sendspin-cpp).
- **PipeWire required.**
- **Flatpak first:**
  - `--filesystem=xdg-run/pipewire-0` (not `--socket=pulseaudio`)
  - `--talk-name=org.kde.StatusNotifierWatcher`
  - `--system-talk-name=org.freedesktop.Avahi`
  - MPRIS name is auto-granted — do not declare it
  - Background portal for autostart
- Outside Flatpak, an XDG autostart entry.

## Depends on

- **sendspin-cpp**
  - `source@v1`: tracker #95, PR stack #112–#117.
  - Encryption and pairing: the `encryption-support` branch, unmerged with no PR (87 ahead / 6
    behind as of 2026-09-10).
  - Unpaired access.
  - Player, controller, metadata and artwork already exist.
- **sendspin-cpp-cli** — a PipeWire playback sink and dns_sd mDNS already exist, inside the
  CLI-shaped `sendspin-cli-core` static library.
- **local-audio-addon** — MA's local line-in is planned there
  ([server#5079](https://github.com/music-assistant/server/pull/5079)). Share one source
  implementation.
- **Spec** — target selection from a client (above).

## Open questions

1. **MA's line-in auto-play default.** For a client that is both player and source, MA
   pre-selects "This device"
   (`music_assistant/providers/sendspin/player.py`, `_get_source_autostart_config_entries`). On a
   desktop that plays your own audio back out of your own speakers, delayed, until the user
   changes it. Options:
   - ask MA to default it to Off, or to ask during pairing;
   - don't report `signal`, and start the input by hand in MA every time;
   - keep the default and point the user at the setting after pairing.
2. **UI toolkit.** Qt 6 has a tray (StatusNotifierItem), QtDBus for MPRIS and a KDE Flatpak
   runtime. gtkmm 4 looks native on GNOME but has no tray API, so SNI would be hand-rolled.
3. **Reuse from sendspin-cpp-cli.** Link `sendspin-cli-core`, or extract its sinks and mDNS into
   a library both use?
4. **Encryption in sendspin-cpp.** Who drives `encryption-support` to a PR, and when?
5. **Key storage.** A 0600 file under the XDG data dir, or the Secret portal?
6. **Source codec.** FLAC or PCM on a LAN, Opus as a fallback?
7. **Inputs.** Confirm the MVP is just the Sendspin output plus a line-in device.
8. **App ID.** `io.sendspin.client` is already taken by chrisuthe/sendspin-player.
