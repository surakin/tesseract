#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/StickerPicker.h"
#include "tk_test_surface.h"

#include <tesseract/image_pack.h>

#include <memory>
#include <string>
#include <vector>

using namespace tk;
using tesseract::ImagePack;
using tesseract::ImagePackImage;
using tesseract::PackSourceKind;
using tesseract::PackUsage;
using tesseract::PackUsageFilter;
using tesseract::views::StickerPicker;

namespace
{

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(360, 420);
    LayoutCtx layout_ctx()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    PaintCtx paint_ctx()
    {
        return PaintCtx{surface->canvas(), surface->factory(), Theme::light()};
    }
    void run(Widget& root, Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

} // namespace

TEST_CASE("StickerPicker without a client renders zero tabs beyond Favorites",
          "[tk][stickerpicker]")
{
    Stage st;
    StickerPicker p;
    st.run(p, {0, 0, 360, 420});
    REQUIRE(p.packs().empty());
    REQUIRE(p.active_tab() == 0); // Favorites
    REQUIRE(p.current().empty());
}

TEST_CASE("PackUsage operators behave like a bitset", "[image_pack]")
{
    constexpr auto both = PackUsage::Sticker | PackUsage::Emoticon;
    REQUIRE(any(both & PackUsage::Sticker));
    REQUIRE(any(both & PackUsage::Emoticon));
    REQUIRE(both == PackUsage::Any);
    constexpr auto none = PackUsage::None;
    REQUIRE(!any(none & PackUsage::Sticker));
}

TEST_CASE("PackUsageFilter maps to the FFI tags expected by Rust",
          "[image_pack]")
{
    REQUIRE(std::string(tesseract::pack_usage_filter_to_str(
                PackUsageFilter::Sticker)) == "sticker");
    REQUIRE(std::string(tesseract::pack_usage_filter_to_str(
                PackUsageFilter::Emoticon)) == "emoticon");
    REQUIRE(std::string(tesseract::pack_usage_filter_to_str(
                PackUsageFilter::Any)) == "any");
}
