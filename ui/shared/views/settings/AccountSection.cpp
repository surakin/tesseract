#include "AccountSection.h"

#include "tesseract/client.h"

#include "tk/controls.h"
#include "tk/i18n.h"
#include "tk/layout.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "views/AvatarEditControl.h"
#include "PronounsEditor.h"
#include "TimezonePicker.h"

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
protected:
    // host() is nullable — see AccountSection::AccountSection().
    Content();
    TK_WIDGET_FACTORY_FRIEND(Content)

public:
    using ImageProvider = AccountSection::ImageProvider;

    void set_display_name(std::string name);
    void set_user_id(std::string user_id);
    void set_avatar_url(std::string mxc_url);
    void set_avatar_preview(std::shared_ptr<tk::Image> image);
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

AccountSection::Content::Content()
{
    if (host())
    {
        auto field = tk::create_widget<tk::TextField>(this, kAccountSectionNameH + 4.0f);
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

void AccountSection::Content::set_avatar_preview(std::shared_ptr<tk::Image> image)
{
    avatar_.set_local_preview(std::move(image));
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
// AccountSection::ExtendedFields — labeled editable rows for MSC4133
// extended profile fields: pronouns (a PronounsEditor — a repeatable-row
// widget, since MSC4247 pronouns can have more than one language entry),
// timezone (a TimezonePicker — a searchable IANA zone-id field), and bio (a
// plain TextField).
// ---------------------------------------------------------------------------

class AccountSection::ExtendedFields : public tk::Widget
{
protected:
    // host() is nullable — see AccountSection::AccountSection().
    ExtendedFields();
    TK_WIDGET_FACTORY_FRIEND(ExtendedFields)

public:
    void set_pronouns(std::vector<tesseract::PronounEntry> entries);
    void set_tz(std::string v);
    void set_biography(std::string v);
    void set_fields_editable(bool editable);
    // idx: 0 = pronouns, 1 = tz, 2 = bio (matches AccountSection::key_to_index).
    void set_field_busy(int idx, bool busy);
    void set_field_error(int idx, std::string error);

    PronounsEditor* pronouns_editor() const
    {
        return pronouns_editor_;
    }
    TimezonePicker* tz_field() const
    {
        return tz_picker_;
    }
    tk::TextField* bio_field() const
    {
        return bio_field_;
    }

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     on_theme_changed(const tk::Theme& t) override;
    void     paint(tk::PaintCtx&) override;

private:
    // Y offset (relative to bounds_.y) for the pronouns row and for the
    // tz/bio rows (indices 0/1 into fields_), accounting for the pronouns
    // block's own variable height and any tz error text.
    float pronouns_row_y() const;
    float row_y(int idx) const; // idx into fields_ (0 = tz, 1 = bio)

    // Height contributed by the error text following the tz row (0 if none).
    float error_extra(int idx) const;

    tk::Rect field_rect_for(int idx) const; // idx into fields_ (0 = tz, 1 = bio)

    // Current pronouns-block height, recomputed each arrange() since it
    // depends on how many entries are staged.
    float pronouns_h_ = 0.0f;

    std::string tz_;
    std::string biography_;

    bool fields_editable_ = false;
    bool busy_[2]         = {};
    std::string error_[2];

    PronounsEditor* pronouns_editor_ = nullptr; // owned via add_child

    // Self-owned fields, one per row (tz/bio) — null when constructed
    // without a Host. Owned via add_child. Separate members (not a
    // homogeneous array like before) since TimezonePicker isn't a
    // tk::TextField — busy_/error_/*_layout_[2] below still use the
    // idx-0-is-tz/idx-1-is-bio convention for state/painting, which doesn't
    // care about the field's C++ type.
    TimezonePicker* tz_picker_ = nullptr;
    tk::TextField* bio_field_ = nullptr;

    // Cached text layouts — reset when content changes
    mutable std::unique_ptr<tk::TextLayout> pronouns_label_layout_;
    mutable std::unique_ptr<tk::TextLayout> label_layout_[2];
    mutable std::unique_ptr<tk::TextLayout> value_layout_[2];
    mutable std::unique_ptr<tk::TextLayout> error_layout_[2];

    static constexpr const char* kLabels[2] = {"Timezone", "Bio"};
};

AccountSection::ExtendedFields::ExtendedFields()
{
    // Hidden until set_fields_editable(true) confirms the server supports
    // MSC4133 profile fields, so the labels don't flash on screen before
    // server info is fetched (mirrors ServerSection's group_->set_visible).
    // Applied before the host()-null early return so it's unconditional,
    // including in unit tests that construct with a null host.
    set_visible(false);

    if (!host())
        return;

    auto pronouns = tk::create_widget<PronounsEditor>(this);
    pronouns_editor_ = add_child(std::move(pronouns));

    auto tz = tk::create_widget<TimezonePicker>(this);
    tz_picker_ = add_child(std::move(tz));

    auto bio = tk::create_widget<tk::TextField>(this, kBioH);
    bio->set_placeholder(tk::tr("Short biography"));
    bio_field_ = add_child(std::move(bio));
}

// ---- helpers ---------------------------------------------------------------

float AccountSection::ExtendedFields::error_extra(int idx) const
{
    if (error_[idx].empty()) return 0.0f;
    return kAccountSectionErrorGap + kErrorH;
}

float AccountSection::ExtendedFields::pronouns_row_y() const
{
    return bounds_.y + kFieldPadY;
}

float AccountSection::ExtendedFields::row_y(int idx) const
{
    float y = pronouns_row_y() + pronouns_h_ + kAccountSectionRowSpacing;
    if (idx >= 1) y += kAccountSectionRowH + kAccountSectionRowSpacing + error_extra(0);
    return y;
}

tk::Rect AccountSection::ExtendedFields::field_rect_for(int idx) const
{
    if (!fields_editable_ || busy_[idx]) return {};
    const float row_h = (idx == 1) ? kBioH : kAccountSectionRowH;
    const float x = bounds_.x + kFieldPadX + kAccountSectionLabelW + kAccountSectionFieldGap;
    const float w = std::max(0.0f, bounds_.x + bounds_.w - kFieldPadX - x);
    const float y = row_y(idx);
    return {x, y, w, row_h};
}

// ---- setters ---------------------------------------------------------------

void AccountSection::ExtendedFields::set_pronouns(
    std::vector<tesseract::PronounEntry> entries)
{
    if (pronouns_editor_) pronouns_editor_->set_pronouns(std::move(entries));
}

void AccountSection::ExtendedFields::set_tz(std::string v)
{
    if (tz_ == v) return;
    tz_ = v;
    value_layout_[0].reset();
    if (tz_picker_) tz_picker_->set_value(std::move(v));
}

void AccountSection::ExtendedFields::set_biography(std::string v)
{
    if (biography_ == v) return;
    biography_ = v;
    value_layout_[1].reset();
    if (bio_field_) bio_field_->set_text(std::move(v));
}

void AccountSection::ExtendedFields::set_fields_editable(bool editable)
{
    fields_editable_ = editable;
    if (pronouns_editor_) pronouns_editor_->set_editable(editable);
    // Unsupported/disabled servers hide the whole block (labels included)
    // rather than showing inert rows with no way to fill them in.
    set_visible(editable);
}

void AccountSection::ExtendedFields::set_field_busy(int idx, bool busy)
{
    if (idx == 0)
    {
        if (pronouns_editor_) pronouns_editor_->set_busy(busy);
        return;
    }
    const int i = idx - 1;
    if (i < 0 || i > 1) return;
    busy_[i] = busy;
    if (busy) error_[i].clear();
    value_layout_[i].reset();
    error_layout_[i].reset();
}

void AccountSection::ExtendedFields::set_field_error(int idx, std::string error)
{
    if (idx == 0)
    {
        if (pronouns_editor_) pronouns_editor_->set_error(std::move(error));
        return;
    }
    const int i = idx - 1;
    if (i < 0 || i > 1) return;
    error_[i] = std::move(error);
    error_layout_[i].reset();
}

// ---- layout ----------------------------------------------------------------

tk::Size AccountSection::ExtendedFields::measure(tk::LayoutCtx& ctx,
                                                  tk::Size constraints)
{
    const float w = constraints.w > 0 ? constraints.w : 0;
    const float field_w = std::max(0.0f,
        w - kFieldPadX - kAccountSectionLabelW - kAccountSectionFieldGap - kFieldPadX);
    pronouns_h_ = pronouns_editor_
                      ? pronouns_editor_->measure(ctx, {field_w, 0}).h
                      : 0.0f;
    const float h = kFieldPadY
                    + pronouns_h_
                    + kAccountSectionRowSpacing
                    + kAccountSectionRowH + error_extra(0)
                    + kAccountSectionRowSpacing
                    + kBioH + error_extra(1)
                    + kFieldPadY;
    return {w, h};
}

void AccountSection::ExtendedFields::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    const float label_x = bounds_.x + kFieldPadX;
    const float field_x = label_x + kAccountSectionLabelW + kAccountSectionFieldGap;
    const float field_w = std::max(0.0f, bounds_.x + bounds_.w - kFieldPadX - field_x);

    if (pronouns_editor_)
    {
        pronouns_h_ = pronouns_editor_->measure(ctx, {field_w, 0}).h;
        const bool show = fields_editable_ && visible_in_tree();
        pronouns_editor_->set_visible(show);
        if (show)
            pronouns_editor_->arrange(ctx, {field_x, pronouns_row_y(), field_w, pronouns_h_});
    }
    pronouns_label_layout_.reset();

    // Invalidate label layouts so they rebuild with the new width.
    for (int i = 0; i < 2; ++i)
    {
        label_layout_[i].reset();
        value_layout_[i].reset();
        error_layout_[i].reset();
    }

    // See AccountSection::Content::arrange()'s comment: a deselected tab
    // still gets re-arranged, so visible_in_tree() is required to keep a
    // hidden field from reshowing itself.
    if (tz_picker_)
    {
        const tk::Rect r = field_rect_for(0);
        const bool show = !r.empty() && visible_in_tree();
        tz_picker_->set_visible(show);
        if (show)
            tz_picker_->arrange(ctx, r);
    }
    if (bio_field_)
    {
        const tk::Rect r = field_rect_for(1);
        const bool show = !r.empty() && visible_in_tree();
        bio_field_->set_visible(show);
        if (show)
            bio_field_->arrange(ctx, r);
    }
}

void AccountSection::ExtendedFields::on_theme_changed(const tk::Theme& t)
{
    // TimezonePicker doesn't expose set_text_color directly (it owns its
    // internal field itself) — its own on_theme_changed forwards to it, same
    // as PronounsEditor's rows do for their own LanguagePickers.
    if (tz_picker_) tz_picker_->on_theme_changed(t);
    if (bio_field_) bio_field_->set_text_color(t.palette.text_primary);
}

// ---- paint -----------------------------------------------------------------

void AccountSection::ExtendedFields::paint(tk::PaintCtx& ctx)
{
    paint_children(ctx); // this override adds labels; children still need their own paint()
    const auto& pal = ctx.theme.palette;

    // Pronouns label — top-aligned with the (variable-height) editor block,
    // drawn once regardless of how many language rows it currently holds.
    {
        const float label_x = bounds_.x + kFieldPadX;
        const float ry      = pronouns_row_y();
        if (!pronouns_label_layout_)
        {
            tk::TextStyle st;
            st.role      = tk::FontRole::Body;
            st.halign    = tk::TextHAlign::Leading;
            st.valign    = tk::TextVAlign::Top;
            st.max_width = kAccountSectionLabelW;
            pronouns_label_layout_ = ctx.factory.build_text(tk::tr("Pronouns"), st);
        }
        if (pronouns_label_layout_)
        {
            ctx.canvas.draw_text(*pronouns_label_layout_, {label_x, ry},
                                 pal.text_secondary);
        }
    }

    const std::string* values[2] = {&tz_, &biography_};

    for (int i = 0; i < 2; ++i)
    {
        const float row_h  = (i == 1) ? kBioH : kAccountSectionRowH;
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

AccountSection::AccountSection()
{
    // Content owns its own outer padding; zero out the page inset so the
    // visuals match the pre-refactor build pixel-for-pixel. A fill_main
    // Spacer between Content and the Log Out row pins the button to the
    // bottom of the panel.
    set_padding(tk::Edges{});
    set_spacing(0.0f);

    content_ = add_widget(tk::create_widget<Content>(this));
    content_->on_avatar_upload_clicked = [this]
    {
        if (on_avatar_upload_clicked) on_avatar_upload_clicked();
    };
    content_->on_avatar_remove_clicked = [this]
    {
        if (on_avatar_remove_clicked) on_avatar_remove_clicked();
    };

    ext_fields_ = add_widget(tk::create_widget<ExtendedFields>(this));

    auto spacer = tk::create_widget<tk::Spacer>(this);
    spacer->set_layout_hints({.fill_main = true});
    add_widget(std::move(spacer));

    auto row = tk::create_widget<tk::HBox>(this);
    row->set_main(tk::Main::End);
    row->set_padding(tk::Edges{0.0f, kAccountSectionPadX, kAccountSectionPadY, kAccountSectionPadX});
    row->add_child(tk::create_widget<tk::Button>(this,
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

void AccountSection::set_avatar_preview(std::shared_ptr<tk::Image> image)
{
    content_->set_avatar_preview(std::move(image));
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

PronounsEditor* AccountSection::pronouns_editor() const
{
    return ext_fields_->pronouns_editor();
}

TimezonePicker* AccountSection::tz_field() const
{
    return ext_fields_->tz_field();
}

tk::TextField* AccountSection::bio_field() const
{
    return ext_fields_->bio_field();
}

} // namespace tesseract::views
