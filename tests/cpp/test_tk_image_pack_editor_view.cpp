#include <catch2/catch_test_macros.hpp>

#include "views/ImagePackEditorView.h"
#include "tk_test_surface.h"

#include <tesseract/image_pack.h>

using tesseract::views::ImagePackEditorView;
using tesseract::views::ImagePackEditorResult;

namespace
{

struct TkImagePackEditorStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 600);
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

tesseract::ImagePack make_pack(std::string id, std::string name,
                               const std::string& room_id)
{
    tesseract::ImagePack p;
    // Distinct per-pack state_key (mirroring `id`, not production's
    // "room:!id/key" format — this is a test fixture) so removed_state_keys
    // can tell which of several packs in a fixture was actually removed.
    p.source_state_key = id;
    p.id = std::move(id);
    p.display_name = std::move(name);
    p.usage = tesseract::PackUsage::Any;
    p.source_kind = tesseract::PackSourceKind::Room;
    p.source_room = room_id;
    return p;
}

tesseract::ImagePackImage make_image(std::string pack_id, std::string shortcode,
                                     std::string url)
{
    tesseract::ImagePackImage img;
    img.pack_id = std::move(pack_id);
    img.shortcode = std::move(shortcode);
    img.url = std::move(url);
    img.usage = tesseract::PackUsage::Any;
    return img;
}

// â”€â”€ Geometry mirrors ImagePackEditorView/ImagePackSectionList's private
// layout constants for an 800x600 stage. See ImagePackEditorView.cpp's
// arrange()/compute_layout_() for the derivation:
//   editor: kPadY=16, kLabelH=16, kLabelGap=4, kRowH=32
//     -> new-pack row at y=16+16+4=36, height 32; list starts at
//        y=36+32+16=84
//   section list: grid_w = 800-2*8=784; cols = floor((784+8)/(96+8)) = 7
constexpr float kListTop        = 84.0f;
constexpr float kHeaderH        = 40.0f;
constexpr float kTileSize       = 96.0f;
constexpr float kTileSpacing    = 8.0f;
constexpr float kTilePad        = 8.0f;
constexpr float kSectionGap     = 16.0f;
constexpr float kImageH         = 76.0f;
constexpr float kLabelH         = 20.0f;
constexpr float kRemoveChipR    = 9.0f;
constexpr float kHeaderRemoveR  = 10.0f;
constexpr float kUsageSegW      = 52.0f;
constexpr float kUsageSegH      = 24.0f;
constexpr float kUsageSegGap    = 4.0f;
constexpr float kHeaderPadX     = 12.0f;
constexpr std::size_t kCols     = 7;

float section_height(std::size_t image_count)
{
    const std::size_t tile_count = image_count + 1; // + hint tile
    const std::size_t rows = (tile_count + kCols - 1) / kCols;
    const float grid_h =
        kTilePad * 2.0f +
        (rows > 0 ? static_cast<float>(rows) * kTileSize +
                        static_cast<float>(rows - 1) * kTileSpacing
                  : 0.0f);
    return kHeaderH + grid_h + kSectionGap;
}

float section_top_world(const std::vector<std::size_t>& image_counts,
                        std::size_t pack_idx)
{
    float y = kListTop;
    for (std::size_t i = 0; i < pack_idx; ++i)
        y += section_height(image_counts[i]);
    return y;
}

// World-space center of pack `pack_idx`'s header name area (safe click
// target: well inside the label, left of the usage toggle).
tk::Point header_name_point(const std::vector<std::size_t>& image_counts,
                            std::size_t pack_idx)
{
    const float top = section_top_world(image_counts, pack_idx);
    return {100.0f, top + kHeaderH * 0.5f};
}

tk::Point header_remove_chip_point(const std::vector<std::size_t>& image_counts,
                                   std::size_t pack_idx)
{
    const float top = section_top_world(image_counts, pack_idx);
    const float cx = 800.0f - kHeaderPadX - kHeaderRemoveR;
    return {cx, top + kHeaderH * 0.5f};
}

// seg: 0=Any, 1=Emoticon, 2=Sticker.
tk::Point header_usage_segment_point(const std::vector<std::size_t>& image_counts,
                                     std::size_t pack_idx, int seg)
{
    const float top = section_top_world(image_counts, pack_idx);
    const float chip_right = 800.0f - kHeaderPadX;
    const float chip_left  = chip_right - kHeaderRemoveR * 2.0f;
    float seg_right = chip_left - kUsageSegGap;
    for (int s = 2; s > seg; --s)
        seg_right -= kUsageSegGap + kUsageSegW;
    const float seg_left = seg_right - kUsageSegW;
    return {(seg_left + seg_right) * 0.5f, top + kHeaderH * 0.5f};
}

tk::Point tile_remove_chip_point(const std::vector<std::size_t>& image_counts,
                                 std::size_t pack_idx, std::size_t tile_idx)
{
    const float top = section_top_world(image_counts, pack_idx);
    const std::size_t row = tile_idx / kCols;
    const std::size_t col = tile_idx % kCols;
    const float cell_x = kTilePad + static_cast<float>(col) * (kTileSize + kTileSpacing);
    const float cell_y =
        top + kHeaderH + kTilePad + static_cast<float>(row) * (kTileSize + kTileSpacing);
    return {cell_x + kTileSize - kRemoveChipR, cell_y + kRemoveChipR};
}

tk::Point tile_label_point(const std::vector<std::size_t>& image_counts,
                           std::size_t pack_idx, std::size_t tile_idx)
{
    const float top = section_top_world(image_counts, pack_idx);
    const std::size_t row = tile_idx / kCols;
    const std::size_t col = tile_idx % kCols;
    const float cell_x = kTilePad + static_cast<float>(col) * (kTileSize + kTileSpacing);
    const float cell_y =
        top + kHeaderH + kTilePad + static_cast<float>(row) * (kTileSize + kTileSpacing);
    return {cell_x + kTileSize * 0.5f, cell_y + kImageH + kLabelH * 0.5f};
}

} // namespace

TEST_CASE("ImagePackEditorView: closed by default", "[image_pack][view]")
{
    ImagePackEditorView v;
    CHECK_FALSE(v.is_open());
    CHECK(v.new_pack_name_field_rect().empty());
    CHECK(v.shortcode_edit_rect().empty());
    CHECK(v.list_rect().empty());
    CHECK(v.packs().empty());
}

TEST_CASE("ImagePackEditorView: open() opens the view for the given room",
         "[image_pack][view]")
{
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    CHECK(v.is_open());
    CHECK(v.room_id() == "!room:example.org");
}

TEST_CASE("ImagePackEditorView: no packs leaves the list empty and no "
         "active pack",
         "[image_pack][view]")
{
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({});

    CHECK(v.packs().empty());
    CHECK_FALSE(v.active_pack_index().has_value());
}

TEST_CASE("ImagePackEditorView: available packs are all listed at once, "
         "the first is active, and on_pack_images_needed fires for each",
         "[image_pack][view]")
{
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);

    std::vector<std::string> needed;
    v.on_pack_images_needed = [&](std::string pack_id)
    { needed.push_back(std::move(pack_id)); };

    std::vector<tesseract::ImagePack> packs;
    packs.push_back(make_pack("p1", "Emotes", "!room:example.org"));
    packs.push_back(make_pack("p2", "Stickers", "!room:example.org"));
    v.set_available_packs(packs);

    REQUIRE(v.packs().size() == 2);
    CHECK(v.packs()[0].pack_id == "p1");
    CHECK(v.packs()[1].pack_id == "p2");
    REQUIRE(v.active_pack_index().has_value());
    CHECK(*v.active_pack_index() == 0);
    REQUIRE(needed.size() == 2);
    CHECK(needed[0] == "p1");
    CHECK(needed[1] == "p2");
}

TEST_CASE("ImagePackEditorView: defaults to read-only on open() until "
         "set_field_permissions(true)",
         "[image_pack][view]")
{
    TkImagePackEditorStage st;
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_available_packs({});

    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    REQUIRE(v.create_button() != nullptr);
    CHECK_FALSE(v.create_button()->enabled());
    CHECK(v.new_pack_name_field_rect().empty());

    // Create button is disabled -> not even hit-testable.
    v.set_new_pack_name_text("New Pack");
    const tk::Point create_pt{800.0f - 24.0f - 44.0f, 16.0f + 20.0f + 16.0f};
    CHECK(v.dispatch_pointer_down(create_pt) == nullptr);
    CHECK(v.packs().empty());

    v.set_field_permissions(true);
    CHECK(v.create_button()->enabled());
    CHECK_FALSE(v.new_pack_name_field_rect().empty());

    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
    tk::Widget* create_hit = v.dispatch_pointer_down(create_pt);
    REQUIRE(create_hit != nullptr);
    create_hit->on_pointer_up(create_hit->world_to_local(create_pt),
                              /*inside_self=*/true);
    CHECK(v.packs().size() == 1);
}

TEST_CASE("ImagePackEditorView: set_field_permissions(false) disables "
         "remove/usage-change/rename/paste but header click still selects "
         "the pack as active",
         "[image_pack][view]")
{
    TkImagePackEditorStage st;
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org")});
    v.set_field_permissions(false);

    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    const std::vector<std::size_t> counts{0};

    // Remove chip click falls through to header-click-select-active instead.
    const tk::Point remove_pt = header_remove_chip_point(counts, 0);
    tk::Widget* remove_hit = v.dispatch_pointer_down(remove_pt);
    REQUIRE(remove_hit != nullptr);
    CHECK(v.packs().size() == 1);
    REQUIRE(v.active_pack_index().has_value());
    CHECK(*v.active_pack_index() == 0);

    // Usage-segment click is a no-op (also falls through to header-select).
    const tk::Point usage_pt = header_usage_segment_point(counts, 0, /*seg=*/2);
    v.dispatch_pointer_down(usage_pt);
    CHECK(v.packs()[0].usage == tesseract::PackUsage::Any);

    // Paste/drop is a no-op.
    v.add_pending_image_to_active({1, 2, 3}, "image/png");
    CHECK(v.packs()[0].images.empty());
    CHECK_FALSE(v.has_changes());

    // Re-enabling permission restores normal behavior.
    v.set_field_permissions(true);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
    tk::Widget* hit2 = v.dispatch_pointer_down(remove_pt);
    REQUIRE(hit2 != nullptr);
    CHECK(v.packs().empty());
}

TEST_CASE("ImagePackEditorView: set_pack_images populates the matching "
         "pack only",
         "[image_pack][view]")
{
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org"),
                           make_pack("p2", "Stickers", "!room:example.org")});

    v.set_pack_images("p2",
                      {make_image("p2", "wave", "mxc://example.org/1")});

    REQUIRE(v.packs()[0].images.empty());
    REQUIRE(v.packs()[1].images.size() == 1);
    CHECK(v.packs()[1].images[0].shortcode == "wave");
}

TEST_CASE("ImagePackEditorView: set_pack_images for an unknown pack id is "
         "ignored",
         "[image_pack][view]")
{
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org")});

    v.set_pack_images("does-not-exist",
                      {make_image("x", "wave", "mxc://example.org/1")});

    CHECK(v.packs()[0].images.empty());
}

TEST_CASE("ImagePackEditorView: clicking a pack header selects it as active",
         "[image_pack][view]")
{
    TkImagePackEditorStage st;
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org"),
                           make_pack("p2", "Stickers", "!room:example.org")});
    REQUIRE(*v.active_pack_index() == 0);

    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    const std::vector<std::size_t> counts{0, 0};
    const tk::Point pt = header_name_point(counts, 1);
    tk::Widget* hit = v.dispatch_pointer_down(pt);
    REQUIRE(hit != nullptr);

    REQUIRE(v.active_pack_index().has_value());
    CHECK(*v.active_pack_index() == 1);
}

TEST_CASE("ImagePackEditorView: clicking a header's usage segment sets "
         "that pack's usage",
         "[image_pack][view]")
{
    TkImagePackEditorStage st;
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org")});
    REQUIRE(v.packs()[0].usage == tesseract::PackUsage::Any);

    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    const std::vector<std::size_t> counts{0};
    {
        const tk::Point pt = header_usage_segment_point(counts, 0, /*seg=*/2);
        tk::Widget* hit = v.dispatch_pointer_down(pt);
        REQUIRE(hit != nullptr);
        CHECK(v.packs()[0].usage == tesseract::PackUsage::Sticker);
    }
    {
        const tk::Point pt = header_usage_segment_point(counts, 0, /*seg=*/1);
        tk::Widget* hit = v.dispatch_pointer_down(pt);
        REQUIRE(hit != nullptr);
        CHECK(v.packs()[0].usage == tesseract::PackUsage::Emoticon);
    }
}

TEST_CASE("ImagePackEditorView: clicking a header's remove chip deletes "
         "that pack and records it as removed",
         "[image_pack][view]")
{
    TkImagePackEditorStage st;
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org"),
                           make_pack("p2", "Stickers", "!room:example.org")});

    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    const std::vector<std::size_t> counts{0, 0};
    const tk::Point pt = header_remove_chip_point(counts, 0);
    tk::Widget* hit = v.dispatch_pointer_down(pt);
    REQUIRE(hit != nullptr);

    REQUIRE(v.packs().size() == 1);
    CHECK(v.packs()[0].pack_id == "p2");
    // active_pack_index_ pointed at the removed pack (0) -> cleared.
    CHECK_FALSE(v.active_pack_index().has_value());

    // build_result() is what RoomSettingsView's shared Accept click reads â€”
    // this view no longer has its own Accept button to click.
    const ImagePackEditorResult accepted = v.build_result();
    REQUIRE(accepted.removed_state_keys.size() == 1);
    CHECK(accepted.removed_state_keys[0] == "p1");
}

TEST_CASE("ImagePackEditorView: removing a newly-created pack does not "
         "record it as removed (it never existed server-side)",
         "[image_pack][view]")
{
    TkImagePackEditorStage st;
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({});

    v.set_new_pack_name_text("New Pack");
    REQUIRE(v.create_button() != nullptr);
    // Drive creation through a real click dispatch (press+release), mirroring
    // Accept/Cancel dispatch elsewhere in this file.
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Create button sits right after the name field, near the top.
    const tk::Point create_pt{800.0f - 24.0f - 44.0f, 16.0f + 20.0f + 16.0f};
    tk::Widget* chit = v.dispatch_pointer_down(create_pt);
    REQUIRE(chit != nullptr);
    chit->on_pointer_up(chit->world_to_local(create_pt), /*inside_self=*/true);

    REQUIRE(v.packs().size() == 1);
    CHECK(v.packs()[0].is_new);
    CHECK(v.packs()[0].display_name == "New Pack");
    REQUIRE(v.active_pack_index().has_value());
    CHECK(*v.active_pack_index() == 0);
    CHECK(v.new_pack_name_reset_generation() == 1);

    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
    const std::vector<std::size_t> counts{0};
    const tk::Point rpt = header_remove_chip_point(counts, 0);
    tk::Widget* rhit = v.dispatch_pointer_down(rpt);
    REQUIRE(rhit != nullptr);

    CHECK(v.packs().empty());

    const ImagePackEditorResult accepted = v.build_result();
    CHECK(accepted.removed_state_keys.empty());
}

TEST_CASE("ImagePackEditorView: add_pending_image_to_active targets the "
         "active pack",
         "[image_pack][view]")
{
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org"),
                           make_pack("p2", "Stickers", "!room:example.org")});
    REQUIRE(*v.active_pack_index() == 0);

    std::uint64_t added_id = 0;
    v.on_pending_image_added = [&](std::uint64_t local_id,
                                   const std::vector<std::uint8_t>&,
                                   const std::string&)
    { added_id = local_id; };

    v.add_pending_image_to_active({1, 2, 3}, "image/png");

    REQUIRE(added_id != 0);
    CHECK(v.packs()[0].images.size() == 1);
    CHECK(v.packs()[1].images.empty());
    CHECK(v.packs()[0].images[0].local_id == added_id);
    CHECK(v.packs()[0].images[0].shortcode.empty());
}

TEST_CASE("ImagePackEditorView: add_pending_image_to_active is a no-op "
         "with no active pack",
         "[image_pack][view]")
{
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({});

    int added_count = 0;
    v.on_pending_image_added = [&](std::uint64_t, const std::vector<std::uint8_t>&,
                                   const std::string&) { ++added_count; };
    v.add_pending_image_to_active({1, 2, 3}, "image/png");

    CHECK(added_count == 0);
}

TEST_CASE("ImagePackEditorView: add_pending_image_at targets the pack "
         "under the drop point, not the active pack",
         "[image_pack][view]")
{
    TkImagePackEditorStage st;
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org"),
                           make_pack("p2", "Stickers", "!room:example.org")});
    REQUIRE(*v.active_pack_index() == 0); // active is p1, but we'll drop on p2

    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    const std::vector<std::size_t> counts{0, 0};
    const float pack2_top = section_top_world(counts, 1);
    const tk::Point drop_pt{100.0f, pack2_top + 10.0f}; // inside pack2's header

    v.add_pending_image_at(drop_pt, {4, 5, 6}, "image/png");

    CHECK(v.packs()[0].images.empty());
    REQUIRE(v.packs()[1].images.size() == 1);
    CHECK(v.packs()[1].images[0].pending_bytes == std::vector<std::uint8_t>{4, 5, 6});
}

TEST_CASE("ImagePackEditorView: add_pending_image_at falls back to the "
         "active pack when the point isn't over any pack's section",
         "[image_pack][view]")
{
    TkImagePackEditorStage st;
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org")});
    REQUIRE(*v.active_pack_index() == 0);

    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Well above the list (over the new-pack-name row) â€” not in any section.
    const tk::Point drop_pt{100.0f, 40.0f};
    v.add_pending_image_at(drop_pt, {7, 8, 9}, "image/png");

    REQUIRE(v.packs()[0].images.size() == 1);
    CHECK(v.packs()[0].images[0].pending_bytes == std::vector<std::uint8_t>{7, 8, 9});
}

TEST_CASE("ImagePackEditorView: add_pending_image_at is a no-op with no "
         "packs at all",
         "[image_pack][view]")
{
    TkImagePackEditorStage st;
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({});

    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    v.add_pending_image_at({100.0f, 200.0f}, {1}, "image/png");
    CHECK(v.packs().empty());
}

TEST_CASE("ImagePackEditorView: set_tile_preview finds the image across "
         "packs by local_id",
         "[image_pack][view]")
{
    auto tmp_surface = TestSurface::create(4, 4);
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org"),
                           make_pack("p2", "Stickers", "!room:example.org")});

    std::uint64_t added_id = 0;
    v.on_pending_image_added = [&](std::uint64_t local_id,
                                   const std::vector<std::uint8_t>&,
                                   const std::string&) { added_id = local_id; };
    // active pack is p1 (index 0); add to p1.
    v.add_pending_image_to_active({1, 2, 3}, "image/png");
    REQUIRE(added_id != 0);

    const std::vector<std::uint8_t> pixel{255, 255, 255, 255};
    auto img = tmp_surface->factory().create_image_rgba(pixel.data(), 1, 1);
    REQUIRE(img);
    v.set_tile_preview(added_id, std::move(img));

    REQUIRE(v.packs()[0].images.size() == 1);
    CHECK(v.packs()[0].images[0].local_preview != nullptr);
}

TEST_CASE("ImagePackEditorView: clicking a tile's shortcode label begins "
         "editing, set_editing_shortcode_text + commit updates it",
         "[image_pack][view]")
{
    TkImagePackEditorStage st;
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org")});
    v.set_pack_images("p1",
                      {make_image("p1", "happy", "mxc://example.org/1")});

    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
    REQUIRE(v.shortcode_edit_rect().empty());

    const std::vector<std::size_t> counts{1};
    const tk::Point pt = tile_label_point(counts, 0, 0);
    tk::Widget* hit = v.dispatch_pointer_down(pt);
    REQUIRE(hit != nullptr);
    CHECK_FALSE(v.shortcode_edit_rect().empty());

    v.set_editing_shortcode_text("wave");
    CHECK(v.packs()[0].images[0].shortcode == "wave");

    v.commit_editing_shortcode();
    CHECK(v.shortcode_edit_rect().empty());
    CHECK(v.packs()[0].images[0].shortcode == "wave");
}

TEST_CASE("ImagePackEditorView: clicking a tile's remove chip removes it "
         "and shifts in-progress editing on later tiles",
         "[image_pack][view]")
{
    TkImagePackEditorStage st;
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org")});
    v.set_pack_images("p1", {make_image("p1", "a", "mxc://example.org/1"),
                             make_image("p1", "b", "mxc://example.org/2")});

    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    const std::vector<std::size_t> counts{2};
    // Begin editing tile 1 ("b").
    {
        const tk::Point pt = tile_label_point(counts, 0, 1);
        tk::Widget* hit = v.dispatch_pointer_down(pt);
        REQUIRE(hit != nullptr);
    }
    REQUIRE_FALSE(v.shortcode_edit_rect().empty());

    // Remove tile 0 ("a") â€” tile 1 shifts down to index 0; editing_ should
    // follow it rather than being clobbered.
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
    const tk::Point rpt = tile_remove_chip_point(counts, 0, 0);
    tk::Widget* rhit = v.dispatch_pointer_down(rpt);
    REQUIRE(rhit != nullptr);

    REQUIRE(v.packs()[0].images.size() == 1);
    CHECK(v.packs()[0].images[0].shortcode == "b");
    CHECK_FALSE(v.shortcode_edit_rect().empty());
}

TEST_CASE("ImagePackEditorView: the section list clamps wheel scroll",
         "[image_pack][view]")
{
    TkImagePackEditorStage st;
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org")});

    std::vector<tesseract::ImagePackImage> images;
    for (int i = 0; i < 60; ++i)
        images.push_back(make_image("p1", "s" + std::to_string(i),
                                    "mxc://example.org/" + std::to_string(i)));
    v.set_pack_images("p1", images);

    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    REQUIRE(v.list() != nullptr);
    CHECK(v.list()->scroll_y() == 0.0f);

    v.list()->on_wheel({0.0f, 0.0f}, 0.0f, 1'000'000.0f);
    CHECK(v.list()->scroll_y() > 0.0f);

    v.list()->on_wheel({0.0f, 0.0f}, 0.0f, -1'000'000.0f);
    CHECK(v.list()->scroll_y() == 0.0f);
}

TEST_CASE("ImagePackEditorView: build_result() returns every remaining "
         "pack, including a tile with only pending_bytes",
         "[image_pack][view]")
{
    TkImagePackEditorStage st;
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org")});
    v.set_pack_images("p1",
                      {make_image("p1", "happy", "mxc://example.org/1")});
    v.add_pending_image_to_active({9, 9, 9}, "image/png");

    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // RoomSettingsView's shared Accept click reads this directly â€” this
    // view no longer has its own Accept button.
    const ImagePackEditorResult accepted = v.build_result();
    CHECK(accepted.room_id == "!room:example.org");
    REQUIRE(accepted.packs.size() == 1);
    CHECK(accepted.packs[0].pack_id == "p1");
    REQUIRE(accepted.packs[0].images.size() == 2);
    CHECK(accepted.packs[0].images[0].shortcode == "happy");
    CHECK(accepted.packs[0].images[1].existing_url.empty());
    CHECK(accepted.packs[0].images[1].pending_bytes ==
         std::vector<std::uint8_t>{9, 9, 9});
    CHECK(accepted.removed_state_keys.empty());
}

TEST_CASE("ImagePackEditorView: set_committing(true) disables the Create "
         "button and hides the native-overlay rects",
         "[image_pack][view]")
{
    TkImagePackEditorStage st;
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org")});

    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    REQUIRE(v.create_button() != nullptr);
    CHECK(v.create_button()->enabled());
    CHECK_FALSE(v.new_pack_name_field_rect().empty());

    v.set_committing(true);
    CHECK_FALSE(v.create_button()->enabled());
    CHECK(v.new_pack_name_field_rect().empty());

    v.set_committing(false);
    CHECK(v.create_button()->enabled());
    CHECK_FALSE(v.new_pack_name_field_rect().empty());
}

TEST_CASE("ImagePackEditorView: close() closes the view", "[image_pack][view]")
{
    ImagePackEditorView v;
    v.open("!room:example.org");
    v.set_field_permissions(true);
    v.close();
    CHECK_FALSE(v.is_open());
}

TEST_CASE("ImagePackEditorView: paints without crashing across states",
         "[image_pack][view]")
{
    TkImagePackEditorStage st;
    ImagePackEditorView v;

    // Closed.
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Open, no packs yet.
    v.open("!room:example.org");
    v.set_field_permissions(true);
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    v.set_available_packs({});
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});

    // Multiple packs, one mid-edit, one being created.
    v.set_available_packs({make_pack("p1", "Emotes", "!room:example.org"),
                           make_pack("p2", "Stickers", "!room:example.org")});
    v.set_pack_images("p1",
                      {make_image("p1", "happy", "mxc://example.org/1")});
    v.add_pending_image_to_active({1, 2, 3}, "image/png");
    st.run(v, {0.0f, 0.0f, 800.0f, 600.0f});
}
