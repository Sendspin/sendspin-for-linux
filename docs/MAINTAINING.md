# Maintaining Sendspin for Linux

Packages, releases and the package repositories. Building and testing are in
[DEVELOPING.md](DEVELOPING.md).

## Packages and releases

`scripts/vendor.sh` makes the source tarball: the tree at `HEAD` with what the build would
download in `third_party/fetched/`, and its commit's date in `RELEASE_DATE` for the metainfo. A
tree that has that directory builds offline. The packages are CPack's, built from the tarball
on the release they are for, Arch's is `makepkg`'s from `arch/PKGBUILD`, and the Flatpak is
`flatpak/com.sendspin_audio.Sendspin.yml`, built inside the tarball:

```sh
scripts/vendor.sh dist
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=/usr
cmake --build build && (cd build && cpack -G DEB)   # or RPM
```

CI builds all of them on every change, builds the tarball with no network, and installs and
starts each package in a clean container. To release, set `project(VERSION)` in
`CMakeLists.txt` and push the tag `v<version>`: the Release workflow attaches the tarball, the
packages, the Flatpak bundle, the manifest Flathub builds from and `SHA256SUMS` to a draft
release. Its last job signs `SHA256SUMS` with the repositories' key, once the
`package-signing` environment's reviewer approves it: that signature is what an installed app
verifies an update with, so publish the draft after it is there. The job fails if
`data/sendspin.asc`, the public key the packages carry, does not verify what it signed. CI has
each package update itself to a later build in a clean container (`ci/update-test.sh`).
[flatpak/FLATHUB.md](../flatpak/FLATHUB.md) has the Flathub side: submitting the app, and what
each release needs there.

## Package repositories

Publishing the release runs the Package repositories workflow: `scripts/package-repos.sh` builds
apt and dnf repositories from the packages of every published release, signs them, and the
workflow puts them on GitHub Pages. It can be run by hand too, after a release is deleted or the
key is replaced. CI builds the same repositories with a key of its own on every change, and
installs and upgrades from them in clean containers (`ci/repo-fixture.sh`, `ci/repo-test.sh`).
That test sets its machine up by running the README's own install blocks, the ones marked
`<!-- repo-setup: … -->`, against its fixture: what the README tells a user is what CI installs
with.

The script takes any package named `<name>-<version>-<distro>-<arch>.deb` or `.rpm`, so another
project's packages are published by downloading them into the same directory.

A machine asks for the repository of its own release, `apt/dists/debian13` or `rpm/fedora45`, so
a release of a distribution has packages only once it is built for: a new Fedora needs an entry
beside the last one in each matrix of `packages.yml` and in `ci.yml`, its `.rpm` in the files
`release.yml` expects, and its name in the README. Its repository appears with the next release
published after that.

Only what the Release workflow built is signed: each package has to match its release's
`SHA256SUMS` and carry that workflow's build attestation, or the run fails.

### One-time setup

Once, by a maintainer:

1. Create the signing key on a machine you trust: a primary key that only certifies and stays
   there, and a signing subkey, which alone goes to GitHub. Give it a passphrase.

   ```sh
   gpg --quick-generate-key "Sendspin packages" rsa4096 cert never
   gpg --quick-add-key <primary fingerprint> rsa4096 sign never
   gpg --list-keys --with-subkey-fingerprints "Sendspin packages"
   gpg --armor --export-secret-subkeys '<subkey fingerprint>!'
   ```

   Put the primary key's fingerprint in the README's install steps, twice, and in SPEC.md's
   "Updates", and the public key (`gpg --armor --export`) in `data/sendspin.asc`: the packages
   install it, and an installed app verifies its updates with that file alone. Keep the primary
   key and the passphrase somewhere safe: machines that installed from the repositories trust
   this key, and a new one means setting each of them up again.
2. Create the environment `package-signing`, with a required reviewer and its deployment
   branches and tags limited to `main` and `v*`. Add the export there as the secret
   `REPO_GPG_PRIVATE_KEY` and the passphrase as `REPO_GPG_PASSPHRASE`: environment secrets, not
   repository secrets, so only the jobs that name the environment can read them: this one, and
   the Release workflow's, which signs a draft's `SHA256SUMS`. Each then waits for the reviewer.
3. Set the Pages source to **GitHub Actions**. Publishing a release runs on its tag, and the
   `github-pages` environment allows only the default branch at first: add `v*` to its
   deployment branches and tags.
4. Add a ruleset that restricts who can create `v*` tags, and turn on immutable releases, so a
   published release's files cannot be replaced before they are signed.

## The AUR

`arch/PKGBUILD` is the AUR's: it downloads a published release's tarball and checks its sha256,
which only exists once the release does. So set `pkgver` with `project(VERSION)`, and after
publishing, on Arch (`updpkgsums` is in `pacman-contrib`):

```sh
cd arch && updpkgsums && makepkg --printsrcinfo >.SRCINFO
```

Commit both, and copy them to the AUR repository, which needs a maintainer's account. CI builds
the PKGBUILD against the tarball it has just made instead, with that tarball's sum.
