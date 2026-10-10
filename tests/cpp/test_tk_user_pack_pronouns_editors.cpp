#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/settings/PronounsEditor.h"
#include "views/settings/UserPackEditor.h"

#include <tesseract/image_pack.h>
#include <tesseract/types.h>

#include <cstdint>
#include <string>
#include <vector>

using tesseract::views::PronounsEditor;
using tesseract::views::UserPackEditor;

namespace
{

// ── PronounsEditor ──────────────────────────────────────────────────────

struct PeStage : vt::Stage
{
    std::unique_ptr<PronounsEditor> ed = tk::create_root_widget<PronounsEditor>(&host);
    PeStage() { run(); }
    void run() { mount(*ed, {0, 0, 600, 700}); }
    // Per row: [language picker's field, summary field]; rows are fixed slots.
    tk::TextField* summary(std::size_t row)
    {
        return vt::find_all<tk::TextField>(*ed).at(row * 2 + 1);
    }
};

// ── UserPackEditor ──────────────────────────────────────────────────────

tesseract::ImagePackImage upe_img(const std::string& sc, const std::string& url)
{
    tesseract::ImagePackImage i;
    i.shortcode = sc;
    i.url = url;
    i.body = sc;
    return i;
}

struct UpeStage : vt::Stage
{
    std::unique_ptr<UserPackEditor> ed = tk::create_root_widget<UserPackEditor>(&host);
    UpeStage()
    {
        ed->set_images({upe_img("happy", "mxc://x/1"), upe_img("sad", "mxc://x/2")});
        run();
    }
    void run() { mount(*ed, {0, 0, 480, 320}); }
};

} // namespace

TEST_CASE("PronounsEditor lists entries and flushes edited summaries",
          "[tk][view][pronouns]")
{
    PeStage s;
    s.ed->set_editable(true);
    s.ed->set_pronouns({{"en", "she/her", ""}});
    s.run();
    REQUIRE(s.ed->pronouns().size() == 1);
    CHECK(s.summary(0)->text() == "she/her");

    std::vector<std::vector<tesseract::PronounEntry>> saves;
    s.ed->on_changed = [&](std::vector<tesseract::PronounEntry> e) { saves.push_back(std::move(e)); };

    s.ed->flush(); // nothing changed: no notification
    CHECK(saves.empty());
    s.summary(0)->set_text("they/them");
    s.ed->flush();
    REQUIRE(saves.size() == 1);
    CHECK(saves[0][0].summary == "they/them");
    s.ed->flush(); // unchanged since
    CHECK(saves.size() == 1);
}

TEST_CASE("PronounsEditor add and remove rows, with the 8-row cap",
          "[tk][view][pronouns]")
{
    PeStage s;
    s.ed->set_editable(true);
    s.ed->set_pronouns({{"en", "she/her", ""}});
    s.run();
    int saves = 0;
    s.ed->on_changed = [&](std::vector<tesseract::PronounEntry>) { ++saves; };

    REQUIRE(vt::press(*s.ed, "+ Add language"));
    CHECK(s.ed->pronouns().size() == 2);
    s.run();
    // An incomplete (empty summary) row is not reported as a change.
    s.ed->flush();
    CHECK(saves == 0);
    s.summary(1)->set_text("he/him");
    // The default language comes from the process locale, which is empty
    // under test; pick one explicitly like the user would.
    vt::find_all<tesseract::views::LanguagePicker>(*s.ed).at(1)->set_value("en");
    s.ed->flush();
    CHECK(saves == 1);

    // Removing a row flushes the remaining entries.
    auto removes = vt::nodes_named(*s.ed, "Remove pronoun language");
    REQUIRE_FALSE(removes.empty());
    CHECK(tk::invoke_default_action(removes[0]));
    CHECK(s.ed->pronouns().size() == 1);
    CHECK(saves == 2);

    for (int i = 0; i < 10; ++i)
        vt::press(*s.ed, "+ Add language");
    s.run();
    CHECK(s.ed->pronouns().size() == 8);
    CHECK_FALSE(vt::has_name(*s.ed, "+ Add language")); // hidden at the cap
}

TEST_CASE("PronounsEditor set_pronouns keeps unfinished local rows; busy/error/"
          "compact/theme are safe",
          "[tk][view][pronouns]")
{
    PeStage s;
    s.ed->set_editable(true);
    vt::press(*s.ed, "+ Add language"); // incomplete local row
    s.ed->set_pronouns({{"fr", "elle", ""}});
    CHECK(s.ed->pronouns().size() == 2); // the unfinished row survives a refresh

    s.ed->set_busy(true);
    s.ed->set_error("save failed");
    CHECK_FALSE(vt::press(*s.ed, "+ Add language")); // disabled while busy
    s.ed->set_busy(false);
    s.ed->set_error("again");
    s.ed->set_compact(true);
    s.ed->on_theme_changed(tk::Theme::dark());
    s.run();
    auto lc = s.lc();
    CHECK(s.ed->measure(lc, {600, 700}).h > 0);
    s.ed->set_visible(false);
    s.run();
    s.ed->set_visible(true);
    s.ed->set_editable(false);
}

TEST_CASE("PronounsEditor without a host is inert", "[tk][view][pronouns]")
{
    auto ed = tk::create_root_widget<PronounsEditor>(nullptr);
    ed->set_pronouns({{"en", "x", ""}});
    ed->flush();
    CHECK(ed->pronouns().size() == 1);
}

// ── UserPackEditor ──────────────────────────────────────────────────────

TEST_CASE("UserPackEditor lists images as edit/remove rows",
          "[tk][view][user_pack]")
{
    UpeStage s;
    CHECK_FALSE(s.ed->has_changes());
    CHECK(s.ed->access_row_count() == 4);
    CHECK(s.ed->access_name_for_widget_row(0) == "Edit shortcode happy");
    CHECK(s.ed->access_name_for_widget_row(3) == "Remove sad");
    CHECK(s.ed->access_role_for_widget_row(0) == tk::Role::Button);
    CHECK(s.ed->access_rect_for_widget_row(0).w > 0);
    CHECK(s.ed->access_name_for_widget_row(99).empty());
    CHECK(s.ed->access_rect_for_widget_row(99).w == 0);
    CHECK(s.ed->focusable());
}

TEST_CASE("UserPackEditor removing an existing image records its shortcode",
          "[tk][view][user_pack]")
{
    UpeStage s;
    int layouts = 0;
    s.ed->on_layout_changed = [&] { ++layouts; };
    REQUIRE(s.ed->access_row_count() == 4);
    CHECK(s.ed->access_activate_widget_row(1)); // remove "happy"
    CHECK(s.ed->has_changes());
    CHECK(s.ed->images().size() == 1);
    CHECK(layouts >= 1);
    auto r = s.ed->build_result();
    CHECK(r.removed_shortcodes == std::vector<std::string>{"happy"});
    CHECK(r.images.size() == 1);
    CHECK_FALSE(s.ed->access_activate_widget_row(50));
}

TEST_CASE("UserPackEditor shortcode edit: commit, cancel and live text",
          "[tk][view][user_pack]")
{
    UpeStage s;
    CHECK(s.ed->access_activate_widget_row(0)); // edit "happy"
    CHECK(s.ed->shortcode_edit_initial_text() == "happy");
    CHECK(s.ed->shortcode_edit_reset_generation() == 1);
    s.run();
    CHECK(s.ed->shortcode_edit_rect().w > 0);
    s.ed->set_editing_shortcode_text("glad");
    s.ed->commit_editing_shortcode();
    CHECK(s.ed->has_changes());
    CHECK(s.ed->images()[0].shortcode == "glad");
    CHECK(s.ed->shortcode_edit_rect().w == 0);
    s.ed->commit_editing_shortcode(); // nothing being edited: no-op

    s.ed->access_activate_widget_row(2); // edit "sad"
    s.ed->set_editing_shortcode_text("changed");
    s.ed->cancel_editing_shortcode(); // reverts
    CHECK(s.ed->images()[1].shortcode == "sad");
    s.ed->cancel_editing_shortcode();
    s.ed->set_editing_shortcode_text("ignored"); // not editing: ignored
    CHECK(s.ed->images()[1].shortcode == "sad");
    CHECK(s.ed->shortcode_edit_initial_text().empty());
}

TEST_CASE("UserPackEditor pasted and dropped images become pending tiles",
          "[tk][view][user_pack]")
{
    UpeStage s;
    std::vector<std::uint64_t> added;
    s.ed->on_pending_image_added = [&](std::uint64_t id, const std::vector<std::uint8_t>& b,
                                       const std::string& mime)
    {
        added.push_back(id);
        CHECK_FALSE(b.empty());
        CHECK(mime == "image/png");
    };
    s.ed->add_pasted_image({1, 2, 3}, "image/png");
    s.ed->add_dropped_image({0, 0}, {4, 5}, "image/png", "party parrot.png");
    tk::FileDropPayload payload;
    payload.bytes = {9};
    payload.mime = "image/png";
    payload.filename = "dup.png";
    CHECK(s.ed->on_file_drop({0, 0}, payload));
    CHECK(added.size() == 3);
    CHECK(s.ed->images().size() == 5);
    CHECK(s.ed->has_changes());
    s.ed->set_tile_preview(added[0], nullptr);
    s.ed->set_tile_preview(9999, nullptr); // unknown id: ignored
    s.run();

    // While committing, additions and removals are refused.
    s.ed->set_committing(true);
    CHECK_FALSE(s.ed->focusable());
    s.ed->add_pasted_image({7}, "image/png");
    CHECK(s.ed->images().size() == 5);
    CHECK(s.ed->access_row_count() == 0);
    CHECK_FALSE(s.ed->access_activate_widget_row(1));
    s.ed->set_committing(false);
}

TEST_CASE("UserPackEditor pointer input: hover, wheel, drag-hover and focus "
          "keys",
          "[tk][view][user_pack]")
{
    UpeStage s;
    CHECK(s.ed->on_pointer_move({20, 20}));
    CHECK_FALSE(s.ed->on_pointer_move({22, 22}));
    s.ed->on_pointer_leave();
    CHECK(s.ed->on_native_drag_hover({10, 10}));
    CHECK(s.ed->drag_hover());
    s.ed->on_native_drag_leave();
    CHECK_FALSE(s.ed->drag_hover());
    s.ed->on_wheel({10, 10}, 0, 3, false);
    s.ed->on_pointer_drag({10, 10});
    s.ed->on_pointer_up({10, 10}, true);
    s.run();

    // Scan the tile area: a press on a label begins a shortcode edit.
    bool began = false;
    for (float y = 4; y < 300 && !began; y += 6)
        for (float x = 4; x < 470 && !began; x += 6)
        {
            if (s.ed->on_pointer_down({x, y}))
            {
                began = s.ed->shortcode_edit_reset_generation() > 0 || s.ed->has_changes();
                s.ed->cancel_editing_shortcode();
            }
        }
    CHECK(began);

    // Keyboard cursor: Down lands on the first stop and Enter activates it.
    s.host.request_focus(s.ed.get());
    CHECK(s.ed->has_focus());
    CHECK(s.ed->on_key_down(vt::key(tk::Key::Down)));
    s.ed->on_focus_lost();
    s.run();
}

TEST_CASE("UserPackEditor without a host has no shortcode field",
          "[tk][view][user_pack]")
{
    auto ed = tk::create_root_widget<UserPackEditor>(nullptr);
    ed->set_images({upe_img("a", "mxc://x/a")});
    ed->access_activate_widget_row(0);
    CHECK(ed->shortcode_edit_initial_text() == "a");
    ed->commit_editing_shortcode();
    CHECK(ed->has_changes());
}
