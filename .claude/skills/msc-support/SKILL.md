---
name: msc-support
description: Use when asked to update, refresh, or regenerate the README's list of supported MSCs (Matrix Spec Change proposals) or its Matrix-spec-compatibility statement — rebuilds that section from what's actually implemented in source today.
---

# MSC Support

## Overview

README has a `## Supported MSCs & spec compatibility` section listing which
Matrix Spec Change proposals Tesseract currently implements, plus a plain
statement of what a homeserver needs to support to work with it. The ground
truth for "currently supported" is Tesseract's own source tree, not
`FEATURES.md` or `CHANGES.md` — both drift: `FEATURES.md` is hand-maintained
and goes stale (it can lag behind shipped features), and `CHANGES.md`
accumulates MSC mentions for things later superseded or stabilized into the
spec proper. This skill rebuilds the section from a fresh scoped scan every
time, the same way `release-notes` rebuilds notes from `CHANGES.md` instead
of trusting a hand-maintained summary.

## When to Use

- Asked to update/refresh/regenerate the README's supported-MSC list or
  Matrix-spec-compatibility statement.
- Asked "which MSCs does Tesseract support?" — the scan in step 1 answers
  this directly even if you don't go on to edit the README.
- NOT for adding a brand-new MSC-backed feature yourself — that's normal
  implementation work; this skill only documents what's already shipped.

## 1. Scan — Never Hand-Roll This Grep

Run the bundled scanner from the repo root and capture its full output to a
scratch file — don't rely on a truncated terminal preview, some MSCs (e.g.
image-pack support) have 100+ hits:

```bash
python3 <skill-dir>/scan_mscs.py > /tmp/msc-scan.txt
```

(`<skill-dir>` is the directory this SKILL.md lives in.) Then read
`/tmp/msc-scan.txt` in full. It walks a hardcoded allow-list of Tesseract's
own source (`sdk/src`, `client/src`, `client/include`, `ui/shared`, each
platform's `ui/*/src`, `docs/BUILD.md`, `packaging/arch/PKGBUILD.in`, and
`sdk/Cargo.toml`) for `MSC####` identifiers, grouped by MSC number with every
`file:line` hit. It deliberately skips `CHANGES.md`, `FEATURES.md`,
`ROADMAP.md`, `STATUS.md`, `i18n/*.po`, and vendored/build directories —
those are historical or generated, not current-implementation signal. Do not
substitute a freehand `grep -r MSC` — it will pull in vendored third-party
code and stale docs the scanner already excludes on purpose.

## 2. Classify Each MSC

For every MSC number in the scan output:

- **Confirm it's live.** The hit should sit in a real code path (a struct
  field, an event-type string, a handler) — not a commented-out block or a
  string that's only ever referenced in a test fixture for something else.
- **Drop planned-only MSCs.** Cross-check `ROADMAP.md` and `FEATURES.md`'s
  "Not yet implemented" section. If an MSC number's *only* signal is "this is
  planned," it doesn't belong in a *supported*-MSCs list — leave it out even
  if the scan found a hit (e.g. a TODO comment naming the MSC it'll use).
- **Flag experimental/gated ones.** If the hit context names an unstable
  Cargo feature (`unstable-msc____`) or a runtime toggle the code itself
  calls out as experimental/opt-in (e.g. a `_enabled` flag, "behind
  `X_enabled`"), mark that MSC as *(experimental)* in the output rather than
  listing it as unconditionally supported.

## 3. Write One Line Per Surviving MSC

Format as a linked list item, plain language, no internal file/class/type
names — same jargon-translation rule as `release-notes`:

```markdown
- [MSC2545](https://github.com/matrix-org/matrix-spec-proposals/pull/2545) — custom emoji & sticker image packs
- [MSC4491](https://github.com/matrix-org/matrix-spec-proposals/pull/4491) — invite reasons on room/DM creation *(experimental)*
```

The MSC number **is** the PR number in `matrix-org/matrix-spec-proposals`, so
the link is mechanical — never look it up, never guess a different number.

**Merge MSCs that back the same user-facing feature into one bullet** — the
scan routinely surfaces clusters like calls (MSC4143 + MSC3401 + MSC4075 +
MSC4195 + MSC4196 + MSC4354) or extended profile fields (MSC4133 + MSC4247 +
MSC4175 + MSC4440) that are all facets of one capability. Lead with the
primary/user-visible MSC, then fold the supporting ones into the same
bullet's description with their own links, rather than emitting one bullet
per MSC number — a flat one-per-MSC list reads as ~35 disconnected items
where ~20 grouped ones read as an actual feature list. **Foundational
protocol-level MSCs (e.g. Sliding Sync, MSC3575) belong in the step-4
compatibility paragraph, not this list** — they gate the whole app, not one
feature, so a separate list bullet would be redundant with that paragraph.

## 4. Write the Spec-Compatibility Statement

One short paragraph restating the practical server requirement, matching the
wording already used in README's own `## Server requirements` section —
**not** a version-number claim (nothing in the codebase pins a single Matrix
spec version; matrix-sdk/ruma versions in `sdk/Cargo.toml` don't map to one
either). Cover: Sliding Sync is required (native, e.g. Synapse's built-in
support) — name it with its MSC3575 link here, since this is where
foundational/protocol-level MSCs belong, not the per-feature list in step 3;
login is OAuth 2.0/OIDC via Matrix Authentication Service (MAS), with legacy
username/password as a fallback for existing accounts on servers without MAS.

## 5. Assemble and Insert

Build the full section:

```markdown
## Supported MSCs & spec compatibility

<spec-compatibility paragraph from step 4>

<MSC list from step 3>
```

Insert it into `README.md` immediately after the existing
`## Server requirements` section and before `## Minimum OS requirements`. If
a `## Supported MSCs & spec compatibility` section already exists from a
prior run, replace it in place — don't duplicate the heading.

## 6. Sanity Check Before Finishing

- Every MSC number in the final list must trace back to a line in
  `/tmp/msc-scan.txt` — no invented numbers.
- No MSC appears in the final list *and* only as a "not yet implemented"
  cross-check hit (step 2 should have already dropped these).
- No version-number claim ("Matrix v1.x") snuck into the compatibility
  paragraph.

## Common Mistakes

- Sourcing the list from `FEATURES.md`/`CHANGES.md` directly instead of the
  scan — both drift from what's actually shipped.
- Inventing a Matrix spec version number because the section title mentions
  "spec compatibility" — state the server requirement instead, never a
  version.
- Dropping the *(experimental)* caveat for feature-flagged MSCs like MSC4491.
- Re-running a hand-written `grep -r MSC` instead of the bundled scanner —
  it'll surface vendored/third-party hits and stale doc mentions the
  scanner's allow-list exists to exclude.
- Reading only the truncated terminal preview of the scan instead of the
  full redirected output — high-hit-count MSCs (100+ hits) get cut off in a
  preview.
