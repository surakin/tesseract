// MessageListView gallery cells: click-to-open, hidden-gallery reveal, hit
// geometry lifetime and the accessibility actions.

#include <catch2/catch_test_macros.hpp>

#include "access_test_util.h"
#include "tk/access_tree.h"
#include "tk/canvas.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "views/MessageListView.h"

#include <tesseract/media_source.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

using namespace tk;
using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;

namespace
{

struct GalStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(600, 400);
    void run(Widget& root, Rect bounds = {0, 0, 600, 400})
    {
        LayoutCtx lc{surface->factory(), Theme::light()};
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
        root.paint(pc);
    }
};

MessageRowData gallery_row(std::string eid)
{
    using Row = MessageRowData::GalleryItemRow;
    MessageRowData m;
    m.kind = MessageRowData::Kind::Gallery;
    m.event_id = std::move(eid);
    m.sender_name = "Alice";
    m.body = "holiday";
    for (int i = 0; i < 3; ++i)
    {
        Row r;
        r.kind = i == 1 ? Row::Kind::Video : Row::Kind::Image;
        r.body = "item" + std::to_string(i);
        r.source = tesseract::MediaSource::plain("mxc://x/g" + std::to_string(i));
        r.media_w = 640;
        r.media_h = 480;
        m.gallery_items.push_back(std::move(r));
    }
    m.gallery_total = 3;
    return m;
}

// Centre of gallery cell `i` after a paint pass, probing the recorded geometry.
bool cell_centre(const MessageListView& v, std::size_t i, Point& out)
{
    for (float y = 0.0f; y < 400.0f; y += 4.0f)
    {
        for (float x = 0.0f; x < 600.0f; x += 4.0f)
        {
            auto hit = v.gallery_item_hit_at({x, y});
            if (hit && hit->item_index == i)
            {
                out = {hit->world_rect.x + hit->world_rect.w * 0.5f,
                       hit->world_rect.y + hit->world_rect.h * 0.5f};
                return true;
            }
        }
    }
    return false;
}

} // namespace

TEST_CASE("clicking a gallery cell fires on_gallery_item_clicked with the index and items",
          "[message_list][gallery][click]")
{
    GalStage st;
    MessageListView view;
    view.set_messages({gallery_row("$g1")});
    st.run(view);

    std::vector<MessageListView::GalleryHit> fired;
    view.on_gallery_item_clicked = [&](const MessageListView::GalleryHit& h)
    {
        fired.push_back(h);
    };

    Point c{};
    REQUIRE(cell_centre(view, 1, c));
    REQUIRE(view.on_pointer_down(c));
    view.on_pointer_up(c, true);
    REQUIRE(fired.size() == 1);
    CHECK(fired[0].event_id == "$g1");
    CHECK(fired[0].index == 1);
    CHECK(fired[0].caption == "holiday");
    REQUIRE(fired[0].items.size() == 3);
    CHECK(fired[0].items[1].kind == MessageRowData::GalleryItemRow::Kind::Video);

    // Releasing outside the cell does not fire.
    REQUIRE(view.on_pointer_down(c));
    view.on_pointer_up(c, false);
    CHECK(fired.size() == 1);
}

TEST_CASE("a gallery past the painted cap opens the viewer on every stored item",
          "[message_list][gallery][click]")
{
    GalStage st;
    st.surface = TestSurface::create(600, 1600);
    MessageListView view;
    MessageRowData m = gallery_row("$gbig");
    m.gallery_items.clear();
    using Row = MessageRowData::GalleryItemRow;
    for (int i = 0; i < 30; ++i)
    {
        Row r;
        r.kind = Row::Kind::Image;
        r.source = tesseract::MediaSource::plain("mxc://x/b" + std::to_string(i));
        m.gallery_items.push_back(std::move(r));
    }
    m.gallery_total = 45;
    REQUIRE(m.gallery_painted_count() == MessageRowData::kMaxGalleryItems);
    view.set_messages({m});
    st.run(view, {0, 0, 600, 1600});

    std::vector<MessageListView::GalleryHit> fired;
    view.on_gallery_item_clicked = [&](const MessageListView::GalleryHit& h)
    {
        fired.push_back(h);
    };

    auto find_cell = [&](std::size_t i, Point& out)
    {
        for (float y = 0.0f; y < 1600.0f; y += 4.0f)
        {
            for (float x = 0.0f; x < 600.0f; x += 4.0f)
            {
                auto hit = view.gallery_item_hit_at({x, y});
                if (hit && hit->item_index == i)
                {
                    out = {hit->world_rect.x + 2.0f, hit->world_rect.y + 2.0f};
                    return true;
                }
            }
        }
        return false;
    };
    Point c{};
    // Only the first kMaxGalleryItems cells are painted; the last is the "+N" tile.
    CHECK_FALSE(find_cell(MessageRowData::kMaxGalleryItems, c));
    REQUIRE(find_cell(MessageRowData::kMaxGalleryItems - 1, c));
    REQUIRE(view.on_pointer_down(c));
    view.on_pointer_up(c, true);
    REQUIRE(fired.size() == 1);
    CHECK(fired[0].index == MessageRowData::kMaxGalleryItems - 1);
    // The viewer still gets all 30 stored items to step through.
    CHECK(fired[0].items.size() == 30);
}

TEST_CASE("clicking a hidden gallery reveals it instead of opening", 
          "[message_list][gallery][click]")
{
    GalStage st;
    MessageListView view;
    view.set_media_hidden_predicate(
        [](const std::string&, bool) { return true; });
    view.set_messages({gallery_row("$g2")});
    st.run(view);

    std::string revealed;
    int opened = 0;
    view.on_reveal_media = [&](const std::string& eid) { revealed = eid; };
    view.on_gallery_item_clicked = [&](const MessageListView::GalleryHit&) { ++opened; };

    Point c{};
    REQUIRE(cell_centre(view, 0, c)); // the placeholder is recorded as item 0
    REQUIRE(view.on_pointer_down(c));
    view.on_pointer_up(c, true);
    CHECK(revealed == "$g2");
    CHECK(opened == 0);
}

TEST_CASE("a room switch clears the gallery hit geometry", "[message_list][gallery]")
{
    GalStage st;
    MessageListView view;
    view.set_messages({gallery_row("$g3")});
    st.run(view);
    Point c{};
    REQUIRE(cell_centre(view, 0, c));
    REQUIRE(view.gallery_item_hit_at(c).has_value());

    view.begin_switch_loading();
    CHECK_FALSE(view.gallery_item_hit_at(c).has_value());
}

TEST_CASE("a gallery row offers an Open gallery accessibility action",
          "[message_list][gallery][accessibility]")
{
    GalStage st;
    MessageListView view;
    view.set_messages({gallery_row("$g4")});
    st.run(view);

    std::vector<MessageListView::GalleryHit> fired;
    view.on_gallery_item_clicked = [&](const MessageListView::GalleryHit& h)
    {
        fired.push_back(h);
    };

    AccessNode tree = build_access_tree(&view);
    const AccessNode* open = access_test::find_named(tree, "Open gallery");
    REQUIRE(open != nullptr);
    REQUIRE(open->activate);
    CHECK(open->activate());
    REQUIRE(fired.size() == 1);
    CHECK(fired[0].index == 0);
    CHECK(fired[0].event_id == "$g4");
}

TEST_CASE("a hidden gallery row offers Show hidden media instead",
          "[message_list][gallery][accessibility]")
{
    GalStage st;
    MessageListView view;
    view.set_media_hidden_predicate([](const std::string&, bool) { return true; });
    view.set_messages({gallery_row("$g5")});
    st.run(view);

    std::string revealed;
    view.on_reveal_media = [&](const std::string& eid) { revealed = eid; };

    AccessNode tree = build_access_tree(&view);
    CHECK(access_test::find_named(tree, "Open gallery") == nullptr);
    const AccessNode* show = access_test::find_named(tree, "Show hidden media");
    REQUIRE(show != nullptr);
    REQUIRE(show->activate);
    CHECK(show->activate());
    CHECK(revealed == "$g5");
}
