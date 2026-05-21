#pragma once

#include "tk/canvas.h"
#include "tk/widget.h"

#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

class AccountSection : public tk::Widget
{
public:
    AccountSection();
    ~AccountSection() override = default;

    // ----- Content ----------------------------------------------------------

    void set_display_name(std::string name);
    void set_user_id(std::string user_id);
    void set_avatar_url(std::string mxc_url);

    using ImageProvider =
        std::function<const tk::Image*(const std::string& mxc)>;
    void set_image_provider(ImageProvider provider);

    // ----- Editing capabilities (gated by ServerInfo) -----------------------

    void set_editable(bool editable);
    void set_avatar_editable(bool editable);

    // ----- Busy / error state -----------------------------------------------

    void set_name_busy(bool busy);
    void set_name_error(std::string error);

    void set_avatar_busy(bool busy);
    void set_avatar_error(std::string error);

    // ----- NativeTextField overlay rect -------------------------------------

    tk::Rect name_field_rect() const;

    // ----- Callbacks (wired by the shell) -----------------------------------

    std::function<void()> on_avatar_upload_clicked;
    std::function<void()> on_avatar_remove_clicked;

    // ----- tk::Widget overrides ---------------------------------------------

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

    bool on_pointer_down(tk::Point local) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;

private:
    void invalidate_text();

    bool in_disc(tk::Point local) const;
    bool in_remove_chip(tk::Point local) const;
    tk::Point disc_centre() const;

    std::string display_name_;
    std::string user_id_;
    std::string avatar_url_;
    ImageProvider image_provider_;

    bool name_editable_  = false;
    bool name_busy_      = false;
    std::string name_error_;

    bool avatar_editable_ = false;
    bool avatar_busy_     = false;
    std::string avatar_error_;
    bool avatar_hovered_  = false;

    std::unique_ptr<tk::TextLayout> name_layout_;
    std::unique_ptr<tk::TextLayout> uid_layout_;
    std::unique_ptr<tk::TextLayout> name_error_layout_;
    std::unique_ptr<tk::TextLayout> avatar_error_layout_;
};

} // namespace tesseract::views
