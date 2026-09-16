#include "NotificationsSection.h"

#include "SettingsGroup.h"
#include "icons.h"

#include "tk/i18n.h"
#include "tk/layout.h"
#include "tk/text_field.h"

#include "tesseract/settings.h"

#include <algorithm>
#include <cctype>
#include <memory>
#include <utility>

namespace tesseract::views
{

namespace
{
std::string trim(const std::string& s)
{
    std::size_t begin = 0;
    std::size_t end   = s.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(s[begin])))
        ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])))
        --end;
    return s.substr(begin, end - begin);
}
} // namespace

// ---------------------------------------------------------------------------
// KeywordChip — a content-sized pill: HBox[ Label(keyword) | Button("x") ]
// with a rounded-rect background drawn behind both. Sized to its own content
// (no fill_main stretch) so KeywordFlow can pack several per row.
// ---------------------------------------------------------------------------

namespace
{
constexpr float kKeywordChipPadX = 10.0f;
constexpr float kKeywordChipPadY = 5.0f;
constexpr float kKeywordChipGap  = 6.0f;  // between label and remove button
constexpr float kKeywordFlowGapX = 8.0f;  // between chips on the same row
constexpr float kKeywordFlowGapY = 8.0f;  // between rows
} // namespace

class NotificationsSection::KeywordChip : public tk::HBox
{
public:
    explicit KeywordChip(const std::string& keyword, std::function<void()> on_remove)
    {
        set_spacing(kKeywordChipGap);
        set_padding(tk::Edges::symmetric(kKeywordChipPadX, kKeywordChipPadY));

        auto label = tk::create_widget<tk::Label>(this, keyword);
        add_child(std::move(label));

        auto remove = tk::create_widget<tk::Button>(
            this, "", std::function<void()>{}, tk::Button::Variant::Icon);
        remove->set_icon(kCloseSvg, 12.0f);
        remove->set_min_size({16.0f, 16.0f});
        remove->set_accessible_name(tk::trf(tk::tr("Remove keyword \"{0}\""), {keyword}));
        remove->set_on_click(std::move(on_remove));
        add_child(std::move(remove));
    }

    void paint_before_children(tk::PaintCtx& ctx) override
    {
        ctx.canvas.fill_rounded_rect(bounds_, bounds_.h * 0.5f,
                                     ctx.theme.palette.chip_bg);
    }
};

// ---------------------------------------------------------------------------
// KeywordFlow — packs KeywordChip children left-to-right, wrapping onto a
// new row when the next chip would overflow the available width. Plain
// left-to-right/top-to-bottom flow, no per-row alignment tricks: this is a
// settings list, not a design surface.
// ---------------------------------------------------------------------------

class NotificationsSection::KeywordFlow : public tk::Widget
{
public:
    KeywordFlow() = default;

    tk::Size measure(tk::LayoutCtx& ctx, tk::Size constraints) override
    {
        return {constraints.w, layout_(ctx, constraints.w, false)};
    }

    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override
    {
        bounds_ = bounds;
        layout_(ctx, bounds.w, true);
    }

private:
    // Single pass used for both measure() (dry run, no arrange() calls) and
    // arrange() (places children too). Returns the total content height.
    float layout_(tk::LayoutCtx& ctx, float avail_w, bool place)
    {
        float x = 0.0f;
        float y = 0.0f;
        float row_h = 0.0f;
        bool row_has_item = false;

        for (auto& ch : children())
        {
            if (!ch->visible())
                continue;
            tk::Size sz = ch->measure(ctx, {avail_w, 0});
            if (row_has_item && x + sz.w > avail_w)
            {
                x = 0.0f;
                y += row_h + kKeywordFlowGapY;
                row_h = 0.0f;
                row_has_item = false;
            }
            if (place)
            {
                ch->arrange(ctx, {bounds_.x + x, bounds_.y + y, sz.w, sz.h});
            }
            x += sz.w + kKeywordFlowGapX;
            row_h = std::max(row_h, sz.h);
            row_has_item = true;
        }
        return row_has_item ? y + row_h : 0.0f;
    }
};

NotificationsSection::NotificationsSection()
{
    const auto& s = tesseract::Settings::instance();

    auto* group = add_group(tk::tr("Notifications"));

    auto notif_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Enable notifications on this device"), s.notifications_enabled);
    notif_cb_ = group->add_widget(std::move(notif_cb));
    notif_cb_->on_change = [this](bool v)
    {
        if (on_notifications_changed) on_notifications_changed(v);
    };

    auto hide_content_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Hide message content in notifications"),
        s.notification_hide_content);
    hide_content_cb_ = group->add_widget(std::move(hide_content_cb));
    hide_content_cb_->on_change = [this](bool v)
    {
        if (on_hide_content_changed) on_hide_content_changed(v);
    };

    auto previews_cb = tk::create_widget<tk::CheckButton>(
        this, tk::tr("Show image & sticker previews in notifications"),
        s.notification_image_previews);
    previews_cb_ = group->add_widget(std::move(previews_cb));
    previews_cb_->on_change = [this](bool v)
    {
        if (on_image_previews_changed) on_image_previews_changed(v);
    };

    // ---- Mentions & Keywords (push-rule-backed, account-wide) ----

    mentions_group_ = add_group(tk::tr("Mentions & Keywords"));

    auto mentions_loading_lbl =
        tk::create_widget<tk::Label>(this, tk::tr("Loading…"));
    mentions_loading_label_ = mentions_group_->add_widget(std::move(mentions_loading_lbl));
    mentions_loading_label_->set_visible(false);

    auto mentions_sw = tk::create_widget<tk::SwitchButton>(
        this, tk::tr("Notify me for @mentions"), true);
    mentions_switch_ = mentions_group_->add_widget(std::move(mentions_sw));
    mentions_switch_->on_change = [this](bool v)
    {
        if (on_mentions_changed) on_mentions_changed(v);
    };

    auto room_mentions_sw = tk::create_widget<tk::SwitchButton>(
        this, tk::tr("Notify me when @room is used"), true);
    room_mentions_switch_ = mentions_group_->add_widget(std::move(room_mentions_sw));
    room_mentions_switch_->on_change = [this](bool v)
    {
        if (on_room_mentions_changed) on_room_mentions_changed(v);
    };

    auto all_messages_sw = tk::create_widget<tk::SwitchButton>(
        this, tk::tr("Notify me for all messages"), true);
    all_messages_switch_ = mentions_group_->add_widget(std::move(all_messages_sw));
    all_messages_switch_->on_change = [this](bool v)
    {
        if (on_notify_all_messages_changed) on_notify_all_messages_changed(v);
    };

    auto notify_on_keywords_sw = tk::create_widget<tk::SwitchButton>(
        this, tk::tr("Notify on keywords"), false);
    notify_on_keywords_switch_ = mentions_group_->add_widget(std::move(notify_on_keywords_sw));
    notify_on_keywords_switch_->on_change = [this](bool v)
    {
        if (on_notify_on_keywords_changed) on_notify_on_keywords_changed(v);
    };

    auto add_row = tk::create_widget<tk::HBox>(this);
    add_row->set_spacing(8.0f);

    auto keyword_field = tk::create_widget<tk::TextField>(this, 32.0f);
    keyword_field->set_placeholder(tk::tr("Add a keyword…"));
    keyword_field->set_on_submit([this] { try_submit_keyword_(); });
    keyword_field->set_layout_hints({.fill_main = true});
    keyword_field_ = add_row->add_child(std::move(keyword_field));

    auto keyword_add = tk::create_widget<tk::Button>(
        this, tk::tr("Add"), [this] { try_submit_keyword_(); },
        tk::Button::Variant::Subtle);
    keyword_add_btn_ = add_row->add_child(std::move(keyword_add));

    mentions_group_->add_widget(std::move(add_row));

    auto keyword_flow = tk::create_widget<KeywordFlow>(this);
    keyword_flow_ = mentions_group_->add_widget(std::move(keyword_flow));

    auto keyword_error_lbl = tk::create_widget<tk::Label>(this, "");
    keyword_error_label_ = mentions_group_->add_widget(std::move(keyword_error_lbl));
    keyword_error_label_->set_visible(false);

    rebuild_keyword_rows_();
}

NotificationsSection::~NotificationsSection() = default;

void NotificationsSection::set_checked(bool enabled)
{
    notif_cb_->set_checked(enabled);
}

void NotificationsSection::set_hide_content_checked(bool enabled)
{
    hide_content_cb_->set_checked(enabled);
}

void NotificationsSection::set_image_previews_checked(bool enabled)
{
    previews_cb_->set_checked(enabled);
}

void NotificationsSection::set_mentions_enabled(bool enabled)
{
    mentions_switch_->set_checked(enabled);
}

void NotificationsSection::set_room_mentions_enabled(bool enabled)
{
    room_mentions_switch_->set_checked(enabled);
}

void NotificationsSection::set_notify_all_messages(bool enabled)
{
    all_messages_switch_->set_checked(enabled);
}

void NotificationsSection::set_notify_on_keywords(bool enabled)
{
    notify_on_keywords_switch_->set_checked(enabled);
}

void NotificationsSection::set_notification_keywords(std::vector<std::string> keywords)
{
    keywords_ = std::move(keywords);
    rebuild_keyword_rows_();
}

void NotificationsSection::set_mentions_loading(bool loading)
{
    mentions_loading_ = loading;
    mentions_loading_label_->set_visible(loading);
    mentions_switch_->set_enabled(!loading);
    room_mentions_switch_->set_enabled(!loading);
    all_messages_switch_->set_enabled(!loading);
    notify_on_keywords_switch_->set_enabled(!loading);
    keyword_field_->set_enabled(!loading);
    keyword_add_btn_->set_enabled(!loading);
    for (auto* chip : keyword_chips_)
        if (chip) chip->set_enabled(!loading);
}

void NotificationsSection::set_keyword_error(std::string error)
{
    const bool has_error = !error.empty();
    keyword_error_label_->set_text(error);
    keyword_error_label_->set_visible(has_error);
}

void NotificationsSection::try_submit_keyword_()
{
    const std::string trimmed = trim(keyword_field_->text());
    if (trimmed.empty())
        return;
    keyword_field_->set_text("");
    if (std::find(keywords_.begin(), keywords_.end(), trimmed) != keywords_.end())
        return; // already present (e.g. a slow revert hasn't landed yet)

    // Apply optimistically — see SettingsController::add_notification_
    // keyword's comment for why the round trip can't be trusted to hand
    // back a fresh list afterward. revert_keyword_add() undoes this if the
    // server write fails.
    keywords_.push_back(trimmed);
    rebuild_keyword_rows_();
    if (on_keyword_add_requested)
        on_keyword_add_requested(trimmed);
}

void NotificationsSection::revert_keyword_add(const std::string& keyword)
{
    auto it = std::find(keywords_.begin(), keywords_.end(), keyword);
    if (it == keywords_.end())
        return;
    keywords_.erase(it);
    rebuild_keyword_rows_();
}

void NotificationsSection::revert_keyword_remove(const std::string& keyword)
{
    if (std::find(keywords_.begin(), keywords_.end(), keyword) != keywords_.end())
        return;
    keywords_.push_back(keyword);
    rebuild_keyword_rows_();
}

void NotificationsSection::rebuild_keyword_rows_()
{
    keyword_flow_->clear_children();
    keyword_chips_.clear();

    for (const auto& kw : keywords_)
    {
        auto chip = tk::create_widget<KeywordChip>(
            this, kw,
            [this, kw]
            {
                // Apply optimistically, same reasoning as try_submit_keyword_.
                auto it = std::find(keywords_.begin(), keywords_.end(), kw);
                if (it != keywords_.end())
                {
                    keywords_.erase(it);
                    rebuild_keyword_rows_();
                }
                if (on_keyword_remove_requested)
                    on_keyword_remove_requested(kw);
            });
        keyword_chips_.push_back(keyword_flow_->add_child(std::move(chip)));
    }
}

} // namespace tesseract::views
