---
name: release-notes
description: Use when asked to draft release notes, a "what's new" summary, or a changelog announcement for Tesseract from CHANGES.md — converts the most recent version's Summary bullets into a curated, user-facing list.
---

# Release Notes

## Overview

CHANGES.md's `### Summary` bullets are developer-facing: conventional-commit
prefixes (`feat(scope):`, `fix(scope):`, ...), internal jargon, and build/CI/test
noise mixed in with real user-facing changes. Release notes are the curated,
plain-language subset of that list, grouped for an end user who has never heard
of the codebase.

## When to Use

- Asked for release notes, a changelog announcement, or "what's new" text for
  the current (top-most) Tesseract version.
- NOT for writing new CHANGES.md entries themselves — that's a direct edit to
  CHANGES.md, not this skill.
- NOT for older/tagged versions unless the user names one explicitly — default
  to the newest section.

## 1. Find the Source Bullets

CHANGES.md is newest-first. The source is **only**:

- The first `## v...` header in the file (whether `— unreleased` or dated).
- Its `### Summary` subsection — the bullets between `### Summary` and the
  next `### Details`.

Stop there. Never pull from `### Details` (per-day prose) or from any older
`## v...` section.

## 2. Filter

| Always drop | Why |
|---|---|
| `build:`, `build(...)`, `ci:`, `chore:`, `chore(...)` | Build/tooling, invisible to users |
| `fix(build)`, `fix(ci)`, `fix(cpack)` | Packaging/CI fixes, not user-facing |
| `test(...)` | Test-only changes |
| `docs(...)` | Repo documentation, not in-app |
| Any bullet whose own text flags itself as superseded (e.g. "first (superseded) attempt at ...") | Superseded by a later bullet in the same list |
| `refactor(...)` / `perf(...)` with no user-visible symptom described in the bullet itself | Pure internal cleanup |

| Keep as candidates | Why |
|---|---|
| `feat(...)` | New capability |
| `fix(...)` not in the drop list above, **and** fixing a previously-released feature (see below) | User-visible bug fix |
| `perf(...)` describing a perceptible speed/responsiveness change | User-perceptible |
| `refactor(...)` **only if** its own text names a resulting user-visible fix (e.g. "...fixing several fields the old list missed entirely") | Extract the visible effect; drop the mechanism |

When in doubt whether a survivor is "important," ask: would an end user who
never reads commit logs notice or care about this? If the bullet's only
content is an internal component, file, or class name with no described
symptom, drop it.

**Fixes require a previously-released feature.** Every bullet you're
filtering comes from one unreleased version's Summary list — nothing in it
has shipped yet. A `fix(...)` bullet only belongs under Fixes if the behavior
it corrects was already live in a *prior* release. So before keeping a
`fix(...)`, check whether this *same* Summary list also contains a
`feat(...)` (or `refactor(...)`) bullet introducing the component/feature the
fix touches (matching scope, same subject). If it does, the fix is patching
code a user never had — drop it outright; the feature bullet already
describes what actually shipped, and a separate "fixed" callout would
describe a bug that, from the user's point of view, never existed. A strong
textual signal for this: the fix's own wording says "newly added," "just
introduced," or otherwise flags the broken thing as new. Only keep a
`fix(...)` bullet when nothing earlier in this same list first introduced
what it's correcting. If a fix's text names multiple affected surfaces and
only some trace back to a `feat`/`refactor` bullet in this list, keep it but
narrow the rewritten note to the surface(s) that predate this version.

## 3. Rewrite Each Survivor

- **Strip the prefix.** No `type(scope):` tag may appear in the output, ever.
- **Translate jargon.** No library/protocol/internal names — no MSC numbers,
  `ruma`, FFI, SDK internals, class/function/file names. Describe what the
  user actually sees or experiences instead.
- **Merge duplicates.** Several bullets about the same user-facing capability
  (e.g. five separate image-pack commits) become one or two release-note
  lines, not five.
- **Surface the platform.** If the original scope names a platform
  (`windows`/`macos`/`linux`), prefix the rewritten line with `Windows:`,
  `macOS:`, or `Linux:`. Drop toolkit-only scopes (`qt`/`gtk`) unless the
  behavior itself is genuinely platform-specific and worth calling out that way.
- **Plain characters only, never HTML-escaped.** Write `&` and `'` as literal
  characters. This includes literal names that contain `&`, e.g. the
  "Emojis & Stickers" tab — write it as `Emojis & Stickers`, never
  `Emojis &amp; Stickers`. The output is plain markdown text, not an HTML
  document; there is nothing to escape.
- **No version title.** Release notes get pasted under an existing heading
  (a GitHub release title, an announcement subject line). Start directly with
  category sections — no `## vX.Y.Z` line.

## 4. Group the Output

Only emit headers that have content:

```markdown
### New Features
- ...

### Improvements
- ...

### Fixes
- ...
```

- **New Features** — `feat`-derived bullets.
- **Improvements** — `perf`-derived and UX-polish bullets (things that were
  already possible but now feel better/faster).
- **Fixes** — `fix`-derived and qualifying `refactor`-derived bullets.

## 5. Before You Finish — Run the Verifier, Don't Hand-Write the Check

Visually re-reading your own draft does not catch a leaked `&amp;`. Worse,
freehand shell commands don't either: models reliably mistype a literal `&`
while typing a check for it — every hand-written grep tried here typed
`&amp;#`/`&amp;lt;`/`&amp;gt;` instead of `&#`/`&lt;`/`&gt;`, so the check
silently matched nothing while the real leak sailed through. Do not write
your own grep or sed for this. Instead:

1. Write your draft to a scratch file, e.g. `/tmp/release-notes-draft.md`.
2. Run the bundled verifier — it fixes entity leaks automatically and flags
   leftover tags, so you never have to type a literal `&` yourself:

   ```bash
   python3 <skill-dir>/verify.py /tmp/release-notes-draft.md
   ```

   (`<skill-dir>` is the directory this SKILL.md lives in.)
3. Read the script's output. If it reports leftover conventional-commit
   tags, fix those lines by hand and re-run the script until it reports none.
   Entity leaks are already fixed for you in the file.

Do not skip this because the draft "looks clean" — it looked clean in
testing too, and still had three leaked `&amp;`s that both a visual read and
a hand-written grep missed.

Separately, confirm every bullet traces back to something in the Summary
section — no invented content.

## 6. Deliver the File — Do Not Retype the Notes Into Your Answer

**This step matters as much as step 5 and is not optional.** In every test of
this skill, the verified file on disk was clean, and the leak still came
back — because the agent then retyped the notes into its own chat response,
and composing free-form chat prose re-triggers the same auto-escaping habit
that step 5 exists to fix. The file was correct; the sentence describing the
file was not.

So: the deliverable is the verified file, not a paraphrase of it.

- If the caller (user or orchestrating agent) gave you a destination path,
  write the final verified text there.
- To show the notes in your response, do not compose them from memory —
  relay the file's exact bytes via a tool call (e.g. `cat <file>`, or a
  `Read` of it) and treat that tool output as the content, rather than typing
  your own rendition of it into your final answer.
- If your harness requires you to produce a final natural-language message,
  keep that message to a short pointer ("release notes written to `<path>`,
  N bullets") and let the tool-relayed content carry the actual text —
  never re-key the release notes text by hand at that point.

## Example

Source (`### Summary`, abridged):

```
- refactor(tk): replace the hand-maintained per-shell native-field theming list
  with a generic `tk::Widget::apply_theme()` tree traversal, fixing several
  fields the old list missed entirely (Qt6's `SettingsWidget`/`JoinRoomDialog`
  never re-theming)
- feat(tk): add a generic `tk::Host`-owned tooltip system ... migrate all 8
  hand-rolled hover/tooltip sites onto it
- fix(theme): sync every Qt6 native text field's color on theme change instead
  of just 2 of 13, fixing black-on-dark text in the quick switcher
- feat(compose): add a poll-creation flow (question + up to 20 options) to
  the compose bar
- fix(compose): fix the newly-added poll option list overflowing its card on
  narrow windows
- build: enable CMake unity builds (`TESSERACT_UNITY_BUILD`, default ON)
- perf(compose): first (superseded) attempt at the slow local echo
- fix(build): exclude CG test surface files from the unity build to fix
  `macos-appkit-x86_64-release`
```

Release notes:

```markdown
### New Features
- Added the ability to create polls, with up to 20 options, from the compose bar.

### Improvements
- Added consistent tooltips throughout the app.

### Fixes
- Fixed several settings dialogs not updating their colors when switching themes.
- Fixed unreadable (black-on-dark) text in the quick switcher and other search fields.
```

(`build:` and `fix(build)` are dropped outright; the superseded `perf(compose)`
attempt is dropped in favor of whatever later bullet actually fixed it. The
poll-overflow fix is dropped too — its own text says "newly-added," and the
`feat(compose)` bullet two lines above it introduced that exact poll option
list in this same unreleased batch, so no user ever saw it overflow; the new
poll feature's own bullet already covers what actually shipped.)

## Common Mistakes

- Leaving a `fix(...)`/`feat(...)` tag in the output — the prefix must never survive.
- Keeping `build`/`ci`/`test`/`docs`/`cpack` bullets because they happen to say "fix".
- Escaping `&` as `&amp;` — this is plain markdown output, not HTML.
- Pulling from `### Details` or an older tagged `## v...` section instead of the top Summary.
- Keeping a superseded attempt alongside the bullet that superseded it.
- Listing a fix for a bug that only ever existed in a feature this same
  unreleased batch introduced — nothing shipped was ever broken, so there's
  nothing to announce as fixed. Check for a matching `feat`/`refactor` bullet
  in the same list before keeping any `fix(...)`.
- Inventing an empty category header just to show all three sections.
- Naming internal classes/files/protocols (`apply_theme()`, MSC2545, `ruma`) instead of describing the user-visible effect.
