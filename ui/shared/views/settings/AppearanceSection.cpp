#include "AppearanceSection.h"

#include "FontsEditor.h"
#include "MessageLayoutPreview.h"
#include "RoomListPreview.h"
#include "SettingsGroup.h"

#include "tesseract/settings.h"
#include "tk/i18n.h"
#include "tk/radio.h"
#include "tk/theme.h"
#include "tk/widget.h"

#include <tesseract/visual.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

namespace
{

// One-line explainer shown under the message-layout combobox, tracking the
// current selection.
std::string message_layout_description(tesseract::Settings::MessageLayout layout)
{
    using ML = tesseract::Settings::MessageLayout;
    switch (layout)
    {
    case ML::Bubbles:
        return tk::tr("Your own messages appear on the right, in a subtle "
                      "rounded bubble.");
    case ML::Irc:
        return tk::tr("A monospaced, mIRC-style timeline: every line is "
                      "\"[time] <nick>\" with a colour per person.");
    case ML::Classic:
        break;
    }
    return tk::tr("Every message is flush left, with the sender's "
                  "avatar in the gutter.");
}

// Option ids match tk::AccentTheme <-> Settings::ThemeAccent 1:1.
const char* accent_theme_value(tk::AccentTheme accent)
{
    switch (accent)
    {
    case tk::AccentTheme::Blue:   return "blue";
    case tk::AccentTheme::Forest: return "forest";
    case tk::AccentTheme::Sunset: return "sunset";
    case tk::AccentTheme::Violet: return "violet";
    case tk::AccentTheme::System: break;
    }
    return "system";
}

constexpr float kLayoutRowGap = 16.0f; // gap between the combo/desc column and the preview
constexpr float kLayoutColumnGap = 6.0f; // matches SettingsGroup's own inter-row spacing

} // namespace


// ---------------------------------------------------------------------------
// AppearanceSection — thin SettingsPage with one "Theme" group.
// ---------------------------------------------------------------------------

AppearanceSection::AppearanceSection(SettingBinder& binder)
{
    using S = tesseract::Settings;
    auto* group = add_group(tk::tr("Theme"));
    auto picker = tk::create_widget<tk::RadioGroup>(
        this, tk::FlexBox::Axis::Horizontal);
    picker->set_spacing(kLayoutRowGap);
    picker->set_options({
        {tk::tr("Light"), "light"},
        {tk::tr("Dark"), "dark"},
        {tk::tr("System"), "system"},
    });
    picker_ = group->add_widget(std::move(picker));
    binder.bind<S::ThemePreference>(
        picker_, &S::theme_pref, SettingKey::Theme,
        {{S::ThemePreference::Light, "light"},
         {S::ThemePreference::Dark, "dark"},
         {S::ThemePreference::System, "system"}});

    group->add_widget(tk::create_widget<tk::Label>(this, tk::tr("Accent color")));
    {
        auto accent_combo = tk::create_widget<tk::ComboBox>(this);
        std::vector<tk::ComboBox::Option> opts;
        for (const auto& info : tk::accent_theme_infos())
            opts.push_back({tk::tr(info.name), accent_theme_value(info.id)});
        accent_combo->set_options(std::move(opts));
        accent_combo_ = group->add_widget(std::move(accent_combo));
        binder.bind<S::ThemeAccent>(
            accent_combo_, &S::theme_accent, SettingKey::Accent,
            {{S::ThemeAccent::Blue, "blue"},
             {S::ThemeAccent::Forest, "forest"},
             {S::ThemeAccent::Sunset, "sunset"},
             {S::ThemeAccent::Violet, "violet"},
             {S::ThemeAccent::System, "system"}});
    }

    {
        using ML = tesseract::Settings::MessageLayout;
        const auto& s = tesseract::Settings::instance();
        auto* layout_group = add_group(tk::tr("Layout"));

        layout_group->add_widget(tk::create_widget<tk::Label>(
            this, tk::tr("Message layout")));

        // Combo + description on the left, live preview on the right.
        auto row = tk::create_widget<tk::HBox>(this);
        row->set_spacing(kLayoutRowGap);
        row->set_cross(tk::Cross::Start);

        auto column = tk::create_widget<tk::VBox>(this);
        column->set_layout_hints({.fill_main = true});
        column->set_spacing(kLayoutColumnGap);

        auto combo = tk::create_widget<tk::ComboBox>(this);
        combo->set_options({
            {tk::tr("Classic"), "classic"},
            {tk::tr("Bubbles"), "bubbles"},
            {tk::tr("IRC"), "irc"},
        });
        message_layout_combo_ = column->add_child(std::move(combo));

        auto desc = tk::create_widget<tk::Label>(
            this, message_layout_description(s.message_layout),
            tk::FontRole::Small);
        desc->set_wrap(true);
        message_layout_desc_ = column->add_child(std::move(desc));

        // Optional per-layout settings; each container is shown only while
        // its layout is selected.
        {
            auto opts = tk::create_widget<tk::VBox>(this);
            opts->set_spacing(kLayoutColumnGap);
            auto cb = tk::create_widget<tk::CheckButton>(
                this, tk::tr("Show my name and avatar"), s.bubbles_show_own_avatar);
            bubbles_own_avatar_cb_ = opts->add_child(std::move(cb));
            binder.bind(bubbles_own_avatar_cb_, &S::bubbles_show_own_avatar,
                        SettingKey::BubblesOwnAvatar);
            opts->set_visible(s.message_layout == ML::Bubbles);
            layout_options_bubbles_ = column->add_child(std::move(opts));
        }

        row->add_child(std::move(column));
        message_layout_preview_ = row->add_child(
            std::make_unique<MessageLayoutPreview>());

        layout_group->add_widget(std::move(row));

        binder.bind<ML>(message_layout_combo_, &S::message_layout,
                        SettingKey::MessageLayout,
                        {{ML::Classic, "classic"},
                         {ML::Bubbles, "bubbles"},
                         {ML::Irc, "irc"}});
        // Description, per-layout options and preview follow the selection,
        // both after a user change and after a silent reload.
        auto refresh_layout = [this]
        {
            const ML layout = tesseract::Settings::instance().message_layout;
            if (message_layout_desc_)
                message_layout_desc_->set_text(message_layout_description(layout));
            if (layout_options_bubbles_)
                layout_options_bubbles_->set_visible(layout == ML::Bubbles);
            if (message_layout_preview_)
                message_layout_preview_->refresh_layout();
        };
        {
            auto bound = message_layout_combo_->on_changed;
            message_layout_combo_->on_changed =
                [bound, refresh_layout](std::string v)
            {
                if (bound) bound(v);
                refresh_layout();
            };
        }
        binder.bind_custom(refresh_layout);

        // Timeline content column: width slider with a live readout, and
        // where the column sits. Classic and Bubbles only.
        {
            using A = tesseract::Settings::TimelineColumnAnchor;
            layout_group->add_widget(tk::create_widget<tk::Label>(
                this, tk::tr("Timeline width")));

            auto width_row = tk::create_widget<tk::HBox>(this);
            width_row->set_spacing(kLayoutRowGap);
            width_row->set_cross(tk::Cross::Center);

            auto slider = tk::create_widget<tk::Slider>(
                this, static_cast<double>(S::kTimelineColumnMinPct),
                static_cast<double>(S::kTimelineColumnMaxPct), 1.0);
            slider->set_page_step(10.0);
            slider->set_value(static_cast<double>(s.timeline_column_pct));
            slider->set_accessible_name(tk::tr("Timeline width"));
            slider->set_layout_hints({.fill_main = true});
            column_width_slider_ = width_row->add_child(std::move(slider));

            auto readout = tk::create_widget<tk::Label>(
                this, std::string{}, tk::FontRole::Small);
            column_width_value_ = width_row->add_child(std::move(readout));
            layout_group->add_widget(std::move(width_row));

            binder.bind(column_width_slider_, &S::timeline_column_pct,
                        SettingKey::TimelineColumnWidth);
            {
                auto bound = column_width_slider_->on_changed;
                column_width_slider_->on_changed = [this, bound](double v)
                {
                    if (bound) bound(v);
                    refresh_column_controls_();
                };
            }

            layout_group->add_widget(tk::create_widget<tk::Label>(
                this, tk::tr("Timeline position")));
            auto anchor = tk::create_widget<tk::ComboBox>(this);
            anchor->set_options({
                {tk::tr("Left"), "left"},
                {tk::tr("Centered"), "center"},
            });
            column_anchor_combo_ = layout_group->add_widget(std::move(anchor));
            binder.bind<A>(column_anchor_combo_, &S::timeline_column_anchor,
                           SettingKey::TimelineColumnAnchor,
                           {{A::Left, "left"}, {A::Center, "center"}});

            // Follows the layout combo too: IRC has no column to size.
            binder.bind_custom([this] { refresh_column_controls_(); });
            refresh_column_controls_();
            auto layout_bound = message_layout_combo_->on_changed;
            message_layout_combo_->on_changed =
                [layout_bound, this](std::string v)
            {
                if (layout_bound) layout_bound(v);
                refresh_column_controls_();
            };
        }

        // Room list density: radio group + description on the left, live
        // preview on the right.
        layout_group->add_widget(tk::create_widget<tk::Label>(
            this, tk::tr("Room list")));

        auto density_row = tk::create_widget<tk::HBox>(this);
        density_row->set_spacing(kLayoutRowGap);
        density_row->set_cross(tk::Cross::Start);

        auto density_col = tk::create_widget<tk::VBox>(this);
        density_col->set_layout_hints({.fill_main = true});
        density_col->set_spacing(kLayoutColumnGap);

        auto density = tk::create_widget<tk::RadioGroup>(
            this, tk::FlexBox::Axis::Vertical);
        density->set_options({
            {tk::tr("Normal"), "normal"},
            {tk::tr("Compact"), "compact"},
        });
        room_list_density_group_ = density_col->add_child(std::move(density));

        auto density_desc = tk::create_widget<tk::Label>(
            this,
            tk::tr("Compact shows only room names, with smaller avatars."),
            tk::FontRole::Small);
        density_desc->set_wrap(true);
        density_col->add_child(std::move(density_desc));

        density_row->add_child(std::move(density_col));
        room_list_preview_ = density_row->add_child(
            std::make_unique<RoomListPreview>());
        room_list_preview_->set_compact(
            s.room_list_density == tesseract::Settings::RoomListDensity::Compact);
        layout_group->add_widget(std::move(density_row));

        using RD = tesseract::Settings::RoomListDensity;
        binder.bind<RD>(room_list_density_group_, &S::room_list_density,
                        SettingKey::RoomListDensity,
                        {{RD::Normal, "normal"}, {RD::Compact, "compact"}});
        auto refresh_density = [this]
        {
            if (room_list_preview_)
                room_list_preview_->set_compact(
                    tesseract::Settings::instance().room_list_density ==
                    tesseract::Settings::RoomListDensity::Compact);
        };
        {
            auto bound = room_list_density_group_->on_changed;
            room_list_density_group_->on_changed =
                [bound, refresh_density](std::string v)
            {
                if (bound) bound(v);
                refresh_density();
            };
        }
        binder.bind_custom(refresh_density);
    }

    {
        auto* fonts_group = add_group(tk::tr("Fonts"));
        auto editor = tk::create_widget<FontsEditor>(this);
        editor->on_changed = [&binder](const tesseract::Settings::FontPrefs& p)
        {
            auto& s = tesseract::Settings::instance();
            if (s.fonts == p)
                return;
            s.fonts = p;
            binder.changed(SettingKey::Fonts);
        };
        fonts_editor_ = fonts_group->add_widget(std::move(editor));
        binder.bind_custom(
            [this]
            { fonts_editor_->set_prefs(tesseract::Settings::instance().fonts); });
    }

    {
        const auto& s = tesseract::Settings::instance();
        auto* rl_group = add_group(tk::tr("Room list"));

        auto cb_unread = tk::create_widget<tk::CheckButton>(
            this, tk::tr("Group unread rooms"), s.group_unread_rooms);
        group_unread_cb_ = rl_group->add_widget(std::move(cb_unread));
        binder.bind(group_unread_cb_, &S::group_unread_rooms, SettingKey::GroupUnread);

        auto cb = tk::create_widget<tk::CheckButton>(
            this, tk::tr("Group inactive rooms"), s.group_inactive_rooms);
        group_inactive_cb_ = rl_group->add_widget(std::move(cb));
        binder.bind(group_inactive_cb_, &S::group_inactive_rooms,
                    SettingKey::GroupInactive);

        auto combo = tk::create_widget<tk::ComboBox>(this);
        combo->set_options({
            {tk::tr("1 week"),   "7"},
            {tk::tr("2 weeks"),  "14"},
            {tk::tr("1 month"),  "30"},
            {tk::tr("3 months"), "90"},
            {tk::tr("6 months"), "180"},
        });
        period_combo_ = rl_group->add_widget(std::move(combo));
        binder.bind<int>(period_combo_, &S::inactive_room_threshold_days,
                         SettingKey::InactivePeriod,
                         {{7, "7"}, {14, "14"}, {30, "30"}, {90, "90"}, {180, "180"}});

        auto autoscroll = tk::create_widget<tk::CheckButton>(
            this, tk::tr("Scroll to rooms with new messages"), s.autoscroll_unread_rooms);
        autoscroll_cb_ = rl_group->add_widget(std::move(autoscroll));
        binder.bind(autoscroll_cb_, &S::autoscroll_unread_rooms,
                    SettingKey::AutoscrollUnread);
    }

    {
        const auto& s = tesseract::Settings::instance();
        auto* timeline_group = add_group(tk::tr("Timeline"));

        auto cb_membership = tk::create_widget<tk::CheckButton>(
            this, tk::tr("Show room join/leave events"), s.show_room_join_leave_events);
        show_membership_events_cb_ = timeline_group->add_widget(std::move(cb_membership));
        binder.bind(show_membership_events_cb_, &S::show_room_join_leave_events,
                    SettingKey::ShowMembershipEvents);

        auto cb_deleted = tk::create_widget<tk::CheckButton>(
            this, tk::tr("Show deleted messages"), s.show_deleted_messages);
        show_deleted_messages_cb_ = timeline_group->add_widget(std::move(cb_deleted));
        binder.bind(show_deleted_messages_cb_, &S::show_deleted_messages,
                    SettingKey::ShowDeletedMessages);

        auto cb_anim_avatars = tk::create_widget<tk::CheckButton>(
            this, tk::tr("Animate avatars"), s.animate_avatars);
        animate_avatars_cb_ = timeline_group->add_widget(std::move(cb_anim_avatars));
        binder.bind(animate_avatars_cb_, &S::animate_avatars, SettingKey::AnimateAvatars);

        auto cb_status = tk::create_widget<tk::CheckButton>(
            this, tk::tr("Show status emoji next to names"),
            s.show_sender_status_in_timeline);
        show_sender_status_cb_ = timeline_group->add_widget(std::move(cb_status));
        binder.bind(show_sender_status_cb_, &S::show_sender_status_in_timeline,
                    SettingKey::ShowSenderStatus);
    }
}

void AppearanceSection::refresh_column_controls_()
{
    const auto& st = tesseract::Settings::instance();
    const bool usable = st.message_layout != tesseract::Settings::MessageLayout::Irc;
    if (column_width_slider_)
        column_width_slider_->set_enabled(usable);
    if (column_anchor_combo_)
        column_anchor_combo_->set_enabled(usable);
    if (column_width_value_)
        column_width_value_->set_text(
            tk::trf(tk::tr("{0}%"), {std::to_string(st.timeline_column_pct)}));
}

AppearanceSection::~AppearanceSection() = default;

} // namespace tesseract::views
