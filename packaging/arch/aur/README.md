# Publishing to the AUR

This directory holds the hand-maintained, AUR-ready `PKGBUILD`s for the two
published packages:

- [`tesseract-matrix/PKGBUILD`](tesseract-matrix/PKGBUILD) — tracks tagged
  releases (`v*` tags), source = the GitHub tag tarball.
- [`tesseract-matrix-git/PKGBUILD`](tesseract-matrix-git/PKGBUILD) — tracks
  `main` HEAD, source = git, `pkgver()` derived via `git describe`.

Neither is CMake-generated: `pkgver`/`sha256sums`/`pkgrel` get bumped by hand
at release-cut time, same as any other AUR package. They are **not** the
same file as [`../PKGBUILD`](../PKGBUILD) — that one is a local dev-build
convenience copy that builds straight from the working tree and is never
published anywhere.

Neither package's `check()` runs the test suite (see the comment in
`tesseract-matrix/PKGBUILD`): the project's own release CI never runs
`ctest` as part of packaging either, since parts of the Catch2 suite need a
display (Qt `QGuiApplication`, no QPA fallback) and the real OS keychain,
neither of which exists in a clean build chroot.

## Build configuration

Both PKGBUILDs' `build()` pass extra `cmake` flags beyond `-DTESSERACT_UI=qt6`:

- `-DTESSERACT_ENABLE_HARDENING=OFF` — makepkg already injects Arch's
  hardening `CFLAGS`/`CXXFLAGS`/`LDFLAGS`; the project's own
  `cmake/Hardening.cmake` pass would double-inject and conflict with them
  (`_FORTIFY_SOURCE=2` vs Arch's `=3`).
- `-DTESSERACT_AUR_PACKAGE=tesseract-matrix` (stable package only) — makes
  the in-app update checker query the AUR RPC API for a newer `pkgver`
  instead of GitHub Releases, and pairs with `-DTESSERACT_GITHUB_REPO=`
  (empty) to disable the GitHub checker. `tesseract-matrix-git` sets
  neither, since a `-git` package has no meaningful "newer version" to
  point at.

## One-time setup

1. Create an AUR account at https://aur.archlinux.org and add an SSH public
   key under *My Account*.
2. Clone the (initially empty) AUR repos — AUR creates them on first push,
   as long as the name is free:
   ```bash
   git clone ssh://aur@aur.archlinux.org/tesseract-matrix.git
   git clone ssh://aur@aur.archlinux.org/tesseract-matrix-git.git
   ```
3. Install `devtools` (for clean-chroot builds) and `namcap` (for linting):
   ```bash
   sudo pacman -S devtools namcap
   ```

## Publishing `tesseract-matrix` (stable)

1. Cut the release in the main repo, same as any other release:
   ```bash
   git tag vX.Y.Z
   git push github vX.Y.Z
   ```
   This is already wired to `.github/workflows/package.yml`, which builds
   every platform's installer and creates the GitHub Release. GitHub always
   serves a source tarball for the tag regardless, at
   `https://github.com/surakin/tesseract/archive/refs/tags/vX.Y.Z.tar.gz`.
2. Compute the real checksum:
   ```bash
   curl -sL "https://github.com/surakin/tesseract/archive/refs/tags/vX.Y.Z.tar.gz" | sha256sum
   ```
3. In your `tesseract-matrix` AUR clone, copy in
   `tesseract-matrix/PKGBUILD` from this repo, then edit `pkgver` to `X.Y.Z`,
   `sha256sums` to the value from step 2, and reset `pkgrel=1`.
4. Build and verify in a **clean chroot** (not your regular system — this is
   the only reliable way to catch a missing `makedepends`):
   ```bash
   extra-x86_64-build
   ```
5. Lint:
   ```bash
   namcap PKGBUILD
   namcap tesseract-matrix-X.Y.Z-1-x86_64.pkg.tar.zst
   ```
   Address anything it flags.
6. Regenerate `.SRCINFO`, commit, and push:
   ```bash
   makepkg --printsrcinfo > .SRCINFO
   git add PKGBUILD .SRCINFO
   git commit -m "Update to X.Y.Z"
   git push
   ```

For a packaging-only fix (no version bump), just bump `pkgrel` instead and
repeat steps 4–6.

## Publishing `tesseract-matrix-git`

The `pkgver()` function tracks `main` automatically, so there's usually
nothing to bump — AUR users' own `makepkg -su`/AUR helper re-derives
`pkgver` from `git describe` against the latest commit at their own build
time. You only need to touch this package when the *packaging* itself
changes (e.g. a new runtime dependency):

1. Copy the updated `tesseract-matrix-git/PKGBUILD` from this repo into your
   AUR clone, bump `pkgrel`.
2. Verify in a clean chroot and lint with `namcap`, same as above (steps 4–5
   in the stable package's procedure).
3. Regenerate `.SRCINFO`, commit, and push (step 6 above).
