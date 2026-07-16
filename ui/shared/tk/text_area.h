#pragma once

// tk::TextArea — a canvas-tree widget that owns and positions a native
// multi-line text area overlay, and participates fully in the tk-level
// keyboard-focus system (Tab/Shift-Tab traversal, click-to-focus) alongside
// ordinary canvas widgets. Mirrors tk::TextField (see text_field.h) for the
// single-line control; the two differ in exactly one place, called out on
// push_popup_nav() below: TextField always claims Tab/Shift-Tab for canvas
// focus traversal before any pushed handler runs, but TextArea's consumers
// (the compose bar's mention/slash/shortcode/gif popups) need to claim
// Tab/Shift-Tab themselves for suggestion-cycling, so TextArea gives its
// pushed handler stack first refusal on every NavKey and only falls back to
// canvas traversal when nothing in the stack consumes it.
//
// Reserves layout space like a plain Label (draws nothing itself — the
// native control paints on top) and positions the native control directly
// from its own arrange(), replacing the old pattern of a separate
// placeholder Label whose bounds() had to be read and reapplied by an
// external "position_overlay()"-style pass.

#include "controls.h"
#include "host.h"

#include <tesseract/mentions.h>

#include <string>
#include <vector>

namespace tk
{

class TextArea : public Label
{
protected:
    // `min_height` is the minimum vertical space to reserve (and the
    // minimum height handed to the native control) — mirrors
    // tk::TextField's constructor convention. Auto-grow above this floor
    // is the caller's responsibility (via natural_height()/
    // set_on_height_changed() below); this widget does not own a max-height
    // clamp — it just applies whatever bounds its caller computed. Host
    // comes from host() (inherited, valid from the first line of this
    // constructor's body — see widget.h), not a parameter.
    explicit TextArea(float min_height);
    TK_WIDGET_FACTORY_FRIEND(TextArea)

public:
    void set_text(std::string text);
    std::string text() const;
    void set_placeholder(std::string text);
    void set_text_color(Color c);
    void set_font_role(FontRole role);

    // Last value passed to set_visible() — matches
    // NativeTextArea::visible()'s "defaults to true, tracks the
    // hidden→visible transition" semantics some shells rely on for prefill.
    bool visible() const;

    // Natural content height for the caller's auto-grow clamp (e.g.
    // ComposeBar's [min, max] envelope), and a hook fired whenever it
    // changes so the caller can re-run that clamp and relayout.
    float natural_height() const;
    void set_on_height_changed(std::function<void(float)> cb);

    void set_on_changed(std::function<void(const std::string&)> cb);
    void set_on_submit(std::function<void()> cb);

    // Notifies whenever this area's native OS focus changes, after the
    // internal native-sync guard logic below has already run — mirrors
    // tk::TextField::set_on_focus_changed.
    void set_on_focus_changed(std::function<void(bool)> cb);

    // Cursor / selection / pill-insertion API — thin pass-throughs to the
    // wrapped NativeTextArea. See host.h's NativeTextArea for the full
    // contract of each.
    void insert_at_cursor(std::string text);
    Rect cursor_rect() const;
    void replace_range(int start, int end, std::string text);
    int cursor_byte_pos() const;
    void insert_mention(int start, int end, const std::string& user_id,
                        const std::string& display_name, bool is_room);
    void insert_emoticon(int start, int end, const std::string& shortcode,
                         const std::string& mxc_url, const Image* image);
    std::vector<tesseract::MentionSeg> composer_draft() const;
    void set_mention_colors(Color bg, Color fg);

    // Fired when Up is pressed while the area is empty and no popup is
    // open — used to load the last own message for editing. See
    // NativeTextArea::set_on_edit_last.
    void set_on_edit_last(std::function<bool()> cb);

    void set_on_image_paste(NativeTextArea::ImagePasteHandler cb);
    void set_image_resolver(std::function<const Image*(const std::string& uri)> cb);

    // Stackable navigation-key handler chain: any number of independent
    // installers can layer popup-list navigation onto this area without
    // clobbering each other. Tried most-recently-pushed first, for every
    // NavKey INCLUDING Tab/ShiftTab — unlike tk::TextField, which always
    // claims Tab/ShiftTab for canvas traversal before consulting this
    // stack. Falls back to Host::advance_focus() only once nothing in the
    // stack consumes the key, so composer popups (which claim Tab/ShiftTab
    // for suggestion-cycling) keep working, while plain areas with no
    // pushed handler (the topic-edit / paste-catcher uses) still get
    // ordinary Tab-advances-focus behavior.
    void push_popup_nav(std::function<bool(NavKey)> cb);
    void pop_popup_nav();

    void set_enabled(bool enabled) override;

    // Shadows Widget::set_visible (not virtual) so this widget's own
    // visible_ flag and the native control's OS-level visibility always
    // move together — mirrors tk::TextField::set_visible, including its
    // same-value native-forward guard (see that header's comment for the
    // full rationale). Must compare against Widget::visible() specifically,
    // NOT this class's own visible() override just below: that one forwards
    // to the native area_->visible() and returns false unconditionally when
    // area_ is null (e.g. a headless test Host), so using it here would
    // misclassify every set_visible(false) on a null-backed TextArea as
    // "redundant" and skip Widget::set_visible() itself, permanently
    // desyncing visible_ from reality.
    void set_visible(bool v);

    // Programmatic focus, routed through Host::request_focus()/
    // clear_focus() so a caller-driven focus change stays in sync with
    // tk-level focus tracking exactly like a Tab-driven one.
    void set_focused(bool focused);

    void arrange(LayoutCtx& ctx, Rect bounds) override;

    bool focusable() const override
    {
        return enabled_ && area_ != nullptr;
    }
    bool holds_native_focus() const override
    {
        return area_ != nullptr;
    }

    // See tk::TextField::on_focus_gained for why syncing_from_native_
    // guards against redundant set_focused() calls back into a native
    // control that just told us it changed on its own.
    void on_focus_gained() override
    {
        if (area_ && !syncing_from_native_) area_->set_focused(true);
        if (on_focus_changed_cb_) on_focus_changed_cb_(true);
    }
    void on_focus_lost() override
    {
        if (area_ && !syncing_from_native_) area_->set_focused(false);
        if (on_focus_changed_cb_) on_focus_changed_cb_(false);
    }

private:
    std::unique_ptr<NativeTextArea> area_;
    float min_height_;
    bool syncing_from_native_ = false;
    std::function<void(bool)> on_focus_changed_cb_;
    std::vector<std::function<bool(NavKey)>> nav_handlers_;
};

} // namespace tk
