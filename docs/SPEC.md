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
- Native media controls (MPRIS), an icon menu where the desktop has a tray, and one Settings
  window. See [UI](#ui).

### Not in the MVP

- Built-in server / cast mode (#42 AVP).
- A now-playing window. What is playing shows in the desktop's own media controls.
- Choosing the target speaker or group from the app, or one OS output per speaker. The spec
  leaves target choice to the server; see [Where source audio plays](#where-source-audio-plays).
- Capturing whatever is playing locally without switching outputs.
- Systems without PipeWire. See [Platform](#platform) for why.
- A daemon/headless mode.

## UI

Three surfaces, and no others:

1. **The desktop's media controls** (MPRIS) — what is playing, and transport. See
   [Where the playing media shows up](#where-the-playing-media-shows-up).
2. **The icon menu** — status and transport, then Settings… and Quit.
3. **One Settings window:**
   - connection status: which server, or not connected
   - pairing: the code and QR code while MA is pairing, and the Pairing PSK token
   - device name (MA's own custom player name still wins in MA)
   - output device (default: follow the system default)
   - input: Sendspin output or a line-in device
   - guest mode on/off
   - media controls on/off

The Settings window opens from the icon menu, from the app launcher, on a second launch, and from
the pairing notification. On stock GNOME, which has no tray, the launcher is the only way in.

Every setting is live — no Save button.

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
- When MA starts dynamic pairing, post a notification with the code (`123-456`). Activating it
  opens the Settings window, which shows the code and its QR code until pairing ends.
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
Background Apps in Quick Settings with a one-line status, and the app launcher opens the Settings
window.

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
- **Qt 6** for the icon menu (StatusNotifierItem) and the Settings window, QtDBus for MPRIS.
- **PipeWire required.** No PulseAudio or ALSA fallback for now, because:
  - Every supported release uses PipeWire by default.
  - The Sendspin output depends on it. A PipeWire node disappears with the app, can be marked as a
    network device so Plasma's applet shows it, and timestamps each buffer from the graph clock.
  - A PulseAudio version of that output would leak its sink when the app crashes, need a second
    timing path to get sync right, and rely on module loading, which is unsafe from a Flatpak.
  - Revisit if people ask. Playback and line-in could fall back cheaply to sendspin-cpp-cli's
    PulseAudio/ALSA outputs and PortAudio capture; the Sendspin output could not.

### Install methods

Listed in this order in the README and on the website:

1. **Distro packages** — `.deb` for Debian and Ubuntu, `.rpm` for Fedora, attached to each GitHub
   release, with package repositories later. Arch through the AUR. Same problem as
   [backlog#52](https://github.com/Sendspin/backlog/issues/52) for `sendspin-cli`; solve it once.
   - Built against the distro's Qt 6, libpipewire and avahi dns_sd.
   - Installs an XDG autostart entry.
2. **Flatpak** (Flathub), on the KDE runtime.
   - `--filesystem=xdg-run/pipewire-0` (not `--socket=pulseaudio`)
   - `--talk-name=org.kde.StatusNotifierWatcher`
   - `--system-talk-name=org.freedesktop.Avahi`
   - MPRIS name is auto-granted — do not declare it
   - Background portal for autostart. Only this build gets GNOME's Background Apps entry.

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
2. **Reuse from sendspin-cpp-cli.** Link `sendspin-cli-core`, or extract its sinks and mDNS into
   a library both use?
3. **Encryption in sendspin-cpp.** Who drives `encryption-support` to a PR, and when?
4. **Key storage.** A 0600 file under the XDG data dir, or the Secret portal?
5. **Source codec.** FLAC or PCM on a LAN, Opus as a fallback?
6. **Inputs.** Confirm the MVP is just the Sendspin output plus a line-in device.
7. **App ID.** `io.sendspin.client` is already taken by chrisuthe/sendspin-player.
