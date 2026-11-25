# Flathub

Flathub builds `com.sendspin_audio.Sendspin` from its own repository, which holds one file of
ours: the manifest. It is `com.sendspin_audio.Sendspin.yml` here with the release's source
tarball, by URL and sha256, in place of the tree. `scripts/flathub-manifest.sh` writes it, and
every release has it attached, as `com.sendspin_audio.Sendspin.yml`.

CI builds that manifest on every change with Flathub's own builder (`flathub-build` in
`org.flatpak.Builder`), the tarball of the run as its archive, and lints it with
`flatpak-builder-lint`. The metainfo and the built repo are linted on a tag only: the linter
fetches the screenshots, and the metainfo links them by the release's tag.

## Submitting, once

After the first release is published, so that the tarball's URL answers:

1. Fork [flathub/flathub](https://github.com/flathub/flathub) with "Copy the master branch
   only" unticked, and branch from `new-pr`.
2. Add the release's `com.sendspin_audio.Sendspin.yml`, nothing else.
3. Open the pull request against `new-pr`, titled `Add com.sendspin_audio.Sendspin`, and go
   through its checklist. `bot, build` in a comment starts a test build; its result links a
   build to install and try.
4. When it is merged, Flathub creates `flathub/com.sendspin_audio.Sendspin` and invites the
   submitter, whose GitHub account needs two-factor authentication. The app is published a
   few hours after.
5. Verify the app from its page on flathub.org: the ID's domain is `sendspin-audio.com`, and
   Flathub asks for a token at `https://sendspin-audio.com/.well-known/org.flathub.VerifiedApps.txt`.

Flathub builds for x86_64 and aarch64. CI builds x86_64 only: if the aarch64 test build fails
and can't be fixed in time, a `flathub.json` with `{"only-arches": ["x86_64"]}` next to the
manifest leaves it out.

### What a reviewer will ask

The linter has nothing to say about the permissions, so there is no exception to request. A
reviewer reads them all the same (`docs/SPEC.md`, "Install methods"):

| Permission | Why |
| --- | --- |
| `--filesystem=xdg-run/pipewire-0` | The app creates the "Sendspin" output and captures it with its timing, and plays in sync on a chosen output. The PulseAudio socket can do neither, and the camera portal's PipeWire has no audio. EasyEffects is the precedent. |
| `--system-talk-name=org.freedesktop.Avahi` | Finds the speakers and advertises this PC to Music Assistant, over mDNS. |
| `--share=network` | Serves the speakers and dials them; connects to Music Assistant. |
| `--talk-name=org.kde.StatusNotifierWatcher` | The tray icon. |
| `--talk-name=org.freedesktop.Notifications` | The pairing code. It goes once the notification goes through the portal ([#15](https://github.com/Sendspin/sendspin-for-linux/issues/15)). |

## Each release

1. Tag and publish the release as always (README, "Packages and releases"). The tag's run
   fails if the linter would turn the release down.
2. Flathub's bot reads the manifest's `x-checker-data`, finds the new tarball and opens a pull
   request on `flathub/com.sendspin_audio.Sendspin` with its URL and sha256. Merge it when its
   test build passes.
3. When the manifest itself changed here (a permission, the runtime's version, a build option),
   the bot's pull request doesn't have that: replace the file with the release's
   `com.sendspin_audio.Sendspin.yml` instead.

When Flathub's build warns that the KDE runtime is near its end of life, raise
`runtime-version` here and release.

## The metainfo

`data/com.sendspin_audio.Sendspin.metainfo.xml.in`. The release it lists is `project(VERSION)`,
dated by the tagged commit (`RELEASE_DATE` in the tarball), not by Flathub's build of it.

The screenshots are `data/screenshots/picker.png` and `settings.png`, linked by the release's
tag. `scripts/render-screenshots.sh` draws them offscreen from the windows' own QML, over
made-up speakers: run it when a window changes, and commit the result. Pictures taken on a
desktop can replace them under the same names.

## Trying it here

```sh
flatpak install --user flathub org.flatpak.Builder
tarball=$(scripts/vendor.sh dist)
scripts/flathub-manifest.sh "file://$PWD/$tarball" "$(sha256sum "$tarball" | cut -d' ' -f1)" \
    >com.sendspin_audio.Sendspin.yml
flatpak run --command=flathub-build org.flatpak.Builder --install com.sendspin_audio.Sendspin.yml
flatpak run --command=flatpak-builder-lint org.flatpak.Builder manifest com.sendspin_audio.Sendspin.yml
flatpak run --command=flatpak-builder-lint org.flatpak.Builder repo repo
```

Before the version's tag is pushed, the last one reports the screenshots as missing.
