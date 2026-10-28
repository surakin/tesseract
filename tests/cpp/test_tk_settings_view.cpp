#include <catch2/catch_test_macros.hpp>

#include "views/settings/SettingsController.h"
#include "views/SettingsView.h"
#include "views/settings/AppearanceSection.h"
#include "views/settings/LanguageSection.h"
#include "views/settings/NetworkSection.h"
#include "views/settings/PrivacySection.h"
#include "views/settings/SettingBinder.h"
#include "tk/combobox.h"
#include "tk/slider.h"
#include "tk/text_field.h"
#include "views/settings/UserPackEditor.h"
#include "tk/controls.h"
#include "tk/side_tab_view.h"
#include "tk/widget.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

using tesseract::views::SettingsView;

namespace
{

struct TkSettingsViewStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(900, 700);

    tk::LayoutCtx layout_ctx()
    {
        return tk::LayoutCtx{surface->factory(), tk::Theme::light()};
    }
    tk::PaintCtx paint_ctx()
    {
        return tk::PaintCtx{surface->canvas(), surface->factory(),
                            tk::Theme::light()};
    }
    void run(tk::Widget& root, tk::Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

// The hidden Advanced tab is index 12 in tab-registration order (see
// SettingsView::kAdvancedTabIdx: 11 top tabs [0-10], bottom tabs About=11,
// Advanced=12).
constexpr int kAdvancedTabIdx = 12;

// The "Emojis & Stickers" tab is index 9 in tab-registration order:
// Account=0, Devices=1, General=2, Appearance=3, Notifications=4, Media=5,
// Privacy=6, Network=7, Server=8, Emojis & Stickers=9.
constexpr int kUserPackTabIdx = 9;

tk::SideTabView* find_tabs(SettingsView& view)
{
    for (auto& c : view.children())
    {
        if (auto* t = dynamic_cast<tk::SideTabView*>(c.get()))
            return t;
    }
    return nullptr;
}

tk::Button* find_button(tk::Widget& root, const std::string& label)
{
    for (auto& c : root.children())
    {
        if (auto* b = dynamic_cast<tk::Button*>(c.get()); b && b->label() == label)
            return b;
        if (auto* b = find_button(*c, label))
            return b;
    }
    return nullptr;
}

void collect_check_buttons(tk::Widget& root, std::vector<tk::CheckButton*>& out)
{
    for (auto& c : root.children())
    {
        if (auto* b = dynamic_cast<tk::CheckButton*>(c.get()))
            out.push_back(b);
        collect_check_buttons(*c, out);
    }
}

} // namespace

TEST_CASE("AppearanceSection: the sender-status checkbox writes Settings and reports its key",
          "[settings-view]")
{
    auto& st = tesseract::Settings::instance();
    const bool saved = st.show_sender_status_in_timeline;
    st.show_sender_status_in_timeline = true;

    tesseract::views::SettingBinder binder;
    tesseract::views::AppearanceSection section(binder);
    std::vector<tk::CheckButton*> boxes;
    collect_check_buttons(section, boxes);
    REQUIRE_FALSE(boxes.empty());
    // Second to last in the Timeline group (the timezone checkbox follows it).
    REQUIRE(boxes.size() >= 2);
    tk::CheckButton* status_cb = boxes[boxes.size() - 2];

    std::vector<tesseract::views::SettingKey> fired;
    binder.on_changed = [&](tesseract::views::SettingKey k) { fired.push_back(k); };

    binder.reload(); // silent
    CHECK(status_cb->checked());
    CHECK(fired.empty());

    status_cb->on_change(false);
    REQUIRE(fired.size() == 1);
    CHECK(fired[0] == tesseract::views::SettingKey::ShowSenderStatus);
    CHECK_FALSE(st.show_sender_status_in_timeline);

    st.show_sender_status_in_timeline = saved;
}

TEST_CASE("AppearanceSection: the sender-timezone checkbox writes Settings and reports its key",
          "[settings-view]")
{
    auto& st = tesseract::Settings::instance();
    const bool saved = st.show_sender_timezone_in_timeline;
    st.show_sender_timezone_in_timeline = false;

    tesseract::views::SettingBinder binder;
    tesseract::views::AppearanceSection section(binder);
    std::vector<tk::CheckButton*> boxes;
    collect_check_buttons(section, boxes);
    REQUIRE_FALSE(boxes.empty());
    tk::CheckButton* tz_cb = boxes.back(); // added last, to the Timeline group

    std::vector<tesseract::views::SettingKey> fired;
    binder.on_changed = [&](tesseract::views::SettingKey k) { fired.push_back(k); };

    binder.reload();
    CHECK_FALSE(tz_cb->checked());

    tz_cb->on_change(true);
    REQUIRE(fired.size() == 1);
    CHECK(fired[0] == tesseract::views::SettingKey::ShowSenderTimezone);
    CHECK(st.show_sender_timezone_in_timeline);

    st.show_sender_timezone_in_timeline = saved;
}

TEST_CASE("LanguageSection: Restart now shows only while a restart is pending",
          "[settings-view]")
{
    tesseract::views::SettingBinder binder;
    tesseract::views::LanguageSection section(binder);
    int restarts = 0;
    section.on_restart_requested = [&] { ++restarts; };

    auto* restart = find_button(section, "Restart now");
    REQUIRE(restart);
    CHECK_FALSE(restart->visible());

    section.set_restart_pending(true);
    CHECK(restart->visible());
    restart->click();
    CHECK(restarts == 1);

    section.set_restart_pending(false);
    CHECK_FALSE(restart->visible());
}

namespace
{
template <typename T>
T* find_widget(tk::Widget& root)
{
    for (auto& c : root.children())
    {
        if (auto* w = dynamic_cast<T*>(c.get()))
            return w;
        if (auto* w = find_widget<T>(*c))
            return w;
    }
    return nullptr;
}
} // namespace

TEST_CASE("NetworkSection: URL field is enabled only for Manual and invalid URLs do not fire",
          "[settings-view]")
{
    using PM = tesseract::Settings::ProxyMode;
    tesseract::views::SettingBinder binder;
    tesseract::views::NetworkSection section(binder);
    auto* combo = find_widget<tk::ComboBox>(section);
    auto* field = find_widget<tk::TextField>(section);
    REQUIRE(combo);
    REQUIRE(field);

    auto& st = tesseract::Settings::instance();
    const PM saved_mode = st.proxy_mode;
    const std::string saved_url = st.proxy_url;
    int fired = 0;
    binder.on_changed = [&](tesseract::views::SettingKey k)
    {
        CHECK(k == tesseract::views::SettingKey::Proxy);
        ++fired;
    };
    auto load = [&](PM m, const std::string& u)
    {
        st.proxy_mode = m;
        st.proxy_url = u;
        binder.reload(); // silent
    };

    load(PM::System, "");
    CHECK_FALSE(field->enabled());
    CHECK(fired == 0);

    load(PM::Manual, "ftp://bad");
    CHECK(field->enabled());
    combo->on_changed("manual");
    CHECK(fired == 0); // invalid URL: nothing stored or reported

    load(PM::Manual, "http://proxy.example.com:8080");
    st.proxy_mode = PM::System; // differs from the combo's Manual
    combo->on_changed("manual");
    REQUIRE(fired == 1);
    CHECK(st.proxy_mode == PM::Manual);
    CHECK(st.proxy_url == "http://proxy.example.com:8080");

    combo->set_selected_value("none");
    combo->on_changed("none");
    CHECK_FALSE(field->enabled());
    REQUIRE(fired == 2);
    CHECK(st.proxy_mode == PM::None);

    st.proxy_mode = saved_mode;
    st.proxy_url = saved_url;
}

TEST_CASE("SettingsView::show_account_section selects the Account tab",
          "[settings-view]")
{
    TkSettingsViewStage st;
    auto view_owner = tk::create_root_widget<SettingsView>(nullptr);
    SettingsView& view = *view_owner;
    st.run(view, {0, 0, 900, 700});

    auto* tabs = find_tabs(view);
    REQUIRE(tabs);

    bool tab_changed = false;
    view.on_tab_changed = [&] { tab_changed = true; };

    tabs->select(kUserPackTabIdx); // navigate away from Account (index 0)
    REQUIRE(tabs->selected_idx() == kUserPackTabIdx);
    tab_changed = false;

    view.show_account_section();
    CHECK(tabs->selected_idx() == 0);
    CHECK(tab_changed); // fired on_tab_changed on the way back
}

TEST_CASE("SettingsView: Advanced tab is hidden by default", "[settings-view]")
{
    TkSettingsViewStage st;
    auto view_owner = tk::create_root_widget<SettingsView>(nullptr);
    SettingsView& view = *view_owner;
    st.run(view, {0, 0, 900, 700});

    auto* tabs = find_tabs(view);
    REQUIRE(tabs);
    CHECK_FALSE(tabs->tab_visible(kAdvancedTabIdx));
}

TEST_CASE("SettingsView: Advanced tab hides again after navigating away",
          "[settings-view]")
{
    TkSettingsViewStage st;
    auto view_owner = tk::create_root_widget<SettingsView>(nullptr);
    SettingsView& view = *view_owner;
    st.run(view, {0, 0, 900, 700});

    auto* tabs = find_tabs(view);
    REQUIRE(tabs);

    // Reveal it (mirrors AboutSection's "Advanced" button handler) and
    // navigate to it.
    tabs->set_tab_visible(kAdvancedTabIdx, true);
    tabs->select(kAdvancedTabIdx);
    REQUIRE(tabs->tab_visible(kAdvancedTabIdx));
    REQUIRE(tabs->selected_idx() == kAdvancedTabIdx);

    // Navigate to a different tab — Advanced must hide again.
    tabs->select(0);
    CHECK_FALSE(tabs->tab_visible(kAdvancedTabIdx));
}

TEST_CASE("SettingsView: file drop reaches the personal pack editor only "
          "when its tab is selected",
          "[settings-view]")
{
    TkSettingsViewStage st;
    auto view_owner = tk::create_root_widget<SettingsView>(nullptr);
    SettingsView& view = *view_owner;
    st.run(view, {0, 0, 900, 700});

    auto* tabs = find_tabs(view);
    REQUIRE(tabs);
    auto* editor = view.user_pack_editor();
    REQUIRE(editor);

    // Default-selected tab (Account) — the personal pack grid is arranged
    // (SideTabView lays out every tab's content each pass) but is not
    // visible, so tree dispatch must skip it (Widget::dispatch_file_drop
    // bails on !visible_) and leave the payload untouched.
    CHECK(tabs->selected_idx() != kUserPackTabIdx);
    {
        const tk::Rect r = editor->list_rect();
        const tk::Point p{r.x + 5, r.y + 5};
        tk::FileDropPayload payload{{1, 2, 3}, "image/png", "sticker.png"};
        CHECK(view.dispatch_file_drop(p, payload) == nullptr);
        CHECK_FALSE(payload.bytes.empty());
    }

    tabs->select(kUserPackTabIdx);
    st.run(view, {0, 0, 900, 700});
    {
        const tk::Rect r = editor->list_rect();
        const tk::Point p{r.x + 5, r.y + 5};
        tk::FileDropPayload payload{{1, 2, 3}, "image/png", "sticker.png"};
        CHECK(view.dispatch_file_drop(p, payload) == editor);
    }

    tabs->select(0);
    st.run(view, {0, 0, 900, 700});
    {
        const tk::Rect r = editor->list_rect();
        const tk::Point p{r.x + 5, r.y + 5};
        tk::FileDropPayload payload{{1, 2, 3}, "image/png", "sticker.png"};
        CHECK(view.dispatch_file_drop(p, payload) == nullptr);
    }
}

TEST_CASE("SettingsView: drag-hover claims the personal pack editor only "
          "when its tab is selected",
          "[settings-view]")
{
    TkSettingsViewStage st;
    auto view_owner = tk::create_root_widget<SettingsView>(nullptr);
    SettingsView& view = *view_owner;
    st.run(view, {0, 0, 900, 700});

    auto* tabs = find_tabs(view);
    REQUIRE(tabs);
    auto* editor = view.user_pack_editor();
    REQUIRE(editor);

    CHECK(tabs->selected_idx() != kUserPackTabIdx);
    {
        const tk::Rect r = editor->list_rect();
        const tk::Point p{r.x + 5, r.y + 5};
        CHECK(view.dispatch_native_drag_hover(p) == nullptr);
        CHECK_FALSE(editor->drag_hover());
    }

    tabs->select(kUserPackTabIdx);
    st.run(view, {0, 0, 900, 700});
    {
        const tk::Rect r = editor->list_rect();
        const tk::Point p{r.x + 5, r.y + 5};
        CHECK(view.dispatch_native_drag_hover(p) == editor);
        CHECK(editor->drag_hover());

        // No Host in this fixture to drive the leave transition — call the
        // leaf directly, mirroring what Host::dispatch_native_drag_hover/
        // dispatch_native_drag_leave would do on the previous claimant.
        editor->on_native_drag_leave();
        CHECK_FALSE(editor->drag_hover());
    }
}

TEST_CASE("SettingsView: file drop stages an image into the personal "
          "pack editor",
          "[settings-view]")
{
    TkSettingsViewStage st;
    auto view_owner = tk::create_root_widget<SettingsView>(nullptr);
    SettingsView& view = *view_owner;
    st.run(view, {0, 0, 900, 700});

    auto* tabs = find_tabs(view);
    REQUIRE(tabs);
    tabs->select(kUserPackTabIdx);
    st.run(view, {0, 0, 900, 700});

    auto* editor = view.user_pack_editor();
    REQUIRE(editor);
    CHECK(editor->images().empty());
    CHECK_FALSE(editor->has_changes());

    const tk::Rect r = editor->list_rect();
    tk::FileDropPayload payload{{1, 2, 3}, "image/png", "sticker.png"};
    CHECK(view.dispatch_file_drop({r.x + 5, r.y + 5}, payload) == editor);

    CHECK(editor->images().size() == 1);
    CHECK(editor->has_changes());
}

TEST_CASE("SettingsView: signed-out mode keeps only Network, Language and About",
          "[settings-view]")
{
    TkSettingsViewStage st;
    auto view_owner = tk::create_root_widget<SettingsView>(nullptr);
    SettingsView& view = *view_owner;
    st.run(view, {0, 0, 900, 700});

    auto* tabs = find_tabs(view);
    REQUIRE(tabs);
    constexpr int kNetworkTabIdx  = 7;
    constexpr int kLanguageTabIdx = 10;
    constexpr int kAboutTabIdx    = 11;

    view.set_signed_out_mode(true);
    for (int i = 0; i <= kAdvancedTabIdx; ++i)
    {
        const bool expected = i == kNetworkTabIdx || i == kLanguageTabIdx ||
                              i == kAboutTabIdx;
        CHECK(view.tab_visible(i) == expected);
    }
    CHECK(tabs->selected_idx() == kNetworkTabIdx);

    view.set_signed_out_mode(false);
    for (int i = 0; i <= kAboutTabIdx; ++i)
        CHECK(view.tab_visible(i));
    CHECK_FALSE(view.tab_visible(kAdvancedTabIdx));
}

TEST_CASE("NetworkSection: the note says changes apply now when signed out",
          "[settings-view]")
{
    tesseract::views::SettingBinder binder;
    tesseract::views::NetworkSection section(binder);

    const std::string restart = section.restart_note_text();
    CHECK(restart == "Changes take effect after restart."); // signed in

    section.set_applies_immediately(true);
    CHECK(section.restart_note_text() != restart);

    // A pending restart (calls still use the launch-time proxy) keeps the
    // "applied now" wording; the Restart button is what shows up.
    const std::string applied = section.restart_note_text();
    section.set_restart_pending(true);
    CHECK(section.restart_note_text() == applied);
    section.set_restart_pending(false);
    CHECK(section.restart_note_text() == applied);

    section.set_applies_immediately(false);
    CHECK(section.restart_note_text() == restart);
}

TEST_CASE("NetworkSection: the Restart button is hidden when the change applies live",
          "[settings-view]")
{
    tesseract::views::SettingBinder binder;
    tesseract::views::NetworkSection section(binder);

    // Signed in: a pending change needs a restart, so the button shows.
    section.set_restart_pending(true);
    CHECK(section.restart_row_visible());

    // Signed out (opened from the login screen): applied live, no button,
    // even though the setting still differs from the launch-time proxy.
    section.set_applies_immediately(true);
    CHECK(section.restart_pending_for_test());
    CHECK_FALSE(section.restart_row_visible());

    section.set_applies_immediately(false);
    CHECK(section.restart_row_visible());
    section.set_restart_pending(false);
    CHECK_FALSE(section.restart_row_visible());
}

namespace
{

void collect_widgets_of(tk::Widget& root, std::vector<tk::Slider*>& sliders,
                        std::vector<tk::ComboBox*>& combos, std::vector<tk::Label*>& labels)
{
    for (auto& c : root.children())
    {
        if (auto* s = dynamic_cast<tk::Slider*>(c.get()))
            sliders.push_back(s);
        if (auto* b = dynamic_cast<tk::ComboBox*>(c.get()))
            combos.push_back(b);
        if (auto* l = dynamic_cast<tk::Label*>(c.get()))
            labels.push_back(l);
        collect_widgets_of(*c, sliders, combos, labels);
    }
}

struct ColumnSettingsGuard
{
    tesseract::Settings& s = tesseract::Settings::instance();
    int pct = s.timeline_column_pct;
    tesseract::Settings::TimelineColumnAnchor anchor = s.timeline_column_anchor;
    tesseract::Settings::MessageLayout layout = s.message_layout;
    ~ColumnSettingsGuard()
    {
        s.timeline_column_pct = pct;
        s.timeline_column_anchor = anchor;
        s.message_layout = layout;
    }
};

} // namespace

TEST_CASE("AppearanceSection: the timeline width slider persists and reads out a percentage",
          "[settings-view]")
{
    ColumnSettingsGuard guard;
    auto& st = tesseract::Settings::instance();
    st.timeline_column_pct = 50;
    st.message_layout = tesseract::Settings::MessageLayout::Classic;

    tesseract::views::SettingBinder binder;
    tesseract::views::AppearanceSection section(binder);
    std::vector<tk::Slider*> sliders;
    std::vector<tk::ComboBox*> combos;
    std::vector<tk::Label*> labels;
    collect_widgets_of(section, sliders, combos, labels);
    binder.reload();
    REQUIRE(sliders.size() == 1);
    tk::Slider* slider = sliders[0];
    CHECK(slider->access_value().min == 30.0);
    CHECK(slider->access_value().max == 100.0);
    CHECK(slider->value() == 50.0);

    auto readout = [&]() -> std::string
    {
        for (auto* l : labels)
            if (l->access_name().ends_with("%"))
                return l->access_name();
        return {};
    };
    CHECK(readout() == "50%");

    std::vector<tesseract::views::SettingKey> fired;
    binder.on_changed = [&](tesseract::views::SettingKey k) { fired.push_back(k); };
    slider->on_changed(72.0); // live while dragging: value + readout, no save yet
    CHECK(st.timeline_column_pct == 72);
    CHECK(fired.empty());
    CHECK(readout() == "72%");
    slider->on_committed(72.0);
    REQUIRE(fired.size() == 1);
    CHECK(fired[0] == tesseract::views::SettingKey::TimelineColumnWidth);
}

TEST_CASE("AppearanceSection: the timeline position choice writes the anchor",
          "[settings-view]")
{
    ColumnSettingsGuard guard;
    auto& st = tesseract::Settings::instance();
    st.timeline_column_anchor = tesseract::Settings::TimelineColumnAnchor::Left;

    tesseract::views::SettingBinder binder;
    tesseract::views::AppearanceSection section(binder);
    std::vector<tk::Slider*> sliders;
    std::vector<tk::ComboBox*> combos;
    std::vector<tk::Label*> labels;
    collect_widgets_of(section, sliders, combos, labels);
    binder.reload();
    tk::ComboBox* anchor = nullptr;
    for (auto* c : combos)
        if (c->selected_value() == "left" || c->selected_value() == "center")
            anchor = c;
    REQUIRE(anchor != nullptr);
    CHECK(anchor->selected_value() == "left");

    std::vector<tesseract::views::SettingKey> fired;
    binder.on_changed = [&](tesseract::views::SettingKey k) { fired.push_back(k); };
    anchor->on_changed("center");
    CHECK(st.timeline_column_anchor == tesseract::Settings::TimelineColumnAnchor::Center);
    REQUIRE(fired.size() == 1);
    CHECK(fired[0] == tesseract::views::SettingKey::TimelineColumnAnchor);
}

TEST_CASE("AppearanceSection: the timeline column controls are disabled under the IRC layout",
          "[settings-view]")
{
    ColumnSettingsGuard guard;
    auto& st = tesseract::Settings::instance();
    st.message_layout = tesseract::Settings::MessageLayout::Classic;

    tesseract::views::SettingBinder binder;
    tesseract::views::AppearanceSection section(binder);
    std::vector<tk::Slider*> sliders;
    std::vector<tk::ComboBox*> combos;
    std::vector<tk::Label*> labels;
    collect_widgets_of(section, sliders, combos, labels);
    REQUIRE(sliders.size() == 1);
    CHECK(sliders[0]->enabled());

    st.message_layout = tesseract::Settings::MessageLayout::Irc;
    binder.reload();
    CHECK_FALSE(sliders[0]->enabled());

    st.message_layout = tesseract::Settings::MessageLayout::Bubbles;
    binder.reload();
    CHECK(sliders[0]->enabled());
}

TEST_CASE("PrivacySection: the encryption checkboxes no longer ask for a restart",
          "[settings-view]")
{
    tesseract::views::SettingBinder binder;
    tesseract::views::PrivacySection section(binder);
    std::vector<tk::CheckButton*> boxes;
    collect_check_buttons(section, boxes);
    int seen = 0;
    for (auto* b : boxes)
    {
        const std::string l = b->access_name();
        if (l.find("unconfirmed devices") != std::string::npos)
        {
            ++seen;
            CHECK(l.find("restart") == std::string::npos);
        }
    }
    CHECK(seen == 2);
}

TEST_CASE("SettingsView: controller results and pushed state relayout through the host",
          "[settings-view]")
{
    // The controller must outlive the view: the view clears its callbacks on
    // destruction.
    auto post = [](std::function<void()> fn) { fn(); };
    auto run = [](std::function<void()> fn) { fn(); };
    auto picker =
        [](std::function<void(std::vector<uint8_t>, std::string)>) {};
    auto decode = [](const std::vector<uint8_t>&)
    { return std::shared_ptr<tk::Image>(); };
    tesseract::SettingsController ctrl(nullptr, post, run, picker, decode);

    TestHost host(nullptr);
    auto view_owner = tk::create_root_widget<SettingsView>(&host);
    SettingsView& view = *view_owner;
    view.set_controller(&ctrl);

    auto fires_relayout = [&](const char* what, auto&& fn)
    {
        INFO(what);
        const int before = host.relayout_count;
        fn();
        CHECK(host.relayout_count > before);
    };
    fires_relayout("on_devices_loaded", [&] { ctrl.on_devices_loaded({}); });
    fires_relayout("on_name_changed", [&] { ctrl.on_name_changed("Alice"); });
    fires_relayout("on_mentions_toggle_result",
                   [&] { ctrl.on_mentions_toggle_result(true, true); });
    fires_relayout("set_server_info",
                   [&] { view.set_server_info(tesseract::ServerInfo{}); });
    fires_relayout("set_cache_sizes",
                   [&] { view.set_cache_sizes(1, 2, 3, 4, 5, 6, 7); });
}

TEST_CASE("SettingsView: without a host the state setters and load_device_lists are inert",
          "[settings-view]")
{
    auto view_owner = tk::create_root_widget<SettingsView>(nullptr);
    CHECK_NOTHROW(view_owner->request_relayout());
    CHECK_NOTHROW(view_owner->load_device_lists()); // no host: early return
    CHECK_NOTHROW(view_owner->set_server_info(tesseract::ServerInfo{}));
}
