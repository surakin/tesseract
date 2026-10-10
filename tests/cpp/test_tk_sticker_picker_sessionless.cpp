#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/StickerPicker.h"

#include <tesseract/client.h>

#include <string>

using tesseract::views::StickerPicker;

namespace
{
struct SpkStage : vt::Stage
{
    std::unique_ptr<StickerPicker> picker =
        tk::create_root_widget<StickerPicker>(&host);
    tesseract::Client client; // no session: pack/favorite lists are empty
    SpkStage(int w = 360, int h = 420) : vt::Stage(w, h)
    {
        picker->set_client(&client);
        picker->set_current_room_id("!r:x");
        picker->set_current_room_parent_spaces({"!s:x"});
        mount(*picker, {0, 0, StickerPicker::kWidth, StickerPicker::kHeight});
    }
    void run()
    {
        relayout_paint(*picker, {0, 0, StickerPicker::kWidth, StickerPicker::kHeight});
    }
};
} // namespace

TEST_CASE("StickerPicker with an empty sessionless client has no packs",
          "[tk][view][sticker_picker]")
{
    SpkStage s;
    CHECK(s.picker->packs().empty());
    CHECK(s.picker->current().empty());
    CHECK(s.picker->active_tab() == 0);
    s.picker->refresh_packs();
    s.run();
}

TEST_CASE("StickerPicker search query switches to the search page and back",
          "[tk][view][sticker_picker]")
{
    SpkStage s;
    s.picker->set_search_query("cat");
    CHECK(s.picker->active_tab() == -1);
    CHECK(s.picker->current().empty());
    s.run();
    s.picker->set_search_query(""); // cleared with nothing to fall back to
    s.run();
    CHECK(s.picker->current().empty());
}

TEST_CASE("StickerPicker selection callback is not fired for an empty grid",
          "[tk][view][sticker_picker]")
{
    SpkStage s;
    int picked = 0;
    s.picker->on_selected = [&](const tesseract::ImagePackImage&) { ++picked; };
    s.picker->set_image_provider(
        [](const std::string&, const std::string&, bool) -> const tk::Image* { return nullptr; });
    s.picker->open_at({100, 100, 20, 20});
    s.run();
    s.host.dispatch_key_down(vt::key(tk::Key::Enter));
    s.host.dispatch_pointer_down({40, 100});
    s.host.dispatch_pointer_up({40, 100});
    s.picker->on_theme_changed(tk::Theme::dark());
    s.run();
    CHECK(picked == 0);
    s.picker->invalidate_image_cache();
    s.picker->set_visible(false);
}
