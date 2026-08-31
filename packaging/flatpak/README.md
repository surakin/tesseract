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

Nothing is built or published from this repo automatically. CI
([`.github/workflows/flatpak.yml`](../../.github/workflows/flatpak.yml))
builds the manifest offline on every PR that touches this directory or the
dependency-defining files, as a correctness check — it does **not** publish.
Producing a release build and putting it somewhere installable is the manual
process below.

## Distribution: a self-hosted Flatpak repo

Flathub does not accept this app (its 2026-05-29 policy bars AI-assisted
code; see the note at the end). The supported distribution channel is a
self-hosted OSTree repo that users add as a Flatpak remote — mechanically
the same thing Flathub is, minus the store listing.

### Build and export

```bash
# One-time: the build toolchain
flatpak install flathub org.kde.Platform//6.9 org.kde.Sdk//6.9 \
    org.freedesktop.Sdk.Extension.golang//24.08
# plus flatpak-builder from your distro (e.g. `sudo pacman -S flatpak-builder`)

# Regenerate the tracked .yml for the version you are releasing
cmake --preset linux-debug -DTESSERACT_UI=qt6      # any preset; substitutes @PROJECT_VERSION@

# Build into a local OSTree repo, GPG-signed
flatpak-builder --force-clean --gpg-sign=$GPG_KEYID \
    --repo=repo build-dir io.github.surakin.Tesseract.yml

# Regenerate summary + static deltas, prune old history (keep the repo small
# — see the GitHub Pages size cap below)
flatpak build-update-repo --generate-static-deltas --prune --prune-depth=20 \
    --gpg-sign=$GPG_KEYID repo
```

`repo/` is now a self-contained directory of static files.

### Deploy

Copy `repo/` to any static web host — GitHub Pages, GitLab Pages,
Cloudflare Pages, an S3/R2 bucket, or your own box:

```bash
rsync -a --delete repo/ user@host:/var/www/tesseract-flatpak/
```

Nothing server-side is required; it is all static content.

**GitHub Pages caveats.** Published sites have a soft **1 GB** size cap
(hence `--prune-depth`) and a soft **100 GB/month** bandwidth cap. A
popular desktop app can blow past the bandwidth limit — front the repo with
Cloudflare (free CDN over Pages) or move it to Cloudflare R2 if that
happens. The OSTree repo is host-agnostic static content, so migrating is a
copy, no manifest change.

### User-facing files

Commit these two alongside `repo/` once the repo URL is fixed (replace
`REPLACE_WITH_YOUR_REPO_URL` with e.g. `https://flatpak.tesseract.example`):

`tesseract.flatpakrepo` — adds the remote:

```ini
[Flatpak Repo]
Title=Tesseract
Url=REPLACE_WITH_YOUR_REPO_URL/
Homepage=https://github.com/surakin/tesseract
Comment=Cross-platform Matrix chat client
Description=Tesseract release channel
Icon=REPLACE_WITH_YOUR_REPO_URL/io.github.surakin.Tesseract.svg
GPGKey=<base64 of `gpg --export $GPG_KEYID`>
```

`io.github.surakin.Tesseract.flatpakref` — one-click add-remote-and-install:

```ini
[Flatpak Ref]
Name=io.github.surakin.Tesseract
Branch=master
Url=REPLACE_WITH_YOUR_REPO_URL/
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

## Alternative: single-file bundle

For a one-off with no repo and no auto-updates:

```bash
flatpak build-bundle repo tesseract.flatpak io.github.surakin.Tesseract
```

Attach `tesseract.flatpak` to a GitHub Release. Users run
`flatpak install ./tesseract.flatpak`. They must re-download to update.

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

1. Cut the release in the main repo (`git tag vX.Y.Z && git push github vX.Y.Z`).
2. Regenerate `cargo-sources.json` from the tagged `Cargo.lock`:
   ```bash
   curl -sL -o /tmp/flatpak-cargo-generator.py \
     https://raw.githubusercontent.com/flatpak/flatpak-builder-tools/master/cargo/flatpak-cargo-generator.py
   python3 /tmp/flatpak-cargo-generator.py Cargo.lock -o packaging/flatpak/cargo-sources.json
   ```
3. If `sdk/Cargo.toml`'s `webrtc-sys` / `livekit` pins moved, re-derive the
   prebuilt-webrtc archive URL + `sha256` (the tag is `webrtc-sys-build`'s
   `WEBRTC_TAG` constant) and update the `archive` source in `.yml.in`.
4. If the matrix-rust-sdk fork bumped its MSRV past `rust-1.97.0`, bump the
   `rust-toolchain` module's pinned version + `sha256` (published at
   `https://static.rust-lang.org/dist/rust-<ver>-x86_64-unknown-linux-gnu.tar.gz.sha256`).
5. Add a `<release version="X.Y.Z" date="YYYY-MM-DD"/>` line (newest first)
   to `io.github.surakin.Tesseract.metainfo.xml`, sourced from `CHANGES.md`.
6. `cmake --preset linux-debug -DTESSERACT_UI=qt6` to regenerate `.yml`.
7. Build, sign, export, deploy per "Distribution" above.

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
