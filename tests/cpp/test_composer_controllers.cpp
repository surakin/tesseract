// Mention / Shortcode / Gif composer popup controllers: the glue between the
// composer TextArea, the suggestion popup and the shell hooks. Driven through
// the same public entry points the shells use (on_text_changed / on_nav /
// on_submit and the popup's accept callbacks).

#include <catch2/catch_test_macros.hpp>

#include "tk/text_area.h"
#include "tk/theme.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"
#include "views/GifController.h"
#include "views/GifPopup.h"
#include "views/MentionController.h"
#include "views/MentionPopup.h"
#include "views/ShortcodeController.h"
#include "views/ShortcodePopup.h"

#include <tesseract/client.h>

#include <functional>
#include <string>
#include <vector>

using namespace tesseract::views;

namespace
{

// Native composer stand-in: StubTextArea keeps the text but reports caret 0 and
// ignores pill insertion, so give it a caret that follows the text and make
// mention/emoticon insertion observable.
struct CtrlArea : StubTextArea
{
    int cursor_byte_pos() const override
    {
        return static_cast<int>(text_.size());
    }
    void set_cursor_byte_pos(int) override {}
    void insert_mention(int start, int end, const std::string& user_id,
                        const std::string& display_name, bool is_room,
                        const tk::Image*) override
    {
        mentions.push_back(is_room ? std::string("@room") : user_id);
        replace_range(start, end, "[" + display_name + "]");
    }
    void insert_emoticon(int start, int end, const std::string& shortcode,
                         const std::string& mxc, const tk::Image*) override
    {
        emoticons.push_back(mxc);
        replace_range(start, end, "<" + shortcode + ">");
    }
    std::vector<std::string> mentions;
    std::vector<std::string> emoticons;
};

struct CtrlHost : StubHost
{
    std::unique_ptr<tk::NativeTextArea> make_text_area() override
    {
        auto a = std::make_unique<CtrlArea>();
        native = a.get();
        return a;
    }
    CtrlArea* native = nullptr;
};

} // namespace

// ── MentionController ───────────────────────────────────────────────────────

namespace
{

struct MentionRig
{
    MentionRig()
    {
        // Arranging creates the (stub) native backend.
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        area->measure(lc, {200, 40});
        area->arrange(lc, {0, 0, 200, 40});
        REQUIRE(host.native != nullptr);
    }
    std::unique_ptr<TestSurface> surface = TestSurface::create(320, 200);
    CtrlHost host;
    std::unique_ptr<tk::TextArea> area =
        tk::create_root_widget<tk::TextArea>(&host, 40.0f);
    MentionPopup popup;
    tesseract::Client client; // session-less: get_room_members() -> {}
    int shown = 0, hidden = 0, sel_repaints = 0, async_runs = 0;
    int last_rows = 0;
    std::string room = "!r:x";
    bool with_client = true;
    bool async_inline = true;
    std::vector<std::function<void()>> deferred;
    std::unique_ptr<MentionController> ctl;

    void build()
    {
        MentionController::Hooks h;
        h.show = [this](tk::Rect, int rows)
        {
            ++shown;
            last_rows = rows;
        };
        h.hide = [this] { ++hidden; };
        h.repaint = [] {};
        h.repaint_selection = [this] { ++sel_repaints; };
        h.room_id = [this] { return room; };
        h.client = [this]() -> tesseract::Client*
        { return with_client ? &client : nullptr; };
        h.run_async = [this](std::function<void()> fn)
        {
            ++async_runs;
            if (async_inline)
            {
                fn();
            }
            else
            {
                deferred.push_back(std::move(fn));
            }
        };
        h.post_to_ui = [](std::function<void()> fn) { fn(); };
        ctl = std::make_unique<MentionController>(area.get(), nullptr, &popup,
                                                  std::move(h));
    }

    bool type(const std::string& t)
    {
        area->set_text(t);
        return ctl->on_text_changed(t, static_cast<int>(t.size()));
    }
};

} // namespace

TEST_CASE("MentionController: text without an @ prefix is not handled",
          "[composer_controllers][mention]")
{
    MentionRig r;
    r.build();
    CHECK_FALSE(r.type("hello"));
    CHECK_FALSE(r.type("mail me at foo@bar"));
    CHECK_FALSE(r.ctl->visible());
    CHECK(r.async_runs == 0);
}

TEST_CASE("MentionController: a bare @ fetches members then offers @room",
          "[composer_controllers][mention]")
{
    MentionRig r;
    r.build();
    CHECK(r.type("@"));
    CHECK(r.async_runs == 1);
    // The (empty) member fetch completed inline and re-ran the lookup.
    CHECK(r.ctl->visible());
    CHECK(r.shown == 1);
    CHECK(r.last_rows == 1);
}

TEST_CASE("MentionController: member cache is reused for the same room",
          "[composer_controllers][mention]")
{
    MentionRig r;
    r.build();
    r.type("@");
    REQUIRE(r.async_runs == 1);
    r.type("@ro");
    CHECK(r.async_runs == 1);
    CHECK(r.ctl->visible());
}

TEST_CASE("MentionController: a pending fetch is not duplicated",
          "[composer_controllers][mention]")
{
    MentionRig r;
    r.async_inline = false;
    r.build();
    CHECK(r.type("@"));
    CHECK(r.type("@r"));
    CHECK(r.async_runs == 1);
    CHECK_FALSE(r.ctl->visible());
    // Completing the fetch shows the popup for the current text.
    r.area->set_text("@r");
    for (auto& fn : r.deferred)
    {
        fn();
    }
    CHECK(r.ctl->visible());
}

TEST_CASE("MentionController: without a client the fetch is skipped",
          "[composer_controllers][mention]")
{
    MentionRig r;
    r.with_client = false;
    r.build();
    CHECK(r.type("@"));
    CHECK(r.async_runs == 0);
    CHECK_FALSE(r.ctl->visible());
}

TEST_CASE("MentionController: no matching candidate hides the popup",
          "[composer_controllers][mention]")
{
    MentionRig r;
    r.build();
    r.type("@");
    REQUIRE(r.ctl->visible());
    CHECK_FALSE(r.type("@zzzz"));
    CHECK_FALSE(r.ctl->visible());
    CHECK(r.hidden == 1);
}

TEST_CASE("MentionController: switching rooms refetches members",
          "[composer_controllers][mention]")
{
    MentionRig r;
    r.build();
    r.type("@");
    r.room = "!other:x";
    r.type("@");
    CHECK(r.async_runs == 2);
}

TEST_CASE("MentionController: submit accepts the selection into the composer",
          "[composer_controllers][mention]")
{
    MentionRig r;
    r.build();
    r.type("@ro");
    REQUIRE(r.ctl->visible());
    CHECK(r.ctl->on_submit());
    CHECK_FALSE(r.ctl->visible());
    CHECK(r.host.native->mentions == std::vector<std::string>{"@room"});
    CHECK(r.area->text() == "[@room]");
}

TEST_CASE("MentionController: navigation keys", "[composer_controllers][mention]")
{
    MentionRig r;
    r.build();
    // Hidden: nothing is consumed.
    CHECK_FALSE(r.ctl->on_nav(tk::NativeTextArea::NavKey::Down));
    CHECK_FALSE(r.ctl->on_submit());

    r.type("@");
    REQUIRE(r.ctl->visible());
    CHECK(r.ctl->on_nav(tk::NativeTextArea::NavKey::Down));
    CHECK(r.ctl->on_nav(tk::NativeTextArea::NavKey::Up));
    CHECK(r.sel_repaints == 2);
    CHECK_FALSE(r.ctl->on_nav(tk::NativeTextArea::NavKey::Left));
    CHECK_FALSE(r.ctl->on_nav(tk::NativeTextArea::NavKey::ShiftTab));
    CHECK(r.ctl->on_nav(tk::NativeTextArea::NavKey::Escape));
    CHECK_FALSE(r.ctl->visible());
}

TEST_CASE("MentionController: Tab accepts the highlighted candidate",
          "[composer_controllers][mention]")
{
    MentionRig r;
    r.build();
    r.type("@ro");
    REQUIRE(r.ctl->visible());
    CHECK(r.ctl->on_nav(tk::NativeTextArea::NavKey::Tab));
    CHECK_FALSE(r.ctl->visible());
    CHECK(r.host.native->mentions == std::vector<std::string>{"@room"});
    CHECK(r.area->text() == "[@room]");
}

TEST_CASE("MentionController: destroying while a fetch is in flight is safe",
          "[composer_controllers][mention]")
{
    MentionRig r;
    r.async_inline = false;
    r.build();
    r.type("@");
    r.ctl.reset();
    for (auto& fn : r.deferred)
    {
        CHECK_NOTHROW(fn());
    }
}

// ── ShortcodeController ─────────────────────────────────────────────────────

namespace
{

struct ShortcodeRig
{
    ShortcodeRig()
    {
        // Arranging creates the (stub) native backend.
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        area->measure(lc, {200, 40});
        area->arrange(lc, {0, 0, 200, 40});
        REQUIRE(host.native != nullptr);
    }
    std::unique_ptr<TestSurface> surface = TestSurface::create(320, 200);
    CtrlHost host;
    std::unique_ptr<tk::TextArea> area =
        tk::create_root_widget<tk::TextArea>(&host, 40.0f);
    ShortcodePopup popup;
    int shown = 0, hidden = 0, sel_repaints = 0;
    std::vector<tesseract::ImagePackImage> packs;
    std::vector<std::string> fetched;
    std::unique_ptr<ShortcodeController> ctl;

    void build()
    {
        ShortcodeController::Hooks h;
        h.show = [this](tk::Rect, int) { ++shown; };
        h.hide = [this] { ++hidden; };
        h.repaint = [] {};
        h.repaint_selection = [this] { ++sel_repaints; };
        h.emoticons = [this] { return packs; };
        h.fetch_image = [this](const std::string& u) { fetched.push_back(u); };
        ctl = std::make_unique<ShortcodeController>(area.get(), &popup,
                                                    std::move(h));
    }

    bool type(const std::string& t)
    {
        area->set_text(t);
        return ctl->on_text_changed(t, static_cast<int>(t.size()));
    }
};

} // namespace

TEST_CASE("ShortcodeController: one typed character does not open the popup",
          "[composer_controllers][shortcode]")
{
    ShortcodeRig r;
    r.build();
    CHECK_FALSE(r.type(":s"));
    CHECK_FALSE(r.ctl->visible());
    CHECK_FALSE(r.type("plain text"));
}

TEST_CASE("ShortcodeController: a two-character prefix suggests emoji",
          "[composer_controllers][shortcode]")
{
    ShortcodeRig r;
    r.build();
    CHECK(r.type(":sm"));
    CHECK(r.ctl->visible());
    CHECK(r.shown == 1);
    // Deleting back below the minimum hides it again.
    CHECK_FALSE(r.type(":s"));
    CHECK_FALSE(r.ctl->visible());
    CHECK(r.hidden == 1);
}

TEST_CASE("ShortcodeController: a prefix matching nothing stays closed",
          "[composer_controllers][shortcode]")
{
    ShortcodeRig r;
    r.build();
    CHECK_FALSE(r.type(":qzxqzxqzx"));
    CHECK_FALSE(r.ctl->visible());
}

TEST_CASE("ShortcodeController: submit replaces the prefix with the glyph",
          "[composer_controllers][shortcode]")
{
    ShortcodeRig r;
    r.build();
    r.type(":smile");
    REQUIRE(r.ctl->visible());
    CHECK(r.ctl->on_submit());
    CHECK_FALSE(r.ctl->visible());
    // The ":smile" text was replaced by an emoji (no colon left).
    CHECK(r.area->text().find(':') == std::string::npos);
    CHECK_FALSE(r.area->text().empty());
    CHECK(r.host.native->emoticons.empty());
}

TEST_CASE("ShortcodeController: a completed :shortcode: expands in place",
          "[composer_controllers][shortcode]")
{
    ShortcodeRig r;
    r.build();
    CHECK(r.type(":smile: "));
    CHECK(r.area->text().find(":smile:") == std::string::npos);
}

TEST_CASE("ShortcodeController: custom emoticons are suggested and prefetched",
          "[composer_controllers][shortcode]")
{
    ShortcodeRig r;
    tesseract::ImagePackImage img;
    img.pack_id = "p";
    img.shortcode = "partyparrot";
    img.url = "mxc://x/parrot";
    r.packs.push_back(img);
    r.build();
    CHECK(r.type(":partyp"));
    CHECK(r.ctl->visible());
    REQUIRE_FALSE(r.fetched.empty());
    CHECK(r.fetched[0] == "mxc://x/parrot");
    // Accepting a custom emoticon inserts a pill (no glyph) and closes.
    CHECK(r.ctl->on_submit());
    CHECK_FALSE(r.ctl->visible());
    CHECK(r.host.native->emoticons == std::vector<std::string>{"mxc://x/parrot"});
    CHECK(r.area->text() == "<partyparrot>");
}

TEST_CASE("ShortcodeController: navigation keys", "[composer_controllers][shortcode]")
{
    ShortcodeRig r;
    r.build();
    CHECK_FALSE(r.ctl->on_nav(tk::NavKey::Down));
    CHECK_FALSE(r.ctl->on_submit());

    r.type(":sm");
    REQUIRE(r.ctl->visible());
    CHECK(r.ctl->on_nav(tk::NavKey::Down));
    CHECK(r.ctl->on_nav(tk::NavKey::Up));
    CHECK(r.sel_repaints == 2);
    CHECK_FALSE(r.ctl->on_nav(tk::NavKey::Left));
    CHECK_FALSE(r.ctl->on_nav(tk::NavKey::ShiftTab));
    CHECK(r.ctl->on_nav(tk::NavKey::Escape));
    CHECK_FALSE(r.ctl->visible());
}

TEST_CASE("ShortcodeController: Tab accepts the highlighted suggestion",
          "[composer_controllers][shortcode]")
{
    ShortcodeRig r;
    r.build();
    r.type(":smile");
    REQUIRE(r.ctl->visible());
    CHECK(r.ctl->on_nav(tk::NavKey::Tab));
    CHECK_FALSE(r.ctl->visible());
}

// ── GifController ───────────────────────────────────────────────────────────

namespace
{

struct GifRig
{
    GifRig()
    {
        // Arranging creates the (stub) native backend.
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        area->measure(lc, {200, 40});
        area->arrange(lc, {0, 0, 200, 40});
        REQUIRE(host.native != nullptr);
    }
    std::unique_ptr<TestSurface> surface = TestSurface::create(320, 200);
    CtrlHost host;
    std::unique_ptr<tk::TextArea> area =
        tk::create_root_widget<tk::TextArea>(&host, 40.0f);
    GifPopup popup;
    tesseract::Client client;
    int shown = 0, hidden = 0, cleared = 0, sel_repaints = 0;
    std::string api_key = "key";
    std::string room = "!r:x";
    bool with_client = true;
    std::vector<std::pair<int, std::function<void()>>> delayed;
    std::unique_ptr<GifController> ctl;

    void build()
    {
        GifController::Hooks h;
        h.show = [this] { ++shown; };
        h.hide = [this] { ++hidden; };
        h.repaint = [] {};
        h.repaint_selection = [this] { ++sel_repaints; };
        h.room_id = [this] { return room; };
        h.client = [this]() -> tesseract::Client*
        { return with_client ? &client : nullptr; };
        h.post_delayed = [this](int ms, std::function<void()> fn)
        { delayed.emplace_back(ms, std::move(fn)); };
        h.api_key = [this] { return api_key; };
        h.client_key = [] { return std::string("ck"); };
        h.clear_composer = [this] { ++cleared; };
        ctl = std::make_unique<GifController>(area.get(), &popup, std::move(h));
    }

    void fire_last_delayed()
    {
        REQUIRE_FALSE(delayed.empty());
        auto fn = delayed.back().second;
        fn();
    }

    // Deliver `results` to whichever process-global request id the controller
    // just issued (the id counter is static and shared, so sweep for it).
    bool deliver(std::vector<tesseract::GifResult> results)
    {
        for (std::uint64_t id = 1; id < 20000; ++id)
        {
            const int before = shown;
            ctl->on_results(id, results);
            if (shown != before)
            {
                return true;
            }
        }
        return false;
    }
};

tesseract::GifResult cc_gif(const char* id)
{
    tesseract::GifResult g;
    g.id = id;
    g.image_url = std::string("https://x/") + id + ".webp";
    g.image_mime = "image/webp";
    g.preview_url = std::string("https://x/") + id + "-p.webp";
    return g;
}

} // namespace

TEST_CASE("GifController: only a /gif <query> command is handled",
          "[composer_controllers][gif]")
{
    GifRig r;
    r.build();
    CHECK_FALSE(r.ctl->on_text_changed("hello"));
    CHECK_FALSE(r.ctl->on_text_changed("/gif "));
    CHECK(r.delayed.empty());
    CHECK(r.ctl->on_text_changed("/gif cats"));
    REQUIRE(r.delayed.size() == 1);
    CHECK(r.delayed[0].first == 300);
}

TEST_CASE("GifController: a superseded debounce does not search",
          "[composer_controllers][gif]")
{
    GifRig r;
    r.api_key.clear(); // a search would surface the missing-key status
    r.build();
    r.ctl->on_text_changed("/gif c");
    r.ctl->on_text_changed("/gif ca");
    REQUIRE(r.delayed.size() == 2);
    r.delayed[0].second(); // stale generation
    CHECK_FALSE(r.ctl->visible());
    r.delayed[1].second();
    CHECK(r.ctl->visible());
}

TEST_CASE("GifController: a missing API key is reported, not silent",
          "[composer_controllers][gif]")
{
    GifRig r;
    r.api_key.clear();
    r.build();
    r.ctl->on_text_changed("/gif cats");
    r.fire_last_delayed();
    CHECK(r.ctl->visible());
    CHECK(r.shown == 1);
}

TEST_CASE("GifController: no client means no search and no popup",
          "[composer_controllers][gif]")
{
    GifRig r;
    r.with_client = false;
    r.build();
    r.ctl->on_text_changed("/gif cats");
    r.fire_last_delayed();
    CHECK_FALSE(r.ctl->visible());
}

TEST_CASE("GifController: results for the issued search open the strip",
          "[composer_controllers][gif]")
{
    GifRig r;
    r.build();
    r.ctl->on_text_changed("/gif cats");
    r.fire_last_delayed();
    REQUIRE_FALSE(r.ctl->visible());
    REQUIRE(r.deliver({cc_gif("a"), cc_gif("b")}));
    CHECK(r.ctl->visible());
    CHECK(r.popup.visible_count() == 2);
}

TEST_CASE("GifController: stale or unknown request ids are dropped",
          "[composer_controllers][gif]")
{
    GifRig r;
    r.build();
    r.ctl->on_text_changed("/gif cats");
    r.fire_last_delayed();
    r.ctl->on_results(0, {cc_gif("a")});
    r.ctl->on_search_failed(0, "x");
    CHECK_FALSE(r.ctl->visible());
}

TEST_CASE("GifController: empty results show a status instead of the strip",
          "[composer_controllers][gif]")
{
    GifRig r;
    r.build();
    r.ctl->on_text_changed("/gif cats");
    r.fire_last_delayed();
    CHECK(r.deliver({}));
    CHECK(r.ctl->visible());
    CHECK(r.popup.visible_count() == 0);
}

TEST_CASE("GifController: navigation and escape", "[composer_controllers][gif]")
{
    GifRig r;
    r.build();
    CHECK_FALSE(r.ctl->on_nav(tk::NavKey::Right));
    CHECK_FALSE(r.ctl->on_submit());

    r.ctl->on_text_changed("/gif cats");
    r.fire_last_delayed();
    REQUIRE(r.deliver({cc_gif("a"), cc_gif("b"), cc_gif("c")}));
    CHECK(r.ctl->on_nav(tk::NavKey::Right));
    CHECK(r.popup.selected_index() == 1);
    CHECK(r.ctl->on_nav(tk::NavKey::Tab));
    CHECK(r.popup.selected_index() == 2);
    CHECK(r.ctl->on_nav(tk::NavKey::Left));
    CHECK(r.popup.selected_index() == 1);
    CHECK(r.ctl->on_nav(tk::NavKey::ShiftTab));
    CHECK(r.popup.selected_index() == 0);
    CHECK(r.sel_repaints == 4);
    CHECK(r.ctl->on_nav(tk::NavKey::Escape));
    CHECK_FALSE(r.ctl->visible());
    CHECK(r.popup.visible_count() == 0);
}

TEST_CASE("GifController: submitting sends the selection and clears the composer",
          "[composer_controllers][gif]")
{
    GifRig r;
    r.build();
    r.ctl->on_text_changed("/gif cats");
    r.fire_last_delayed();
    REQUIRE(r.deliver({cc_gif("a")}));
    CHECK(r.ctl->on_submit());
    CHECK_FALSE(r.ctl->visible());
    CHECK(r.cleared == 1);
}

TEST_CASE("GifController: submit with no room does nothing destructive",
          "[composer_controllers][gif]")
{
    GifRig r;
    r.build();
    r.ctl->on_text_changed("/gif cats");
    r.fire_last_delayed();
    REQUIRE(r.deliver({cc_gif("a")}));
    r.room.clear();
    CHECK(r.ctl->on_submit());
    CHECK(r.cleared == 0);
    CHECK(r.ctl->visible());
}

TEST_CASE("GifController: a failed search for the current request shows a status",
          "[composer_controllers][gif]")
{
    GifRig r;
    r.build();
    r.ctl->on_text_changed("/gif cats");
    r.fire_last_delayed();
    bool shown = false;
    for (std::uint64_t id = 1; id < 20000 && !shown; ++id)
    {
        r.ctl->on_search_failed(id, "boom");
        shown = r.ctl->visible();
    }
    CHECK(shown);
}

TEST_CASE("GifController: debounce firing after destruction is safe",
          "[composer_controllers][gif]")
{
    GifRig r;
    r.build();
    r.ctl->on_text_changed("/gif cats");
    r.ctl.reset();
    for (auto& d : r.delayed)
    {
        CHECK_NOTHROW(d.second());
    }
}
