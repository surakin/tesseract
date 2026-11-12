#include <catch2/catch_test_macros.hpp>

#include "views/StickerController.h"
#include "views/StickerEngine.h"
#include "views/StickerPopup.h"

#include <string>
#include <vector>

using tesseract::ImagePack;
using tesseract::ImagePackImage;
using tesseract::views::search_stickers;
using tesseract::views::StickerController;
using tesseract::views::StickerEngine;
using tesseract::views::StickerPopup;

namespace
{
ImagePackImage sticker(const char* pack, const char* shortcode, const char* body = "")
{
    ImagePackImage s;
    s.pack_id = pack;
    s.shortcode = shortcode;
    s.body = body;
    s.url = std::string("mxc://srv/") + shortcode;
    return s;
}
} // namespace

TEST_CASE("StickerEngine matches /sticker <query>", "[sticker][engine]")
{
    StickerEngine e;
    REQUIRE(e.match("/sticker cat").has_value());
    CHECK(*e.match("/sticker cat") == "cat");
    CHECK(*e.match("/sticker  funny cat  ") == "funny cat");
    // Empty query is still a /sticker command (it owns the composer).
    REQUIRE(e.match("/sticker ").has_value());
    CHECK(e.match("/sticker ")->empty());
    CHECK_FALSE(e.match("/sticker").has_value());
    CHECK_FALSE(e.match("/stickers cat").has_value());
    CHECK_FALSE(e.match("/gif cat").has_value());
    CHECK_FALSE(e.match("hello /sticker cat").has_value());
}

TEST_CASE("search_stickers puts favourites first and dedups", "[sticker][engine]")
{
    ImagePack a;
    a.id = "a";
    ImagePack b;
    b.id = "b";
    auto images = [](const std::string& id) -> std::vector<ImagePackImage>
    {
        if (id == "a")
        {
            return {sticker("a", "cat_wave"), sticker("a", "dog")};
        }
        return {sticker("b", "wild_cat", "A Cat")};
    };
    std::vector<ImagePackImage> favs{sticker("b", "wild_cat", "A Cat")};

    auto out = search_stickers({a, b}, images, favs, "CAT");
    REQUIRE(out.size() == 2);
    CHECK(out[0].shortcode == "wild_cat"); // favourite first
    CHECK(out[1].shortcode == "cat_wave");
}

TEST_CASE("search_stickers matches body or shortcode", "[sticker][engine]")
{
    ImagePack a;
    a.id = "a";
    auto images = [](const std::string&) -> std::vector<ImagePackImage>
    {
        return {sticker("a", "x1", "Happy Dance"), sticker("a", "party", "")};
    };
    CHECK(search_stickers({a}, images, {}, "dance").size() == 1);
    CHECK(search_stickers({a}, images, {}, "PART").size() == 1);
    CHECK(search_stickers({a}, images, {}, "zzz").empty());
}

TEST_CASE("StickerPopup selection, cap and status", "[sticker][popup]")
{
    StickerPopup p;
    std::vector<ImagePackImage> v;
    v.reserve(StickerPopup::kMaxCells + 5);
    for (int i = 0; i < StickerPopup::kMaxCells + 5; ++i)
    {
        v.push_back(sticker("a", std::to_string(i).c_str()));
    }
    p.set_results(v);
    CHECK(p.visible_count() == StickerPopup::kMaxCells);
    CHECK(p.selected_index() == 0);
    REQUIRE(p.selected() != nullptr);
    CHECK(p.selected()->shortcode == "0");
    p.move_selection(-5);
    CHECK(p.selected_index() == 0);
    p.move_selection(+3);
    CHECK(p.selected_index() == 3);

    p.set_status("No stickers found");
    CHECK(p.has_status());
    CHECK(p.visible_count() == 0);
    CHECK(p.selected() == nullptr);
    CHECK(p.content_size(800.0f).h == 2.0f * StickerPopup::kPad + StickerPopup::kStatusH);
}

namespace
{
struct StickerRig
{
    StickerPopup popup;
    std::unique_ptr<StickerController> ctl;
    std::vector<ImagePackImage> hits;
    std::vector<std::string> queries;
    std::vector<ImagePackImage> sent;
    int shown = 0, hidden = 0, cleared = 0;

    StickerRig()
    {
        StickerController::Hooks h;
        h.show = [this]
        {
            ++shown;
        };
        h.hide = [this]
        {
            ++hidden;
        };
        h.repaint = [] {};
        h.search = [this](const std::string& q)
        {
            queries.push_back(q);
            return hits;
        };
        h.send = [this](const ImagePackImage& s)
        {
            sent.push_back(s);
        };
        h.clear_composer = [this]
        {
            ++cleared;
        };
        ctl = std::make_unique<StickerController>(&popup, std::move(h));
    }
};
} // namespace

TEST_CASE("StickerController handles only /sticker text", "[sticker][controller]")
{
    StickerRig r;
    CHECK_FALSE(r.ctl->on_text_changed("hello"));
    CHECK_FALSE(r.ctl->on_text_changed("/gif cat"));
    CHECK(r.queries.empty());
    CHECK(r.ctl->on_text_changed("/sticker "));
    CHECK(r.queries.empty()); // empty query: no search, no strip
    CHECK_FALSE(r.ctl->visible());
}

TEST_CASE("StickerController shows results, nav and sends the selection", "[sticker][controller]")
{
    StickerRig r;
    r.hits = {sticker("a", "one", "One"), sticker("a", "two")};
    REQUIRE(r.ctl->on_text_changed("/sticker o"));
    CHECK(r.ctl->visible());
    CHECK(r.shown == 1);
    CHECK(r.queries.back() == "o");

    CHECK(r.ctl->on_nav(tk::NavKey::Tab));
    CHECK(r.popup.selected_index() == 1);
    CHECK(r.ctl->on_nav(tk::NavKey::Left));
    CHECK(r.popup.selected_index() == 0);
    CHECK(r.ctl->on_nav(tk::NavKey::Right));

    CHECK(r.ctl->on_submit());
    REQUIRE(r.sent.size() == 1);
    CHECK(r.sent[0].shortcode == "two");
    CHECK(r.cleared == 1);
    CHECK_FALSE(r.ctl->visible());
}

TEST_CASE("StickerController with no match shows status and Enter sends nothing",
          "[sticker][controller]")
{
    StickerRig r;
    REQUIRE(r.ctl->on_text_changed("/sticker zzzz"));
    CHECK(r.ctl->visible());
    CHECK(r.popup.has_status());
    CHECK(r.ctl->on_submit()); // consumed, so the text is not sent
    CHECK(r.sent.empty());
    CHECK(r.cleared == 0);
}

TEST_CASE("StickerController escape hides; leaving the command hides", "[sticker][controller]")
{
    StickerRig r;
    r.hits = {sticker("a", "one")};
    r.ctl->on_text_changed("/sticker o");
    CHECK(r.ctl->on_nav(tk::NavKey::Escape));
    CHECK_FALSE(r.ctl->visible());
    CHECK_FALSE(r.ctl->on_nav(tk::NavKey::Tab));

    r.ctl->on_text_changed("/sticker o");
    CHECK(r.ctl->visible());
    CHECK_FALSE(r.ctl->on_text_changed("plain"));
    CHECK_FALSE(r.ctl->visible());
    CHECK_FALSE(r.ctl->on_submit()); // not a /sticker command any more
}
