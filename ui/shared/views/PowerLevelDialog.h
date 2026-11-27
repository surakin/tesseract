#pragma once

// "Change power level" dialog — a centred modal card (dim backdrop, like
// InviteDialog / ConfirmDialog) for setting one room member's power level.
// The header shows the member (avatar, name, mxid) and the room; below it a
// combo offers the Default / Moderator / Admin presets the caller may grant
// (never above their own level) plus "Custom", which reveals a numeric field.
// Save is enabled only for a valid level that differs from the current one
// and doesn't exceed the caller's own; picking exactly the caller's own
// level shows a can't-undo warning (except for a room creator, whose level
// is unbounded). The dialog does no I/O: Save hands the level to on_save.

#include "tk/canvas.h"
#include "tk/combobox.h"
#include "tk/controls.h"
#include "tk/form_layout.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace tesseract::views
{

// Parse a custom power-level entry: surrounding whitespace is ignored, then
// an optional leading '-' and digits only, within the canonical-JSON integer
// range ±(2^53 − 1). Anything else (empty, exponent, overflow) is nullopt.
std::optional<std::int64_t> parse_power_level(std::string_view text);

class PowerLevelDialog : public tk::Widget
{
protected:
    PowerLevelDialog();
    TK_WIDGET_FACTORY_FRIEND(PowerLevelDialog)

public:
    // own_level == INT64_MAX marks a privileged room creator (room v12),
    // whose level is above every number.
    static constexpr std::int64_t kCreatorLevel = std::numeric_limits<std::int64_t>::max();

    struct Target
    {
        std::string room_id, room_name, user_id, display_name, avatar_url;
        std::int64_t current_level = 0;
        std::int64_t own_level = 0; // INT64_MAX = privileged creator
    };

    using ImageProvider = std::function<const tk::Image*(const std::string& mxc)>;
    void set_avatar_provider(ImageProvider p);

    void open(Target t);
    void close();
    bool is_open() const
    {
        return open_;
    }

    std::function<void()> on_close;
    // Fires when the dialog opens/closes or its card changes height, so the
    // shell re-queries rect accessors (hides native fields it covers).
    std::function<void()> on_layout_changed;
    std::function<void(std::string room_id, std::string user_id, std::string display_name,
                       std::int64_t level)>
        on_save;

    // ── Inspection (tests) ────────────────────────────────────────────────
    tk::ComboBox* level_combo() const
    {
        return level_combo_;
    }
    // Visible only while "Custom" is selected.
    tk::TextField* custom_field() const
    {
        return custom_field_;
    }
    tk::Button* cancel_button() const
    {
        return cancel_btn_;
    }
    tk::Button* save_button() const
    {
        return save_btn_;
    }
    // The selected preset, or the parsed custom text (nullopt when invalid).
    std::optional<std::int64_t> chosen_level() const;
    bool save_enabled() const;
    // "Can't be higher than your own power level" error.
    bool error_shown() const;
    bool same_level_warning_shown() const;
    // Acts as the Save button (no-op while Save is disabled).
    void save_for_test()
    {
        save_();
    }

    // ── tk::Widget overrides ──────────────────────────────────────────────
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint_before_children(tk::PaintCtx&) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_key_down(const tk::KeyEvent& e) override;
    void on_theme_changed(const tk::Theme& t) override;

    // Accessibility: a modal Dialog named by its title; the labels, combo,
    // field and buttons attach under it.
    tk::Role access_role() const override
    {
        return tk::Role::Dialog;
    }
    std::string access_name() const override;
    bool access_modal() const override
    {
        return open_ && visible();
    }

private:
    bool is_custom_() const;
    void refresh_();
    void save_();

    static constexpr float kCardW = 420.0f;
    static constexpr float kCardPad = 20.0f;
    static constexpr float kAvatar = 48.0f;
    static constexpr float kAvatarGap = 12.0f;
    static constexpr float kSectionGap = 16.0f;
    static constexpr float kRowGap = 8.0f;
    static constexpr float kFieldH = 32.0f;
    static constexpr float kBtnH = 36.0f;
    static constexpr float kBtnMinW = 88.0f;
    static constexpr float kBtnGap = 8.0f;

    bool open_ = false;
    bool pending_focus_ = false;
    bool press_backdrop_ = false;
    Target target_;
    ImageProvider avatar_provider_;

    // Children — owned via add_child, raw pointers borrowed back.
    tk::Label* title_label_ = nullptr;
    tk::Label* name_label_ = nullptr;
    tk::Label* mxid_label_ = nullptr;
    tk::Label* room_label_ = nullptr;
    tk::FormLayout* form_ = nullptr;
    tk::ComboBox* level_combo_ = nullptr;
    tk::TextField* custom_field_ = nullptr;
    tk::Label* error_label_ = nullptr;
    tk::Label* warning_label_ = nullptr;
    tk::Button* cancel_btn_ = nullptr;
    tk::Button* save_btn_ = nullptr;

    // World-space rects, updated each arrange().
    tk::Rect card_rect_{};
    tk::Rect avatar_rect_{};
};

} // namespace tesseract::views
