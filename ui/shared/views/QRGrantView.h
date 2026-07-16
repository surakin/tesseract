#pragma once

// QRGrantView — shows a QR code for the MSC4108 "QR grant login" flow.
// The logged-in device generates the QR; the user's phone scans it to add
// the phone as a new device. All blocking FFI calls run on a worker thread;
// results are marshalled back to the UI thread via post_to_ui_.
//
// The check-code field is a tk::TextField — a real widget-tree child that
// owns and positions its own native edit control, visible only during the
// CheckCode state.

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/host.h"
#include "tk/image_view.h"
#include "tk/layout.h"
#include "tk/text_field.h"
#include "tk/widget.h"

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace tesseract
{
class Client;
}

namespace tesseract::views
{

class QRGrantView : public tk::Widget
{
protected:
    // host() is nullable: when null, the check-code field is simply not
    // constructed — lets tests that don't care about the native field
    // default-construct without a Host.
    QRGrantView();
    TK_WIDGET_FACTORY_FRIEND(QRGrantView)

public:
    enum class State
    {
        Loading,
        ShowQR,
        CheckCode,
        WaitingForAuth,
        Done,
        Error,
    };

    // -----------------------------------------------------------------------
    // Callback injection — wire all set_* before calling start()
    // -----------------------------------------------------------------------

    void set_post_to_ui(std::function<void(std::function<void()>)> fn)
    {
        post_to_ui_ = std::move(fn);
    }
    // Must target a single-thread mutable pool (ShellBase::run_async_mut_),
    // not the shared read pool — all grant FFI calls take the exclusive lock.
    void set_run_async(std::function<void(std::function<void()>)> fn)
    {
        run_async_ = std::move(fn);
    }
    void set_relayout(std::function<void()> fn)
    {
        relayout_ = std::move(fn);
    }
    void set_open_browser(std::function<void(const std::string&)> fn)
    {
        open_browser_ = std::move(fn);
    }
    void set_on_done(std::function<void()> fn)
    {
        on_done_ = std::move(fn);
    }
    void set_on_cancel(std::function<void()> fn)
    {
        on_cancel_ = std::move(fn);
    }

    void set_client(tesseract::Client* c);

    // Start the QR grant flow (call after all set_* above).
    void start();

    // Tear down in-flight work. Call from the host before the surface tears down.
    void shutdown();

    std::weak_ptr<bool> alive_token() const { return alive_; }

    // Shadows tk::Widget::set_visible (not virtual — same idiom as
    // tk::TextField's own shadow) so hiding the overlay also hides
    // check_input_field_'s native control. tk::Widget::set_visible does not
    // cascade to children by design; without this, cancelling out of the
    // CheckCode state would leave the native check-code field visibly
    // stuck on screen after the overlay closes.
    void set_visible(bool v);

    void on_theme_changed(const tk::Theme& t) override;

    // -----------------------------------------------------------------------
    // Widget interface
    // -----------------------------------------------------------------------

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;

private:
    void rebuild_tree_();
    void set_state_(State s);
    void join_worker_();

    // State machine
    State       state_     = State::Loading;
    std::string error_msg_;

    // QR image — created lazily in paint() after pixels arrive
    std::vector<uint8_t>       qr_pixels_;
    uint32_t                   qr_side_      = 0;
    std::unique_ptr<tk::Image> qr_image_;
    tk::CanvasFactory*         last_factory_ = nullptr;

    // Check code text field
    std::string check_code_text_;

    // Worker machinery
    tesseract::Client*    client_    = nullptr;
    std::thread           worker_;
    std::atomic<bool>     cancelled_{false};
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};

    // Injected platform hooks
    std::function<void(std::function<void()>)> post_to_ui_;
    std::function<void(std::function<void()>)> run_async_;
    std::function<void()>                      relayout_;
    std::function<void(const std::string&)>    open_browser_;
    std::function<void()>                      on_done_;
    std::function<void()>                      on_cancel_;

    // Borrowed widget pointers (owned by card_ subtree via add_child)
    tk::VBox*      card_              = nullptr;
    tk::Label*     status_lbl_        = nullptr;
    tk::ImageView* qr_view_           = nullptr;
    tk::TextField* check_input_field_ = nullptr;
    tk::Button*    confirm_btn_       = nullptr;
    tk::Button*    cancel_btn_        = nullptr;
    tk::Button*    close_btn_         = nullptr;
    tk::Button*    retry_btn_         = nullptr;
};

} // namespace tesseract::views
