# Packaging Tesseract as a Flatpak

This directory holds the source-of-truth Flatpak manifest,
[`io.github.surakin.Tesseract.yml.in`](io.github.surakin.Tesseract.yml.in)
(CMake generates the tracked `io.github.surakin.Tesseract.yml` from it,
substituting the project version into the `tesseract` module's git `tag:` —
same pattern as `packaging/arch/PKGBUILD.in` → `PKGBUILD`), plus its
supporting files:

- [`io.github.surakin.Tesseract.metainfo.xml`](io.github.surakin.Tesseract.metainfo.xml) —
  AppStream metadata (description, screenshots, release history).
- [`cargo-sources.json`](cargo-sources.json) — generated offline-vendoring
  manifest for every Rust dependency, produced from the root `Cargo.lock` by
  [`flatpak-cargo-generator.py`](https://github.com/flatpak/flatpak-builder-tools/tree/master/cargo)
  (the Flatpak build has no network access, so every crate download has to be
  pre-declared as a pinned source).

CI builds the manifest offline on every PR that touches this directory or
the dependency-defining files
([`.github/workflows/flatpak.yml`](../../.github/workflows/flatpak.yml)),
as a correctness check — it does not sign or publish. Producing a real,
installable release is described below.

## Distribution: a self-hosted Flatpak repo, published automatically

Flathub does not accept this app (its 2026-05-29 policy bars AI-assisted
code; see the note at the end). The distribution channel is a self-hosted,
GPG-signed OSTree repo that users add as a Flatpak remote — mechanically
the same thing Flathub is, minus the store listing — published to GitHub
Pages at `https://surakin.github.io/tesseract/flatpak-repo/`.

**This is fully automatic.** Pushing a `vX.Y.Z` tag triggers the `flatpak`
job in
[`.github/workflows/package.yml`](../../.github/workflows/package.yml),
which regenerates `cargo-sources.json` from that tag's `Cargo.lock`,
builds and GPG-signs the manifest offline, updates the OSTree repo
(static deltas, pruned), publishes it to the `gh-pages` branch, and attaches
a single-file `.flatpak` bundle to the tag's GitHub Release. A separate
`pages-docs` job in the same workflow publishes this repo's `docs/`
(the marketing site) to the same branch. Neither job wipes the other's or
a previous release's content — both deploy with `keep_files: true`, so the
OSTree repo's own update/rollback history keeps accumulating across
releases the way Flatpak expects. See "One-time setup" below for the
prerequisites this depends on (a GPG key, two static files, and the Pages
branch itself).

For an on-demand real publish without cutting a new tag (recovering from a
failed run, republishing, one-off testing), use
[`.github/workflows/build-platform.yml`](../../.github/workflows/build-platform.yml)
manually with both `flatpak` and `flatpak_publish` inputs enabled — it
runs the same sign-and-publish steps off whatever ref you dispatch it
against.

### Manual local build (for testing changes to the manifest itself)

```bash
# One-time: the build toolchain
flatpak install flathub org.kde.Platform//6.9 org.kde.Sdk//6.9 \
    org.freedesktop.Sdk.Extension.golang//24.08
# plus flatpak-builder from your distro (e.g. `sudo pacman -S flatpak-builder`)

# Regenerate the tracked .yml for the version you are testing
cmake --preset linux-debug -DTESSERACT_UI=qt6      # any preset; substitutes @PROJECT_VERSION@

# Build into a local OSTree repo, GPG-signed (use your own test key, not the
# release signing key, for local iteration)
flatpak-builder --force-clean --gpg-sign=$GPG_KEYID \
    --repo=repo build-dir io.github.surakin.Tesseract.yml

flatpak build-update-repo --generate-static-deltas --prune --prune-depth=20 \
    --gpg-sign=$GPG_KEYID repo
```

`repo/` is now a self-contained directory of static files you can point a
local Flatpak remote at, without touching the published repo.

**GitHub Pages caveats.** Published sites have a soft **1 GB** size cap
(hence `--prune-depth`) and a soft **100 GB/month** bandwidth cap. A
popular desktop app can blow past the bandwidth limit — front the repo with
Cloudflare (free CDN over Pages) or move it to Cloudflare R2 if that
happens. The OSTree repo is host-agnostic static content, so migrating is a
copy, no manifest change.

### User-facing files

`tesseract.flatpakrepo` and `io.github.surakin.Tesseract.flatpakref`, both
tracked in this directory, are static and committed once — not regenerated
per release, since the signing key and the Pages URL are both stable. Every
publish run (automatic or manual) just copies them alongside `repo/` onto
`gh-pages`.

`tesseract.flatpakrepo` — adds the remote:

```ini
[Flatpak Repo]
Title=Tesseract
Url=https://surakin.github.io/tesseract/flatpak-repo/
Homepage=https://github.com/surakin/tesseract
Comment=Cross-platform Matrix chat client
Description=Tesseract release channel
Icon=https://surakin.github.io/tesseract/flatpak-repo/io.github.surakin.Tesseract.svg
GPGKey=<base64 of `gpg --export $GPG_KEYID`>
```

`io.github.surakin.Tesseract.flatpakref` — one-click add-remote-and-install:

```ini
[Flatpak Ref]
Name=io.github.surakin.Tesseract
Branch=master
Url=https://surakin.github.io/tesseract/flatpak-repo/
Title=Tesseract
Homepage=https://github.com/surakin/tesseract
IsRuntime=false
GPGKey=<base64 of `gpg --export $GPG_KEYID`>
RuntimeRepo=https://flathub.org/repo/flathub.flatpakrepo
```

`RuntimeRepo` lets a fresh machine pull `org.kde.Platform` from Flathub
automatically. Once a user has the remote, GNOME Software / KDE Discover
list Tesseract and update it like any other app — which is why the manifest
bothers to install the app-id-named `.desktop`, icon, and metainfo.

Regenerate both files (with the same `Url`/`Icon`, only `GPGKey` changing)
if the signing key is ever rotated.

## Single-file bundle

Every automatic or manual publish run also builds
`tesseract-vX.Y.Z.flatpak` and attaches it to the GitHub Release, for users
who don't want to add the remote. `flatpak install ./tesseract-vX.Y.Z.flatpak`
installs it; unlike the repo, it has no auto-updates — reinstalling the
newer bundle is the only way to update.

## Local test build

```bash
flatpak-builder --user --install --force-clean --repo=test-repo \
    build-dir io.github.surakin.Tesseract.yml
flatpak run io.github.surakin.Tesseract
```

Exercise login, sending a message, camera, screen share, notifications, the
tray icon, and `/location` — the sandboxed capabilities this manifest's
`finish-args` were scoped around. Then lint:

```bash
appstreamcli validate --no-net io.github.surakin.Tesseract.metainfo.xml
desktop-file-validate build-dir/files/share/applications/io.github.surakin.Tesseract.desktop
```

## Per-release checklist

Everything below must land in a commit **before** the tag is pushed — the
tag is what CI builds from, and none of this can be fixed retroactively
without re-tagging.

1. If `sdk/Cargo.toml`'s `webrtc-sys` / `livekit` pins moved, re-derive the
   prebuilt-webrtc archive URL + `sha256` (the tag is `webrtc-sys-build`'s
   `WEBRTC_TAG` constant) and update the `archive` source in `.yml.in`.
2. If the matrix-rust-sdk fork bumped its MSRV past `rust-1.97.0`, bump the
   `rust-toolchain` module's pinned version + `sha256` (published at
   `https://static.rust-lang.org/dist/rust-<ver>-x86_64-unknown-linux-gnu.tar.gz.sha256`).
3. Add a `<release version="X.Y.Z" date="YYYY-MM-DD"/>` line (newest first)
   to `io.github.surakin.Tesseract.metainfo.xml`, sourced from `CHANGES.md`.
   CI's `flatpak` job fails the whole publish if this is missing.
4. Cut the release (`git tag vX.Y.Z && git push github vX.Y.Z`). CI takes it
   from here — see "Distribution" above.

`cargo-sources.json` no longer needs manual regeneration before tagging; CI
regenerates it from the tagged `Cargo.lock` itself. The tracked copy in this
directory is only a "last known good" convenience for the local manual
build above.

## One-time setup

Required once before the automatic publish flow above can run at all:

1. **Generate a dedicated GPG signing key** (`gpg --full-generate-key`) —
   don't reuse a key from anything else, so a compromise or rotation here
   doesn't implicate it. Back up the private key and revocation certificate
   somewhere outside GitHub. Then add these three repository secrets
   (Settings → Secrets and variables → Actions):
   - `FLATPAK_GPG_PRIVATE_KEY` — `gpg --export-secret-keys --armor $GPG_KEYID | base64 -w0`
   - `FLATPAK_GPG_PASSPHRASE` — the key's passphrase, if it has one
   - `FLATPAK_GPG_KEY_ID` — `$GPG_KEYID` itself (not sensitive, kept as a
     secret alongside the others for simplicity)

   CI imports the private key into an ephemeral keyring for the duration of
   a single job and discards it when the runner is torn down; it is never
   written to the repo or any commit.
2. **Create `tesseract.flatpakrepo` and `io.github.surakin.Tesseract.flatpakref`**
   in this directory from the templates under "User-facing files" above,
   with `GPGKey` set to `gpg --export $GPG_KEYID | base64 -w0`. Commit them
   — they're static and reused by every publish run.
3. **Point GitHub Pages at the `gh-pages` branch** (Settings → Pages →
   Source), instead of `main`. If `gh-pages` doesn't exist yet, create an
   empty one first so the option is selectable, or just wait for the first
   successful `pages-docs`/`flatpak` run to create it.

## Note on Flathub

The manifest is still a valid Flathub submission and the per-release
checklist above is what a Flathub update would need too. But Flathub's
[2026-05-29 policy](https://docs.flathub.org/) prohibits AI-assisted code
across app code, manifests, and metadata, with a discretionary carve-out for
"mature, well-maintained projects". If that carve-out is ever granted, the
Flathub-published copy would live in a separate
`flathub/io.github.surakin.Tesseract` repo and its buildbot would rebuild on
push; nothing in this directory changes.

## Why the manifest looks the way it does

Three build-system obstacles had to be worked around to make an **offline**
build succeed (flatpak-builder runs each module's build phase with no
network; the CI check and a release build both depend on it):

1. Root `CMakeLists.txt` `FetchContent`s Corrosion and nlohmann_json from
   GitHub at CMake configure time. The manifest stages pinned-commit copies
   as extra `git` sources under `_deps-src/` and points CMake at them via
   `-DFETCHCONTENT_SOURCE_DIR_CORROSION=...` /
   `-DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=...` +
   `-DFETCHCONTENT_FULLY_DISCONNECTED=ON`.
2. `tests/CMakeLists.txt` unconditionally `FetchContent`s Catch2 — but
   packaging never runs `ctest` anyway (see
   [`packaging/arch/aur/README.md`](../arch/aur/README.md)'s note on this),
   so the manifest just passes `-DTESSERACT_BUILD_TESTS=OFF` (a CMake option
   added specifically for this) and skips the problem entirely.
3. `webrtc-sys-build`'s `build.rs` (pulled in via `livekit`, pinned
   `webrtc-sys = "=0.3.39"` in `sdk/Cargo.toml`) downloads a prebuilt
   libwebrtc archive over HTTP during `cargo build` — not something
   `flatpak-cargo-generator.py` can vendor, since it isn't a crates.io
   dependency. The manifest adds that exact archive as a pinned, checksummed
   `archive` source and sets `LK_CUSTOM_WEBRTC` to point at it; a small
   addition to the root `CMakeLists.txt` (`corrosion_set_env_vars(... 
   LK_CUSTOM_WEBRTC=...)`, guarded on the env var being set) forwards it into
   Cargo's build-script environment, mirroring how `AWS_LC_SYS_CMAKE_BUILDER`
   is already forwarded.

The matrix-rust-sdk dependency is a git pin (`surakin/matrix-rust-sdk`, a
fork), not a crates.io release. `flatpak-cargo-generator.py` handles this: it
emits a `git` source that clones the fork at the pinned rev, shell steps that
copy each workspace crate into `cargo/vendor/`, and a `[source."…"]`
`replace-with` stanza in the generated `cargo/config` so the offline build
resolves the git dependency from the vendored copy.

`ui/linux-qt/CMakeLists.txt` also unconditionally renames the installed
binary/desktop-file/icon from `tesseract` to `tesseract-matrix` (to coexist
with the unrelated Tesseract OCR engine, which owns `/usr/bin/tesseract` on
most distros). The manifest's `command:` is `tesseract-matrix` to match, and
a `post-install` step derives the app-id-named
`io.github.surakin.Tesseract.desktop`/`.svg` (which the desktop software
centres need in order to list and update the app) from the renamed artifacts
CMake's own install step already produced.

Camera access uses `--device=all` — Flatpak has no Camera-specific device
class, and Tesseract talks to `/dev/videoN` directly via GStreamer's
`v4l2src` (no portal). This is the same limitation every other raw-V4L2
Flatpak app hits (e.g. Cheese); if the app is ever submitted to Flathub,
expect reviewers to ask for a short justification rather than reject it.
