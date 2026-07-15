#include "AccountSection.h"

#include "tesseract/client.h"

#include "tk/controls.h"
#include "tk/i18n.h"
#include "tk/layout.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "views/AvatarEditControl.h"

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

// ExtendedFields layout constants
constexpr float kFieldPadX  = 24.0f;
constexpr float kFieldPadY  = 12.0f;
constexpr float kAccountSectionLabelW     = 80.0f;
constexpr float kAccountSectionFieldGap   = 8.0f;
constexpr float kAccountSectionRowH       = 28.0f;
constexpr float kBioH       = 44.0f;
constexpr float kAccountSectionRowSpacing = 8.0f;
constexpr float kAvatarRadius     = kAvatarDiameter * 0.5f;
constexpr float kAccountSectionPadX             = 24.0f;
constexpr float kAccountSectionPadY             = 24.0f;
constexpr float kAccountSectionAvatarTextGap    = 16.0f;
constexpr float kAccountSectionLineGap          = 4.0f;
constexpr float kAccountSectionNameH            = 20.0f;
constexpr float kIdH              = 17.0f;
constexpr float kAccountSectionErrorGap         = 4.0f;
constexpr float kErrorH           = 14.0f;

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

    // `host` is nullable — see AccountSection::AccountSection().
    explicit Content(tk::Host* host = nullptr);

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
    tk::TextField* name_field() const
    {
        return name_field_;
    }

    std::function<void()> on_avatar_upload_clicked;
    std::function<void()> on_avatar_remove_clicked;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void on_theme_changed(const tk::Theme& t) override;
    void paint(tk::PaintCtx&) override;

    bool on_pointer_down(tk::Point local) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;

private:
    void invalidate_text();

    tk::Point disc_centre_local() const;

    std::string display_name_;
    std::string user_id_;

    bool name_editable_  = false;
    bool name_busy_      = false;
    std::string name_error_;

    AvatarEditControl avatar_;

    tk::TextField* name_field_ = nullptr; // owned via add_child when host provided

    std::unique_ptr<tk::TextLayout> name_layout_;
    std::unique_ptr<tk::TextLayout> uid_layout_;
    std::unique_ptr<tk::TextLayout> name_error_layout_;
};

AccountSection::Content::Content(tk::Host* host)
{
    if (host)
    {
        auto field = std::make_unique<tk::TextField>(*host, kAccountSectionNameH + 4.0f);
        field->set_placeholder(tk::tr("Display name"));
        name_field_ = add_child(std::move(field));
    }
}

void AccountSection::Content::set_display_name(std::string name)
{
    if (display_name_ == name) return;
    display_name_ = name;
    invalidate_text();
    if (name_field_) name_field_->set_text(std::move(name));
}

void AccountSection::Content::set_user_id(std::string uid)
{
    if (user_id_ == uid) return;
    user_id_ = std::move(uid);
    invalidate_text();
}

void AccountSection::Content::set_avatar_url(std::string mxc_url)
{
    avatar_.set_avatar_url(std::move(mxc_url));
}

void AccountSection::Content::set_image_provider(ImageProvider p)
{
    avatar_.set_image_provider(std::move(p));
}

void AccountSection::Content::set_editable(bool editable)
{
    name_editable_ = editable;
    invalidate_text();
}

void AccountSection::Content::set_avatar_editable(bool editable)
{
    avatar_.set_editable(editable);
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
    avatar_.set_busy(busy);
}

void AccountSection::Content::set_avatar_error(std::string error)
{
    avatar_.set_error(std::move(error));
}

void AccountSection::Content::invalidate_text()
{
    name_layout_.reset();
    uid_layout_.reset();
    name_error_layout_.reset();
}

tk::Point AccountSection::Content::disc_centre_local() const
{
    return {kAccountSectionPadX + kAvatarRadius, kAccountSectionPadY + kAvatarRadius};
}

tk::Rect AccountSection::Content::name_field_rect() const
{
    if (!name_editable_ || name_busy_)
        return {};
    const float text_x = bounds_.x + kAccountSectionPadX + kAvatarDiameter + kAccountSectionAvatarTextGap;
    const float text_w =
        std::max(0.0f, bounds_.x + bounds_.w - kAccountSectionPadX - text_x);
    const float col_h   = kAccountSectionNameH + kAccountSectionLineGap + kIdH;
    const float col_top =
        bounds_.y + kAccountSectionPadY + (kAvatarDiameter - col_h) * 0.5f;
    return {text_x, col_top - 2.0f, text_w, kAccountSectionNameH + 4.0f};
}

tk::Size AccountSection::Content::measure(tk::LayoutCtx&, tk::Size constraints)
{
    const float w = constraints.w > 0 ? constraints.w : 0;
    const float text_col_h = kAccountSectionNameH + kAccountSectionLineGap + kIdH;
    const float extra_h =
        (!name_error_.empty() ? kAccountSectionErrorGap + kErrorH : 0.0f) +
        (avatar_.has_error() ? kAccountSectionErrorGap + kErrorH : 0.0f);
    const float h =
        std::max(kAvatarDiameter, text_col_h) + 2.0f * kAccountSectionPadY + extra_h;
    return {w, h};
}

void AccountSection::Content::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    avatar_.set_geometry(disc_centre_local(), kAvatarDiameter);
    if (name_field_)
    {
        // SideTabView::arrange() re-arranges every tab's content on each
        // relayout, not just the selected one — visible_in_tree() stops a
        // deselected tab's field from reshowing itself just because its own
        // local rect is non-empty (visibility isn't cascaded down from a
        // hidden ancestor automatically).
        const tk::Rect r = name_field_rect();
        const bool show = !r.empty() && visible_in_tree();
        name_field_->set_visible(show);
        if (show)
            name_field_->arrange(ctx, r);
    }
}

void AccountSection::Content::on_theme_changed(const tk::Theme& t)
{
    if (name_field_)
        name_field_->set_text_color(t.palette.text_primary);
}

bool AccountSection::Content::on_pointer_down(tk::Point local)
{
    switch (avatar_.hit_test(local))
    {
    case AvatarEditControl::HitZone::RemoveChip:
        if (on_avatar_remove_clicked) on_avatar_remove_clicked();
        return true;
    case AvatarEditControl::HitZone::Disc:
        if (on_avatar_upload_clicked) on_avatar_upload_clicked();
        return true;
    case AvatarEditControl::HitZone::None:
        return false;
    }
    return false;
}

bool AccountSection::Content::on_pointer_move(tk::Point local)
{
    return avatar_.on_pointer_move(local);
}

void AccountSection::Content::on_pointer_leave()
{
    avatar_.on_pointer_leave();
}

void AccountSection::Content::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    {
        std::string_view name_source = display_name_.empty()
            ? (user_id_.empty() ? ""
                                : std::string_view{user_id_}.substr(
                                      user_id_.front() == '@' ? 1 : 0))
            : std::string_view{display_name_};
        avatar_.paint(ctx, {bounds_.x, bounds_.y}, name_source);
    }

    const float text_x = bounds_.x + kAccountSectionPadX + kAvatarDiameter + kAccountSectionAvatarTextGap;
    const float text_w = std::max(0.0f, bounds_.x + bounds_.w - kAccountSectionPadX - text_x);

    const float col_h   = kAccountSectionNameH + kAccountSectionLineGap + kIdH;
    const float col_top =
        bounds_.y + kAccountSectionPadY + (kAvatarDiameter - col_h) * 0.5f;

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
        const float uly = col_top + kAccountSectionNameH + 2.0f;
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
                             {text_x, col_top + kAccountSectionNameH + kAccountSectionLineGap + kIdH + kAccountSectionErrorGap},
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
                             {text_x, col_top + kAccountSectionNameH + kAccountSectionLineGap},
                             pal.text_secondary);
    }
}

// ---------------------------------------------------------------------------
// AccountSection::ExtendedFields — three labeled editable rows for MSC4133
// extended profile fields: pronouns, timezone, bio.
// ---------------------------------------------------------------------------

class AccountSection::ExtendedFields : public tk::Widget
{
public:
    // `host` is nullable — see AccountSection::AccountSection().
    explicit ExtendedFields(tk::Host* host = nullptr);

    void set_pronouns(std::string v);
    void set_tz(std::string v);
    void set_biography(std::string v);
    void set_fields_editable(bool editable);
    void set_field_busy(int idx, bool busy);
    void set_field_error(int idx, std::string error);

    tk::TextField* pronouns_field() const
    {
        return fields_[0];
    }
    tk::TextField* tz_field() const
    {
        return fields_[1];
    }
    tk::TextField* bio_field() const
    {
        return fields_[2];
    }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     on_theme_changed(const tk::Theme& t) override;
    void     paint(tk::PaintCtx&) override;

private:
    // Returns the Y offset (relative to bounds_.y) for each row's top edge,
    // accounting for any error text that follows the previous row.
    float row_y(int idx) const;

    // Height contributed by the error text below row idx (0 if no error).
    float error_extra(int idx) const;

    tk::Rect field_rect_for(int idx) const;

    std::string pronouns_;
    std::string tz_;
    std::string biography_;

    bool fields_editable_ = false;
    bool busy_[3]         = {};
    std::string error_[3];

    // Self-owned fields, one per row (pronouns/tz/bio) — null when
    // constructed without a Host. Owned via add_child.
    tk::TextField* fields_[3] = {};

    // Cached text layouts — reset when content changes
    mutable std::unique_ptr<tk::TextLayout> label_layout_[3];
    mutable std::unique_ptr<tk::TextLayout> value_layout_[3];
    mutable std::unique_ptr<tk::TextLayout> error_layout_[3];

    static constexpr const char* kLabels[3] = {"Pronouns", "Timezone", "Bio"};
};

AccountSection::ExtendedFields::ExtendedFields(tk::Host* host)
{
    if (!host)
        return;
    auto pronouns = std::make_unique<tk::TextField>(*host, kAccountSectionRowH);
    pronouns->set_placeholder(tk::tr("Pronouns"));
    fields_[0] = add_child(std::move(pronouns));

    auto tz = std::make_unique<tk::TextField>(*host, kAccountSectionRowH);
    tz->set_placeholder(tk::tr("Timezone (e.g. Europe/London)"));
    fields_[1] = add_child(std::move(tz));

    auto bio = std::make_unique<tk::TextField>(*host, kBioH);
    bio->set_placeholder(tk::tr("Short biography"));
    fields_[2] = add_child(std::move(bio));
}

// ---- helpers ---------------------------------------------------------------

float AccountSection::ExtendedFields::error_extra(int idx) const
{
    if (error_[idx].empty()) return 0.0f;
    return kAccountSectionErrorGap + kErrorH;
}

float AccountSection::ExtendedFields::row_y(int idx) const
{
    float y = bounds_.y + kFieldPadY;
    if (idx >= 1) y += kAccountSectionRowH + kAccountSectionRowSpacing + error_extra(0);
    if (idx >= 2) y += kAccountSectionRowH + kAccountSectionRowSpacing + error_extra(1);
    return y;
}

tk::Rect AccountSection::ExtendedFields::field_rect_for(int idx) const
{
    if (!fields_editable_ || busy_[idx]) return {};
    const float row_h = (idx == 2) ? kBioH : kAccountSectionRowH;
    const float x = bounds_.x + kFieldPadX + kAccountSectionLabelW + kAccountSectionFieldGap;
    const float w = std::max(0.0f, bounds_.x + bounds_.w - kFieldPadX - x);
    const float y = row_y(idx);
    return {x, y, w, row_h};
}

// ---- setters ---------------------------------------------------------------

void AccountSection::ExtendedFields::set_pronouns(std::string v)
{
    if (pronouns_ == v) return;
    pronouns_ = v;
    value_layout_[0].reset();
    if (fields_[0]) fields_[0]->set_text(std::move(v));
}

void AccountSection::ExtendedFields::set_tz(std::string v)
{
    if (tz_ == v) return;
    tz_ = v;
    value_layout_[1].reset();
    if (fields_[1]) fields_[1]->set_text(std::move(v));
}

void AccountSection::ExtendedFields::set_biography(std::string v)
{
    if (biography_ == v) return;
    biography_ = v;
    value_layout_[2].reset();
    if (fields_[2]) fields_[2]->set_text(std::move(v));
}

void AccountSection::ExtendedFields::set_fields_editable(bool editable)
{
    fields_editable_ = editable;
}

void AccountSection::ExtendedFields::set_field_busy(int idx, bool busy)
{
    if (idx < 0 || idx > 2) return;
    busy_[idx] = busy;
    if (busy) error_[idx].clear();
    value_layout_[idx].reset();
    error_layout_[idx].reset();
}

void AccountSection::ExtendedFields::set_field_error(int idx, std::string error)
{
    if (idx < 0 || idx > 2) return;
    error_[idx] = std::move(error);
    error_layout_[idx].reset();
}

// ---- layout ----------------------------------------------------------------

tk::Size AccountSection::ExtendedFields::measure(tk::LayoutCtx&,
                                                  tk::Size constraints)
{
    const float w = constraints.w > 0 ? constraints.w : 0;
    const float h = kFieldPadY
                    + kAccountSectionRowH + error_extra(0)
                    + kAccountSectionRowSpacing
                    + kAccountSectionRowH + error_extra(1)
                    + kAccountSectionRowSpacing
                    + kBioH + error_extra(2)
                    + kFieldPadY;
    return {w, h};
}

void AccountSection::ExtendedFields::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    // Invalidate label layouts so they rebuild with the new width.
    for (int i = 0; i < 3; ++i)
    {
        label_layout_[i].reset();
        value_layout_[i].reset();
        error_layout_[i].reset();
        if (fields_[i])
        {
            // See AccountSection::Content::arrange()'s comment: a deselected
            // tab still gets re-arranged, so visible_in_tree() is required
            // to keep a hidden field from reshowing itself.
            const tk::Rect r = field_rect_for(i);
            const bool show = !r.empty() && visible_in_tree();
            fields_[i]->set_visible(show);
            if (show)
                fields_[i]->arrange(ctx, r);
        }
    }
}

void AccountSection::ExtendedFields::on_theme_changed(const tk::Theme& t)
{
    for (auto* f : fields_)
        if (f) f->set_text_color(t.palette.text_primary);
}

// ---- paint -----------------------------------------------------------------

void AccountSection::ExtendedFields::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    const std::string* values[3] = {&pronouns_, &tz_, &biography_};

    for (int i = 0; i < 3; ++i)
    {
        const float row_h  = (i == 2) ? kBioH : kAccountSectionRowH;
        const float ry     = row_y(i);
        const float label_x = bounds_.x + kFieldPadX;
        const float field_x = label_x + kAccountSectionLabelW + kAccountSectionFieldGap;
        const float field_w = std::max(0.0f,
            bounds_.x + bounds_.w - kFieldPadX - field_x);

        // Label
        if (!label_layout_[i])
        {
            tk::TextStyle st;
            st.role      = tk::FontRole::Body;
            st.halign    = tk::TextHAlign::Leading;
            st.valign    = tk::TextVAlign::Top;
            st.max_width = kAccountSectionLabelW;
            label_layout_[i] = ctx.factory.build_text(kLabels[i], st);
        }
        if (label_layout_[i])
        {
            const tk::Size lsz = label_layout_[i]->measure();
            ctx.canvas.draw_text(*label_layout_[i],
                                 {label_x, ry + (row_h - lsz.h) * 0.5f},
                                 pal.text_secondary);
        }

        // Underline when editable and not busy
        if (fields_editable_ && !busy_[i])
        {
            const float uly = ry + row_h - 1.0f;
            ctx.canvas.fill_rect({field_x, uly, field_w, 1.0f},
                                 pal.text_secondary.with_alpha(80));
        }

        // Value text (only when not editable, or when busy)
        const std::string& val = *values[i];
        if (!val.empty() && (!fields_editable_ || busy_[i]))
        {
            if (!value_layout_[i])
            {
                tk::TextStyle st;
                st.role      = tk::FontRole::Body;
                st.halign    = tk::TextHAlign::Leading;
                st.valign    = tk::TextVAlign::Top;
                st.trim      = tk::TextTrim::Ellipsis;
                st.max_width = busy_[i]
                    ? std::max(0.0f, field_w - 20.0f)
                    : field_w;
                value_layout_[i] = ctx.factory.build_text(val, st);
            }
            if (value_layout_[i])
            {
                const float vy = ry + (row_h - value_layout_[i]->measure().h) * 0.5f;
                ctx.canvas.draw_text(*value_layout_[i],
                                     {field_x, vy},
                                     pal.text_primary);

                if (busy_[i])
                {
                    const tk::Size vsz = value_layout_[i]->measure();
                    tk::TextStyle ss;
                    ss.role = tk::FontRole::Body;
                    auto slay = ctx.factory.build_text("\xe2\x80\xa6", ss);
                    if (slay)
                        ctx.canvas.draw_text(*slay,
                                             {field_x + vsz.w + 4.0f, vy},
                                             pal.text_secondary);
                }
            }
        }

        // Error text below this row
        if (!error_[i].empty())
        {
            if (!error_layout_[i])
            {
                tk::TextStyle st;
                st.role      = tk::FontRole::Small;
                st.halign    = tk::TextHAlign::Leading;
                st.valign    = tk::TextVAlign::Top;
                st.max_width = field_w;
                error_layout_[i] = ctx.factory.build_text(error_[i], st);
            }
            if (error_layout_[i])
            {
                ctx.canvas.draw_text(*error_layout_[i],
                                     {field_x, ry + row_h + kAccountSectionErrorGap},
                                     tk::Color::rgb(0xcc3333));
            }
        }
    }
}

// ---------------------------------------------------------------------------
// AccountSection — thin SettingsPage wrapper around Content.
// ---------------------------------------------------------------------------

AccountSection::AccountSection(tk::Host* host)
{
    // Content owns its own outer padding; zero out the page inset so the
    // visuals match the pre-refactor build pixel-for-pixel. A fill_main
    // Spacer between Content and the Log Out row pins the button to the
    // bottom of the panel.
    set_padding(tk::Edges{});
    set_spacing(0.0f);

    content_ = add_widget(std::make_unique<Content>(host));
    content_->on_avatar_upload_clicked = [this]
    {
        if (on_avatar_upload_clicked) on_avatar_upload_clicked();
    };
    content_->on_avatar_remove_clicked = [this]
    {
        if (on_avatar_remove_clicked) on_avatar_remove_clicked();
    };

    ext_fields_ = add_widget(std::make_unique<ExtendedFields>(host));

    auto spacer = std::make_unique<tk::Spacer>();
    spacer->set_layout_hints({.fill_main = true});
    add_widget(std::move(spacer));

    auto row = std::make_unique<tk::HBox>();
    row->set_main(tk::Main::End);
    row->set_padding(tk::Edges{0.0f, kAccountSectionPadX, kAccountSectionPadY, kAccountSectionPadX});
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

tk::TextField* AccountSection::name_field() const
{
    return content_->name_field();
}

// ---- Extended profile forwarding (MSC4133) ---------------------------------

namespace
{
// Map an MSC unstable key to an ExtendedFields row index.
// Returns -1 for unknown keys.
int key_to_index(const std::string& key)
{
    if (key == "io.fsky.nyx.pronouns") return 0;
    if (key == "us.cloke.msc4175.tz")  return 1;
    if (key == "gay.fomx.biography")   return 2;
    return -1;
}
} // namespace

void AccountSection::set_extended_profile(const tesseract::ExtendedProfile& p)
{
    ext_fields_->set_pronouns(p.pronouns);
    ext_fields_->set_tz(p.tz);
    ext_fields_->set_biography(p.biography);
}

void AccountSection::set_profile_fields_editable(bool editable)
{
    ext_fields_->set_fields_editable(editable);
}

void AccountSection::set_profile_field_busy(const std::string& key, bool busy)
{
    const int idx = key_to_index(key);
    if (idx >= 0) ext_fields_->set_field_busy(idx, busy);
}

void AccountSection::set_profile_field_error(const std::string& key,
                                              std::string error)
{
    const int idx = key_to_index(key);
    if (idx >= 0) ext_fields_->set_field_error(idx, std::move(error));
}

tk::TextField* AccountSection::pronouns_field() const
{
    return ext_fields_->pronouns_field();
}

tk::TextField* AccountSection::tz_field() const
{
    return ext_fields_->tz_field();
}

tk::TextField* AccountSection::bio_field() const
{
    return ext_fields_->bio_field();
}

} // namespace tesseract::views
