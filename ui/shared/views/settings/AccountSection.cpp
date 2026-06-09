#include "AccountSection.h"

#include "tk/controls.h"
#include "tk/layout.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "views/media_utils.h"

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace tesseract::views
{

namespace
{

constexpr float kAvatarDiameter   = 64.0f;
constexpr float kAvatarRadius     = kAvatarDiameter * 0.5f;
constexpr float kPadX             = 24.0f;
constexpr float kPadY             = 24.0f;
constexpr float kAvatarTextGap    = 16.0f;
constexpr float kLineGap          = 4.0f;
constexpr float kNameH            = 20.0f;
constexpr float kIdH              = 17.0f;
constexpr float kErrorGap         = 4.0f;
constexpr float kErrorH           = 14.0f;
constexpr float kRemoveChipR      = 9.0f;
constexpr float kXChipTolerance   = kRemoveChipR + 4.0f;

} // namespace

// ---------------------------------------------------------------------------
// AccountSection::Content — the bespoke profile widget (avatar disc, inline
// display-name editing, busy/error rendering). Kept verbatim from the
// pre-SettingsPage version of this section — only the surrounding class
// shape changed.
// ---------------------------------------------------------------------------

class AccountSection::Content : public tk::Widget
{
public:
    using ImageProvider = AccountSection::ImageProvider;

    void set_display_name(std::string name);
    void set_user_id(std::string user_id);
    void set_avatar_url(std::string mxc_url);
    void set_image_provider(ImageProvider provider);

    void set_editable(bool editable);
    void set_avatar_editable(bool editable);

    void set_name_busy(bool busy);
    void set_name_error(std::string error);

    void set_avatar_busy(bool busy);
    void set_avatar_error(std::string error);

    tk::Rect name_field_rect() const;

    std::function<void()> on_avatar_upload_clicked;
    std::function<void()> on_avatar_remove_clicked;

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

void AccountSection::Content::set_display_name(std::string name)
{
    if (display_name_ == name) return;
    display_name_ = std::move(name);
    invalidate_text();
}

void AccountSection::Content::set_user_id(std::string uid)
{
    if (user_id_ == uid) return;
    user_id_ = std::move(uid);
    invalidate_text();
}

void AccountSection::Content::set_avatar_url(std::string mxc_url)
{
    avatar_url_ = std::move(mxc_url);
}

void AccountSection::Content::set_image_provider(ImageProvider p)
{
    image_provider_ = std::move(p);
}

void AccountSection::Content::set_editable(bool editable)
{
    name_editable_ = editable;
    invalidate_text();
}

void AccountSection::Content::set_avatar_editable(bool editable)
{
    avatar_editable_ = editable;
}

void AccountSection::Content::set_name_busy(bool busy)
{
    name_busy_ = busy;
    if (busy) name_error_.clear();
    invalidate_text();
}

void AccountSection::Content::set_name_error(std::string error)
{
    name_error_ = std::move(error);
    name_error_layout_.reset();
}

void AccountSection::Content::set_avatar_busy(bool busy)
{
    avatar_busy_ = busy;
    if (busy) avatar_error_.clear();
    avatar_error_layout_.reset();
}

void AccountSection::Content::set_avatar_error(std::string error)
{
    avatar_error_ = std::move(error);
    avatar_error_layout_.reset();
}

void AccountSection::Content::invalidate_text()
{
    name_layout_.reset();
    uid_layout_.reset();
    name_error_layout_.reset();
}

tk::Point AccountSection::Content::disc_centre() const
{
    return {bounds_.x + kPadX + kAvatarRadius,
            bounds_.y + kPadY + kAvatarRadius};
}

bool AccountSection::Content::in_disc(tk::Point local) const
{
    const float cx = kPadX + kAvatarRadius;
    const float cy = kPadY + kAvatarRadius;
    const float dx = local.x - cx;
    const float dy = local.y - cy;
    return (dx * dx + dy * dy) <= (kAvatarRadius * kAvatarRadius);
}

bool AccountSection::Content::in_remove_chip(tk::Point local) const
{
    const float cx = kPadX + kAvatarDiameter - kRemoveChipR;
    const float cy = kPadY + kRemoveChipR;
    const float dx = local.x - cx;
    const float dy = local.y - cy;
    return (dx * dx + dy * dy) <= (kXChipTolerance * kXChipTolerance);
}

tk::Rect AccountSection::Content::name_field_rect() const
{
    if (!name_editable_ || name_busy_)
        return {};
    const float text_x = bounds_.x + kPadX + kAvatarDiameter + kAvatarTextGap;
    const float text_w =
        std::max(0.0f, bounds_.x + bounds_.w - kPadX - text_x);
    const float col_h   = kNameH + kLineGap + kIdH;
    const float col_top =
        bounds_.y + kPadY + (kAvatarDiameter - col_h) * 0.5f;
    return {text_x, col_top - 2.0f, text_w, kNameH + 4.0f};
}

tk::Size AccountSection::Content::measure(tk::LayoutCtx&, tk::Size constraints)
{
    const float w = constraints.w > 0 ? constraints.w : 0;
    const float text_col_h = kNameH + kLineGap + kIdH;
    const float extra_h =
        (!name_error_.empty() ? kErrorGap + kErrorH : 0.0f) +
        (!avatar_error_.empty() ? kErrorGap + kErrorH : 0.0f);
    const float h =
        std::max(kAvatarDiameter, text_col_h) + 2.0f * kPadY + extra_h;
    return {w, h};
}

void AccountSection::Content::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    bounds_ = bounds;
}

bool AccountSection::Content::on_pointer_down(tk::Point local)
{
    if (!avatar_editable_ || avatar_busy_)
        return false;
    if (!avatar_url_.empty() && in_remove_chip(local))
    {
        if (on_avatar_remove_clicked) on_avatar_remove_clicked();
        return true;
    }
    if (in_disc(local))
    {
        if (on_avatar_upload_clicked) on_avatar_upload_clicked();
        return true;
    }
    return false;
}

bool AccountSection::Content::on_pointer_move(tk::Point local)
{
    if (!avatar_editable_) return false;
    const bool was = avatar_hovered_;
    avatar_hovered_ = in_disc(local);
    return avatar_hovered_ != was;
}

void AccountSection::Content::on_pointer_leave()
{
    if (avatar_hovered_)
    {
        avatar_hovered_ = false;
    }
}

void AccountSection::Content::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    const tk::Point centre = disc_centre();

    const tk::Image* img = (image_provider_ && !avatar_url_.empty())
                               ? image_provider_(avatar_url_)
                               : nullptr;
    {
        std::string_view name_source = display_name_.empty()
            ? (user_id_.empty() ? ""
                                : std::string_view{user_id_}.substr(
                                      user_id_.front() == '@' ? 1 : 0))
            : std::string_view{display_name_};
        draw_avatar(ctx.canvas, img, centre, kAvatarDiameter, name_source,
                    pal.avatar_initials_bg, pal.avatar_initials_text);
    }

    if (avatar_editable_)
    {
        const tk::Rect disc_rect{bounds_.x + kPadX,
                                 bounds_.y + kPadY,
                                 kAvatarDiameter,
                                 kAvatarDiameter};

        if (avatar_busy_)
        {
            ctx.canvas.push_clip_rounded_rect(disc_rect, kAvatarRadius);
            ctx.canvas.fill_rect(disc_rect, tk::Color::rgba(0, 0, 0, 160));
            ctx.canvas.pop_clip();
            tk::TextStyle st;
            st.role = tk::FontRole::Title;
            auto lay = ctx.factory.build_text("\xe2\x80\xa6", st);
            if (lay)
            {
                const tk::Size sz = lay->measure();
                ctx.canvas.draw_text(*lay,
                                     {centre.x - sz.w * 0.5f,
                                      centre.y - sz.h * 0.5f},
                                     tk::Color::rgb(0xffffff));
            }
        }
        else if (avatar_hovered_)
        {
            ctx.canvas.push_clip_rounded_rect(disc_rect, kAvatarRadius);
            ctx.canvas.fill_rect(disc_rect, tk::Color::rgba(0, 0, 0, 128));
            ctx.canvas.pop_clip();
            tk::TextStyle st;
            st.role = tk::FontRole::Title;
            auto lay = ctx.factory.build_text("+", st);
            if (lay)
            {
                const tk::Size sz = lay->measure();
                ctx.canvas.draw_text(*lay,
                                     {centre.x - sz.w * 0.5f,
                                      centre.y - sz.h * 0.5f},
                                     tk::Color::rgb(0xffffff));
            }

            if (!avatar_url_.empty())
            {
                const float cx = bounds_.x + kPadX + kAvatarDiameter - kRemoveChipR;
                const float cy = bounds_.y + kPadY + kRemoveChipR;
                ctx.canvas.fill_rounded_rect(
                    {cx - kRemoveChipR, cy - kRemoveChipR,
                     kRemoveChipR * 2.0f, kRemoveChipR * 2.0f},
                    kRemoveChipR,
                    tk::Color::rgba(40, 40, 40, 220));
                tk::TextStyle xs;
                xs.role = tk::FontRole::Small;
                auto xlay = ctx.factory.build_text("\xc3\x97", xs);
                if (xlay)
                {
                    const tk::Size xsz = xlay->measure();
                    ctx.canvas.draw_text(*xlay,
                                         {cx - xsz.w * 0.5f,
                                          cy - xsz.h * 0.5f},
                                         tk::Color::rgb(0xffffff));
                }
            }
        }

        if (!avatar_error_.empty())
        {
            if (!avatar_error_layout_)
            {
                tk::TextStyle st;
                st.role      = tk::FontRole::Small;
                st.halign    = tk::TextHAlign::Leading;
                st.valign    = tk::TextVAlign::Top;
                st.max_width = kAvatarDiameter;
                avatar_error_layout_ =
                    ctx.factory.build_text(avatar_error_, st);
            }
            ctx.canvas.draw_text(*avatar_error_layout_,
                                 {bounds_.x + kPadX,
                                  bounds_.y + kPadY + kAvatarDiameter + kErrorGap},
                                 tk::Color::rgb(0xcc3333));
        }
    }

    const float text_x = bounds_.x + kPadX + kAvatarDiameter + kAvatarTextGap;
    const float text_w = std::max(0.0f, bounds_.x + bounds_.w - kPadX - text_x);

    const float col_h   = kNameH + kLineGap + kIdH;
    const float col_top =
        bounds_.y + kPadY + (kAvatarDiameter - col_h) * 0.5f;

    if (!name_editable_)
    {
        if (!name_layout_ && !display_name_.empty())
        {
            tk::TextStyle st;
            st.role      = tk::FontRole::Title;
            st.halign    = tk::TextHAlign::Leading;
            st.valign    = tk::TextVAlign::Top;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = text_w;
            name_layout_ = ctx.factory.build_text(display_name_, st);
        }
        if (name_layout_)
        {
            ctx.canvas.draw_text(*name_layout_, {text_x, col_top},
                                 pal.text_primary);
        }
        else if (display_name_.empty() && !user_id_.empty())
        {
            tk::TextStyle st;
            st.role      = tk::FontRole::Title;
            st.halign    = tk::TextHAlign::Leading;
            st.valign    = tk::TextVAlign::Top;
            st.trim      = tk::TextTrim::Ellipsis;
            st.max_width = text_w;
            auto lay = ctx.factory.build_text(user_id_, st);
            if (lay)
                ctx.canvas.draw_text(*lay, {text_x, col_top}, pal.text_primary);
        }
    }
    else
    {
        const float uly = col_top + kNameH + 2.0f;
        ctx.canvas.fill_rect({text_x, uly, text_w, 1.0f},
                             pal.text_secondary.with_alpha(80));

        if (name_busy_)
        {
            if (!name_layout_ && !display_name_.empty())
            {
                tk::TextStyle st;
                st.role      = tk::FontRole::Title;
                st.halign    = tk::TextHAlign::Leading;
                st.valign    = tk::TextVAlign::Top;
                st.trim      = tk::TextTrim::Ellipsis;
                st.max_width = std::max(0.0f, text_w - 20.0f);
                name_layout_ = ctx.factory.build_text(display_name_, st);
            }
            if (name_layout_)
            {
                ctx.canvas.draw_text(*name_layout_, {text_x, col_top},
                                     pal.text_primary);
                const tk::Size nsz = name_layout_->measure();
                tk::TextStyle ss;
                ss.role = tk::FontRole::Body;
                auto slay = ctx.factory.build_text("…", ss);
                if (slay)
                    ctx.canvas.draw_text(*slay,
                                         {text_x + nsz.w + 4.0f, col_top},
                                         pal.text_secondary);
            }
        }
    }

    if (!name_error_.empty())
    {
        if (!name_error_layout_)
        {
            tk::TextStyle st;
            st.role      = tk::FontRole::Small;
            st.halign    = tk::TextHAlign::Leading;
            st.valign    = tk::TextVAlign::Top;
            st.max_width = text_w;
            name_error_layout_ = ctx.factory.build_text(name_error_, st);
        }
        ctx.canvas.draw_text(*name_error_layout_,
                             {text_x, col_top + kNameH + kLineGap + kIdH + kErrorGap},
                             tk::Color::rgb(0xcc3333));
    }

    if (!uid_layout_ && !user_id_.empty())
    {
        tk::TextStyle st;
        st.role      = tk::FontRole::Body;
        st.halign    = tk::TextHAlign::Leading;
        st.valign    = tk::TextVAlign::Top;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = text_w;
        uid_layout_ = ctx.factory.build_text(user_id_, st);
    }
    if (uid_layout_)
    {
        ctx.canvas.draw_text(*uid_layout_,
                             {text_x, col_top + kNameH + kLineGap},
                             pal.text_secondary);
    }
}

// ---------------------------------------------------------------------------
// AccountSection — thin SettingsPage wrapper around Content.
// ---------------------------------------------------------------------------

AccountSection::AccountSection()
{
    // Content owns its own outer padding; zero out the page inset so the
    // visuals match the pre-refactor build pixel-for-pixel. A fill_main
    // Spacer between Content and the Log Out row pins the button to the
    // bottom of the panel.
    set_padding(tk::Edges{});
    set_spacing(0.0f);

    content_ = add_widget(std::make_unique<Content>());
    content_->on_avatar_upload_clicked = [this]
    {
        if (on_avatar_upload_clicked) on_avatar_upload_clicked();
    };
    content_->on_avatar_remove_clicked = [this]
    {
        if (on_avatar_remove_clicked) on_avatar_remove_clicked();
    };

    auto spacer = std::make_unique<tk::Spacer>();
    spacer->set_layout_hints({.fill_main = true});
    add_widget(std::move(spacer));

    auto row = std::make_unique<tk::HBox>();
    row->set_main(tk::Main::End);
    row->set_padding(tk::Edges{0.0f, kPadX, kPadY, kPadX});
    row->add_child(std::make_unique<tk::Button>(
        "Log Out",
        [this] { if (on_logout) on_logout(); },
        tk::Button::Variant::Destructive));
    add_widget(std::move(row));
}

AccountSection::~AccountSection() = default;

void AccountSection::set_display_name(std::string name)
{
    content_->set_display_name(std::move(name));
}

void AccountSection::set_user_id(std::string uid)
{
    content_->set_user_id(std::move(uid));
}

void AccountSection::set_avatar_url(std::string mxc_url)
{
    content_->set_avatar_url(std::move(mxc_url));
}

void AccountSection::set_image_provider(ImageProvider p)
{
    content_->set_image_provider(std::move(p));
}

void AccountSection::set_editable(bool editable)
{
    content_->set_editable(editable);
}

void AccountSection::set_avatar_editable(bool editable)
{
    content_->set_avatar_editable(editable);
}

void AccountSection::set_name_busy(bool busy)
{
    content_->set_name_busy(busy);
}

void AccountSection::set_name_error(std::string error)
{
    content_->set_name_error(std::move(error));
}

void AccountSection::set_avatar_busy(bool busy)
{
    content_->set_avatar_busy(busy);
}

void AccountSection::set_avatar_error(std::string error)
{
    content_->set_avatar_error(std::move(error));
}

tk::Rect AccountSection::name_field_rect() const
{
    return content_->name_field_rect();
}

} // namespace tesseract::views
