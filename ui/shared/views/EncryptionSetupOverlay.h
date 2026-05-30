#pragma once

#include "tk/widget.h"
#include "tk/canvas.h"

#include <functional>
#include <string>

namespace tesseract::views
{

class EncryptionSetupOverlay : public tk::Widget
{
public:
    enum class Mode { Fresh, Recover };
    enum class Step { Intro, ChooseMethod, EnterKey, Progress, ShowKey, Done };

    explicit EncryptionSetupOverlay(Mode mode);

    // ── Callbacks wired by ShellBase ──────────────────────────────────────
    std::function<void()>              on_close;
    std::function<void()>              on_continue_intro; // unused internally; for test compat
    std::function<void(std::string)>   on_enable_recovery; // passphrase or ""
    std::function<void(std::string)>   on_recover;         // key or passphrase
    std::function<void()>              on_request_sas;
    std::function<void(std::string)>   on_copy_to_clipboard;

    // ── Host hooks (NativeTextField rects) ───────────────────────────────
    std::function<tk::Rect()>      passphrase_field_rect;
    std::function<bool()>          passphrase_field_visible;
    std::function<tk::Rect()>      key_field_rect;
    std::function<bool()>          key_field_visible;
    std::function<std::string()>   get_passphrase;
    std::function<std::string()>   get_key_input;

    // ── Driven by ShellBase after on_enable_recovery/on_recover fires ────
    void advance_progress(uint8_t step,
                          const std::string& recovery_key,
                          uint32_t backed_up,
                          uint32_t total);

    // ── Accessors ─────────────────────────────────────────────────────────
    Step        step()          const { return step_; }
    Mode        mode()          const { return mode_; }
    std::string recovery_key()  const { return recovery_key_; }
    std::string error_msg()     const { return error_msg_; }
    std::string progress_label()const { return progress_label_; }

    bool     passphrase_field_rect_visible() const;
    tk::Rect passphrase_field_rect_value()   const;
    bool     key_field_rect_visible()        const;
    tk::Rect key_field_rect_value()          const;

    // ── tk::Widget interface ──────────────────────────────────────────────
    tk::Size measure(tk::LayoutCtx& ctx, tk::Size avail) override;
    void     arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override;
    void     paint(tk::PaintCtx& ctx) override;
    bool     on_pointer_down(tk::Point world) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;

    // ── Test helpers ──────────────────────────────────────────────────────
    void simulate_primary_action();
    void simulate_skip();
    void simulate_select_passphrase_mode();
    void simulate_check_key_saved();
    void simulate_sas_link();
    void set_passphrase_input(std::string v) { passphrase_input_ = std::move(v); }
    void set_key_input(std::string v)        { key_input_ = std::move(v); }

private:
    void     advance_step_(Step next);
    void     fire_primary_();
    tk::Rect card_bounds() const;
    void     update_progress_label_(uint8_t step, uint32_t backed_up, uint32_t total);

    Mode        mode_;
    Step        step_            = Step::Intro;
    std::string recovery_key_;
    std::string error_msg_;
    std::string progress_label_;
    std::string passphrase_input_;
    std::string key_input_;
    uint32_t    backed_up_       = 0;
    uint32_t    total_           = 0;
    bool        key_saved_checked_  = false;
    bool        passphrase_mode_    = false;
};

} // namespace tesseract::views
