# Publishing to Flathub

This directory holds the source-of-truth Flatpak manifest,
[`io.github.surakin.Tesseract.yml.in`](io.github.surakin.Tesseract.yml.in)
(CMake-generates the tracked `io.github.surakin.Tesseract.yml` from it,
substituting the project version into the `tesseract` module's git `tag:` —
same pattern as `packaging/arch/PKGBUILD.in` → `PKGBUILD`), plus its
supporting files:

- [`io.github.surakin.Tesseract.metainfo.xml`](io.github.surakin.Tesseract.metainfo.xml) —
  AppStream metadata (description, screenshots, releases).
- [`cargo-sources.json`](cargo-sources.json) — generated offline-vendoring
  manifest for every Rust dependency, produced from the root `Cargo.lock` by
  [`flatpak-cargo-generator.py`](https://github.com/flatpak/flatpak-builder-tools/tree/master/cargo)
  (Flathub builds have no network access, so every crate download has to be
  pre-declared as a pinned source).

None of this is built or published from this repo directly — same
relationship as [`packaging/arch/PKGBUILD`](../arch/PKGBUILD) vs.
[`packaging/arch/aur/`](../arch/aur/): this manifest is dev-testable locally
with `flatpak-builder`, but the actual Flathub-published copy lives in a
separate `flathub/io.github.surakin.Tesseract` repository (a Flathub org
repo, created via their submission process).

## Why the manifest looks the way it does

Three build-system obstacles had to be worked around to make an **offline**
build (Flathub's hard requirement) succeed:

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

`ui/linux-qt/CMakeLists.txt` also unconditionally renames the installed
binary/desktop-file/icon from `tesseract` to `tesseract-matrix` (to coexist
with the unrelated Tesseract OCR engine, which owns `/usr/bin/tesseract` on
most distros). The manifest's `command:` is `tesseract-matrix` to match, and
a `post-install` step derives the app-id-named
`io.github.surakin.Tesseract.desktop`/`.svg` Flathub requires from the
renamed artifacts CMake's own install step already produced.

Camera access uses `--device=all` — Flatpak has no Camera-specific device
class, and Tesseract talks to `/dev/videoN` directly via GStreamer's
`v4l2src` (no portal). This is the same limitation every other raw-V4L2
Flatpak app hits (e.g. Cheese); expect Flathub reviewers to ask for a short
justification in the submission PR, not a rejection.

## One-time setup

1. Create a Flathub/GitHub account if you don't have one, and read Flathub's
   current [app submission docs](https://docs.flathub.org/docs/for-app-authors/submission)
   — the process for requesting a new app-id has changed over time, so don't
   assume an old workflow is still current.
2. Request the `io.github.surakin.Tesseract` app-id and get a
   `flathub/io.github.surakin.Tesseract` repository created per that process.
3. Clone it locally:
   ```bash
   git clone git@github.com:flathub/io.github.surakin.Tesseract.git
   ```
4. Install the build tooling:
   ```bash
   flatpak install flathub org.kde.Platform//6.9 org.kde.Sdk//6.9 \
       org.freedesktop.Sdk.Extension.golang
   sudo apt install flatpak-builder    # or your distro's equivalent
   ```
   No `rust-stable` SDK extension is needed: `org.kde.Sdk//6.9`'s own
   declared companion branch for it (`24.08`) only ships rustc 1.89.0, older
   than `matrix-sdk`'s MSRV of 1.93, and there's no way to pin a newer
   extension branch for an ID the SDK's own wildcard extension point already
   claims (an `add-build-extensions` override to `25.08` was tried and
   silently ignored by flatpak-builder). Instead, the manifest's
   `rust-toolchain` module vendors Rust's own official prebuilt
   rustc/cargo/std tarball as a pinned, checksummed source — the same
   pattern used for the prebuilt webrtc archive — and the top-level
   `cleanup:` list strips it back out of the final shipped app, since it's a
   build-time-only tool. If a future `matrix-sdk` bump raises the MSRV
   again, bump the pinned version in the `rust-toolchain` module (and its
   `sha256`, published alongside every release at
   `https://static.rust-lang.org/dist/rust-<ver>-x86_64-unknown-linux-gnu.tar.gz.sha256`).

## Publishing a new release

1. Cut the release in the main repo, same as any other packaged variant:
   ```bash
   git tag vX.Y.Z
   git push github vX.Y.Z
   ```
2. Regenerate `cargo-sources.json` from the tagged `Cargo.lock` and commit it
   in *this* repo:
   ```bash
   curl -sL -o flatpak-cargo-generator.py \
     https://raw.githubusercontent.com/flatpak/flatpak-builder-tools/master/cargo/flatpak-cargo-generator.py
   python3 flatpak-cargo-generator.py Cargo.lock -o packaging/flatpak/cargo-sources.json
   git add packaging/flatpak/cargo-sources.json
   git commit -m "flatpak: regenerate cargo-sources.json for vX.Y.Z"
   ```
3. If `sdk/Cargo.toml`'s `webrtc-sys`/`livekit` pins moved, re-derive the
   webrtc archive URL/checksum (the release tag is `webrtc-sys-build`'s
   `WEBRTC_TAG` constant — check its source for the current value) and update
   the `archive` source's `url`/`sha256` in the manifest.
4. `io.github.surakin.Tesseract.yml` is generated from `.yml.in` by CMake
   (its `tesseract` module's `tag:` is substituted from `project(... VERSION
   ...)` in the root `CMakeLists.txt`) — run a CMake configure (any preset)
   so it's regenerated for the version just tagged, same as
   `packaging/arch/PKGBUILD`. Then copy `io.github.surakin.Tesseract.yml`,
   `.metainfo.xml`, and `cargo-sources.json` into your
   `flathub/io.github.surakin.Tesseract` clone, and add a
   `<release version="X.Y.Z" date="YYYY-MM-DD"/>` entry to the metainfo
   (sourced from `CHANGES.md`'s `## vX.Y.Z — YYYY-MM-DD` header).
5. Build and smoke-test locally, in a clean state:
   ```bash
   flatpak-builder --user --install --force-clean --repo=test-repo \
       build-dir io.github.surakin.Tesseract.yml
   flatpak run io.github.surakin.Tesseract
   ```
   Exercise login, sending a message, camera, screen share, notifications,
   the tray icon, and `/location` — these are exactly the sandboxed
   capabilities this manifest's `finish-args` were scoped around.
6. Lint:
   ```bash
   appstreamcli validate io.github.surakin.Tesseract.metainfo.xml
   desktop-file-validate build-dir/files/share/applications/io.github.surakin.Tesseract.desktop
   ```
   Address anything flagged.
7. Commit and push to the Flathub repo:
   ```bash
   git add io.github.surakin.Tesseract.yml io.github.surakin.Tesseract.metainfo.xml cargo-sources.json
   git commit -m "Update to X.Y.Z"
   git push
   ```
   Flathub's own CI (buildbot) rebuilds and publishes automatically once the
   app is established; the very first submission goes through manual review
   instead.

For a packaging-only fix (no version bump), skip steps 1–2 and just update
the manifest itself, then repeat steps 5–7.
