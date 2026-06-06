#pragma once
#include "tk/host.h" // tk::NavKey
#include "views/GifController.h"
#include "views/MentionController.h"
#include "views/ShortcodeController.h"
#include "views/SlashCommandController.h"

#include <string>

namespace tesseract::views
{

// The composer's four autocomplete popups (GIF strip, /command, :shortcode:,
// @mention) share one text area and are mutually exclusive — at most one is
// visible at a time. These helpers centralise the priority order
// (gif > slash > shortcode > mention) and the mutual-exclusion bookkeeping so
// every shell (main window + each pop-out, on every platform) drives them
// identically instead of re-implementing the orchestration. Any controller
// pointer may be null (a shell that lacks that popup just passes nullptr).

// on_changed: run detection in priority order; the first controller that
// consumes the change wins and every other popup is hidden. Returns true if any
// popup handled the change.
inline bool dispatch_compose_text_changed(const std::string& text, int cursor,
                                          GifController* gif,
                                          SlashCommandController* slash,
                                          ShortcodeController* shortcode,
                                          MentionController* mention)
{
    bool g = gif && gif->on_text_changed(text);
    bool sl = !g && slash && slash->on_text_changed(text, cursor);
    bool sc = !g && !sl && shortcode && shortcode->on_text_changed(text, cursor);
    bool me =
        !g && !sl && !sc && mention && mention->on_text_changed(text, cursor);
    // Hide every popup that isn't the winner. Short-circuit && means a
    // lower-priority controller may not have been polled this tick, so its
    // popup could still be showing from a previous keystroke. hide() is
    // idempotent (no-op when already hidden), so hiding the non-winners
    // unconditionally keeps the "at most one visible" invariant with no special
    // cases.
    if (gif && !g)
        gif->hide();
    if (slash && !sl)
        slash->hide();
    if (shortcode && !sc)
        shortcode->hide();
    if (mention && !me)
        mention->hide();
    return g || sl || sc || me;
}

// on_submit (Enter): let the visible popup consume the keystroke. Returns true
// if a popup accepted a selection — the caller must NOT send the message. Only
// one popup is ever visible, so the polling order here is immaterial.
inline bool dispatch_compose_submit(GifController* gif,
                                    SlashCommandController* slash,
                                    ShortcodeController* shortcode,
                                    MentionController* mention)
{
    if (gif && gif->on_submit())
        return true;
    if (slash && slash->on_submit())
        return true;
    if (shortcode && shortcode->on_submit())
        return true;
    if (mention && mention->on_submit())
        return true;
    return false;
}

// popup-nav (Up/Down/Tab/Esc/ShiftTab): route to whichever popup is open. Each
// controller's on_nav returns false when it isn't visible, so a single shared
// handler can replace the per-popup reinstall dance.
inline bool dispatch_compose_nav(tk::NavKey nk, GifController* gif,
                                 SlashCommandController* slash,
                                 ShortcodeController* shortcode,
                                 MentionController* mention)
{
    return (gif && gif->on_nav(nk)) || (slash && slash->on_nav(nk)) ||
           (shortcode && shortcode->on_nav(nk)) ||
           (mention && mention->on_nav(nk));
}

} // namespace tesseract::views
