# Publishing to winget

This directory holds the source `*.yaml.in` templates for the three winget
manifests. CMake substitutes `project(... VERSION ...)` into them and
writes the real manifests to `build/<preset>/winget/` — the build tree, not
back into this directory (unlike `PACKAGING.md`/`PKGBUILD`/the Flatpak
manifest, which *are* generated in-source). That's deliberate: `winget
validate` and `wingetcreate submit` don't filter by extension — they try to
parse **every file** in the directory you point them at as a manifest, and
refuse outright if it contains a subdirectory. A directory that also holds
this README and the `.in` templates fails for exactly that reason. Keeping
the generated output in its own clean build-tree directory is what makes
it a valid target for those tools; see the comment above the
`configure_file()` loop in the root `CMakeLists.txt`.

Edit the `*.yaml.in` templates here, never the generated files in
`build/<preset>/winget/` — those are overwritten on every configure.

The published manifests live in
[microsoft/winget-pkgs](https://github.com/microsoft/winget-pkgs), at
`manifests/t/Tesseract/Matrix/<version>/`, and are submitted via PR —
there is no upload step comparable to Partner Center.

Package identifier: `Tesseract.Matrix` (`Tesseract.Tesseract` was tried
first but the `Tesseract` identifier is already taken upstream by an
unrelated package). Only the NSIS `.exe` is submitted (see the comment at
the top of
[`Tesseract.Matrix.installer.yaml.in`](Tesseract.Matrix.installer.yaml.in)
for why the MSIX editions aren't).

> [!WARNING]
> Once a GitHub token is stored (`wingetcreate token --store`, or a token
> passed inline), `wingetcreate submit`/`wingetcreate update --submit`
> open a **real PR against the public winget-pkgs repo** the moment
> validation passes — there is no local dry-run confirmation step.
> `winget validate --manifest <dir>` is the safe, read-only way to check a
> manifest; only run `wingetcreate submit` once you actually intend to
> publish.

## One-time setup

1. Fork `microsoft/winget-pkgs` on GitHub (or let `wingetcreate --submit`
   do it on first run).
2. Install [`wingetcreate`](https://github.com/microsoft/winget-create):
   ```powershell
   winget install wingetcreate
   ```
3. Create a GitHub personal access token with the `public_repo` scope, then
   store it so `wingetcreate` doesn't prompt for it every run:
   ```powershell
   wingetcreate token --store
   ```
   Storing it means every future `submit`/`update --submit` call will
   publish for real without asking again — see the warning above.

## First submission (manual)

The manifests here are already fully written — don't use `wingetcreate
new`, since that's an interactive wizard that builds a manifest from
scratch by *asking* for publisher/license/description/tags, duplicating
what's already in
[`Tesseract.Matrix.locale.en-US.yaml.in`](Tesseract.Matrix.locale.en-US.yaml.in).
Use `wingetcreate submit` instead, pointed at the build-tree output, which
takes the existing manifest files and opens the PR directly with no
prompts. The only value the templates can't fill in is `InstallerSha256`,
since that depends on the actual built `.exe`.

1. Cut the release in the main repo (tag `vX.Y.Z`, pushed to the `github`
   remote) — same as every other packaging target, this is already wired
   to `.github/workflows/package.yml`, which builds the NSIS installer and
   attaches it to the GitHub Release.
2. Configure so the real (version-substituted) manifests exist, in
   `build/windows-release/winget/`:
   ```powershell
   cmake --preset windows-release
   ```
3. Compute the real installer hash and patch it into the *generated*
   `build/windows-release/winget/Tesseract.Matrix.installer.yaml` (not the
   `.in` template — this file gets overwritten on every configure anyway).
   Download to a scratch location outside the manifest directory so it
   doesn't end up alongside the manifests:

   ```powershell
   Invoke-WebRequest "https://github.com/surakin/tesseract/releases/download/vX.Y.Z/Tesseract-X.Y.Z-AMD64.exe" -OutFile "$env:TEMP\tesseract-installer.exe"
   $hash = (Get-FileHash "$env:TEMP\tesseract-installer.exe" -Algorithm SHA256).Hash
   (Get-Content build/windows-release/winget/Tesseract.Matrix.installer.yaml) -replace 'PLACEHOLDER_SHA256_COMPUTE_FROM_RELEASE_ASSET', $hash |
     Set-Content build/windows-release/winget/Tesseract.Matrix.installer.yaml
   ```

4. Validate first (read-only, no network):

   ```powershell
   winget validate --manifest build/windows-release/winget/
   ```

5. Only once that passes, submit for real:

   ```powershell
   wingetcreate submit build/windows-release/winget/
   ```

   This opens the PR against `winget-pkgs` directly — nothing to answer
   interactively.
6. Address any automated validation feedback the PR bot leaves. If any of
   it changes a value other than the version or the SHA256/URL (both
   already handled above), fold it back into the `.in` templates here so
   the next release stays correct without manual editing.

## Subsequent releases (automated)

Once the package exists upstream, [`.github/workflows/winget-publish.yml`](../../.github/workflows/winget-publish.yml)
handles version bumps automatically on every GitHub Release: it runs
[`vedantmgoyal2009/winget-releaser`](https://github.com/vedantmgoyal2009/winget-releaser),
which wraps `wingetcreate update --submit` — it diffs the new release's
installer URL/SHA256 into the existing upstream manifest and opens the PR,
no manual `wingetcreate` invocation needed.

Requires a `WINGET_TOKEN` repo secret: a GitHub PAT (classic, `public_repo`
scope) belonging to an account that can open PRs against `winget-pkgs` —
the default `GITHUB_TOKEN` can't, since that's a fork/PR against a
repository outside this one. See the workflow file's header comment for
the exact setup steps.

## Manual re-verification

The `.in` templates aren't valid manifests by themselves (`@PROJECT_VERSION@`
is a literal, unsubstituted string, and `@` can't start a plain YAML
scalar) — run a CMake configure first so the real manifests exist in the
build tree:

```powershell
cmake --preset windows-release
```

Then sanity-check with `winget validate` (read-only) or `winget install
--manifest` (installs locally — no PR involved either way):

```powershell
winget validate --manifest build/windows-release/winget/
winget install --manifest build/windows-release/winget/
```
