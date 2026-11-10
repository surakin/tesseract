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
| `feat(...)` | User-visible change (section decided in step 4, not by the prefix) |
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
- **Translate jargon — except a `feat(...)` bullet's own MSC number.** No
  library/protocol/internal names — no `ruma`, FFI, SDK internals,
  class/function/file names. Describe what the user actually sees or
  experiences instead.
  **Exception:** if a `feat(...)` bullet's source text names a specific MSC
  (Matrix Spec Change) number, that number is REQUIRED in the rewritten line,
  whichever section step 4 puts it in — e.g. "Added inline link preview cards
  for shared URLs (MSC4095)." MSC numbers are the one piece of protocol jargon
  release notes must surface, so spec-literate users and other Matrix clients
  can identify interoperable functionality. Keep only the MSC number from that
  bullet — still strip `ruma`, FFI, and other implementation details from the
  same text. The exception follows the source prefix, not the output section:
  a `feat(...)` bullet keeps its MSC number even when it lands under
  Improvements or Fixes, and a `fix(...)`/`perf(...)`/`refactor(...)` bullet
  never gets one, even if its source text happens to mention one.
- **One item per feature.** Every bullet about the same user-facing feature
  becomes a single release-note item, always — not one or two, one. This
  covers repeated commits on one capability (five image-pack commits) and
  broad efforts split across many Summary bullets (a dozen `feat(a11y)`
  bullets for keyboard access and screen readers become one item). A follow-up
  that only polishes or completes a feature in this same batch (a key binding,
  a Backspace fix, a sync detail) folds into that feature's item as a clause,
  or is dropped if it adds nothing a user would notice. Put the merged item in
  the section its main change belongs to (step 4). Related-but-separate
  features (emoji skin tones vs. a newer Emoji version) stay separate items.
- **Write like a person, not a press release.** The notes should read as if
  the developer typed them. Concretely:
  - Plain, direct sentences. Say what changed and stop. Short is fine.
  - No hype or filler: "big/major/significant improvements", "much
    broader", "fully", "seamlessly", "enhanced", "robust", "comprehensive",
    "now supports", "continued work on".
  - Don't lead with a summary label and a colon followed by an exhaustive
    list ("Much broader keyboard access: X, Y, Z, W..."). Name the few
    things a user would look for, not every surface a commit touched.
  - Don't stack the same sentence shape ("X can now..., Y can now...,
    Z can now..."). Vary it, or cut it.
  - "You" is fine ("you can now...", "tell you when..."); passive
    constructions and "the user" are not.
  - Avoid em-dash asides and triplets for rhythm's sake.
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

Pick the section from what the change means to a user of the **previous
release**, not from the commit prefix. The prefix records how the developer
thought about the code; it is only a starting guess. Ask of every survivor:

- **New Features** — the user can now do something they could not do at all
  in the last release: a new action, screen, setting, or protocol capability.
- **Improvements** — something they could already do now works better,
  faster, or more clearly: a redesigned flow, extra detail on something that
  was already shown, a shortcut for a step they already did by hand, polish.
- **Fixes** — something that shipped in an earlier release behaved wrongly
  (broke, crashed, lost data, showed the wrong thing) and now doesn't.

Default mapping, to override whenever the test above says otherwise:
`feat` → New Features, `perf` → Improvements, `fix`/qualifying `refactor` →
Fixes. Common overrides:

- A `feat(...)` that makes existing functionality *keep working* in a case
  where it used to break (e.g. "pop-outs keep working after switching
  accounts") is a **Fix** — the pop-outs already existed; they were broken.
- A `feat(...)` that redesigns or consolidates an existing flow (e.g. "one
  dialog now handles all of encryption", when setup and verification already
  existed) is an **Improvement**.
- A `feat(...)` that adds detail to something already displayed (e.g. "kick
  lines show the reason") or automates a step users already did by hand
  (e.g. a "Restart now" button after a setting that already needed a
  restart) is an **Improvement**.
- A `feat(...)` that adds a genuinely new capability *and* incidentally fixes
  a symptom stays a **New Feature**; the fixed symptom can be a clause in the
  same line rather than a separate Fixes bullet.

The "Fixes require a previously-released feature" rule in step 2 still
applies to anything that lands under Fixes, including a `feat(...)` moved
there: the broken behavior must have shipped in a prior release.

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

## 6. Deliver the Notes — Paste the Verified File Into Your Reply

**The user cannot see tool output.** Output from `cat`, `Read` or any other
tool call is shown to you, not to the user. The only thing the user reads is
your reply text. A reply that says "release notes written to `<path>`" and
nothing else means the user never sees the notes. Always put the full notes
in your final reply.

Retyping the notes from memory brings back the `&amp;` leak that step 5
fixed. So copy them from the verified file, don't compose them again:

1. If the caller (user or orchestrating agent) gave you a destination path,
   write the final verified text there too.
2. `cat` the verified file as your last tool call, so its exact text is in
   front of you.
3. In your final reply, paste that output unchanged inside one fenced
   ```` ```markdown ```` code block. The fence makes it easy to copy and stops
   the chat from rendering or escaping it. Change nothing: no rewording, no
   reordering, no extra bullets.
4. Before sending, compare your pasted block line by line with the `cat`
   output. Literal `&`, `'`, `<` and `>` must stay literal; if you see
   `&amp;`, `&#39;`, `&lt;` or `&gt;` in your reply, fix it to match the file.
5. Outside the code block, add at most a short summary: the file path, the
   bullet count per section, and anything notable you dropped or merged.

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
- feat(accounts): pop-out room windows keep working for the account that
  opened them after switching accounts (previously they sent as the newly
  active account)
- feat(settings): "Restart now" relaunches the app after a language change
```

Release notes:

```markdown
### New Features
- Added the ability to create polls, with up to 20 options, from the compose bar.

### Improvements
- Added consistent tooltips throughout the app.
- After changing the language, "Restart now" relaunches the app in the new language.

### Fixes
- Switching themes now recolors every dialog and text field. Some settings dialogs kept the old colors, and the quick switcher showed black text on a dark background.
- Pop-out room windows keep working for the account that opened them after you switch accounts.
```

(`build:` and `fix(build)` are dropped outright; the superseded `perf(compose)`
attempt is dropped in favor of whatever later bullet actually fixed it. The
poll-overflow fix is dropped too — its own text says "newly-added," and the
`feat(compose)` bullet two lines above it introduced that exact poll option
list in this same unreleased batch, so no user ever saw it overflow; the new
poll feature's own bullet already covers what actually shipped. The two
trailing `feat` bullets are re-sectioned by the step 4 test: pop-outs already
existed and broke on account switch, so that is a Fix; changing the language
already worked and "Restart now" only saves a manual restart, so that is an
Improvement. Only the poll flow is something users could not do before. The
two theme bullets — `refactor(tk)` and `fix(theme)` — are one feature, theme
switching, so they become one Fixes item.)

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
- Spreading one feature over several items (e.g. five keyboard/screen-reader
  lines, or a skin-tone feature plus a separate line for its Backspace
  follow-up) — group them into one item.
- Marketing tone or label-colon-list items ("Much broader keyboard access:
  ...", "Big improvements to ...") — rewrite in plain sentences.
- Naming internal classes/files/protocols (`apply_theme()`, `ruma`) instead of describing the user-visible effect.
- Filing a bullet under New Features only because its prefix is `feat` — apply the step 4 test; a `feat` that repairs, redesigns, or adds detail to something that already shipped belongs under Fixes or Improvements.
- Dropping a `feat(...)` bullet's MSC number when the source names one — required whichever section the bullet lands in; it's the one exception to the no-protocol-jargon rule.
- Appending an MSC number to a line derived from a `fix`/`perf`/`refactor` bullet — the exception follows the `feat` source prefix, not the output section.
- Ending with only a pointer to the notes file. The user never sees tool
  output, so the reply itself must contain the notes in a `markdown` code block.
