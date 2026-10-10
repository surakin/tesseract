#include <catch2/catch_test_macros.hpp>

#include "views/MessageListView.h"
#include "tesseract/types.h"

using tesseract::views::MessageRowData;
using tesseract::views::make_row_data;

TEST_CASE("GalleryEvent produces Kind::Gallery row with per-item fields",
          "[message_list][gallery]")
{
    tesseract::GalleryEvent ev;
    ev.event_id    = "$gal:server";
    ev.sender      = "@alice:server";
    ev.sender_name = "Alice";
    ev.body        = "vacation pics";
    ev.timestamp   = 1700000042000ULL;

    tesseract::GalleryItem img;
    img.kind   = tesseract::GalleryItem::Kind::Image;
    img.body   = "beach.jpg";
    img.source = tesseract::MediaSource::plain("mxc://example.org/img1");
    img.width  = 800;
    img.height = 600;
    img.blurhash = "LEHV6nWB2yk8";
    img.animated = true;
    ev.items.push_back(img);

    tesseract::GalleryItem file;
    file.kind      = tesseract::GalleryItem::Kind::File;
    file.body      = "report.pdf";
    file.source    = tesseract::MediaSource::plain("mxc://example.org/file1");
    file.filename  = "report.pdf";
    file.file_size = 4096;
    ev.items.push_back(file);

    MessageRowData row = make_row_data(ev, /*my_user_id=*/"@bot:server");

    REQUIRE(row.kind == MessageRowData::Kind::Gallery);
    CHECK(row.event_id == "$gal:server");
    CHECK(row.sender_name == "Alice");
    CHECK(row.body == "vacation pics");
    CHECK(row.timestamp_ms == 1700000042000ULL);
    CHECK_FALSE(row.is_own);

    REQUIRE(row.gallery_items.size() == 2);

    const auto& img_row = row.gallery_items[0];
    CHECK(img_row.kind == MessageRowData::GalleryItemRow::Kind::Image);
    CHECK(img_row.body == "beach.jpg");
    REQUIRE(img_row.source);
    CHECK(img_row.source->mxc_url() == "mxc://example.org/img1");
    CHECK(img_row.media_w == 800);
    CHECK(img_row.media_h == 600);
    CHECK(img_row.blurhash == "LEHV6nWB2yk8");
    CHECK(img_row.animated);

    const auto& file_row = row.gallery_items[1];
    CHECK(file_row.kind == MessageRowData::GalleryItemRow::Kind::File);
    CHECK(file_row.filename == "report.pdf");
    CHECK(file_row.file_size == 4096);
}

TEST_CASE("Non-gallery events leave gallery_items empty", "[message_list][gallery]")
{
    tesseract::ImageEvent ev;
    ev.event_id = "$img:server";

    MessageRowData row = make_row_data(ev, /*my_user_id=*/"@bot:server");

    CHECK(row.kind == MessageRowData::Kind::Image);
    CHECK(row.gallery_items.empty());
}

TEST_CASE("Gallery rows cap incoming items but keep the uncapped total",
          "[message_list][gallery]")
{
    tesseract::GalleryEvent ev;
    ev.event_id = "$big:server";
    ev.sender   = "@mallory:server";
    const std::size_t n = MessageRowData::kMaxGalleryViewerItems * 5;
    for (std::size_t i = 0; i < n; ++i)
    {
        tesseract::GalleryItem it;
        it.kind   = tesseract::GalleryItem::Kind::Image;
        it.body   = "i" + std::to_string(i);
        it.source = tesseract::MediaSource::plain("mxc://example.org/" + std::to_string(i));
        ev.items.push_back(std::move(it));
    }

    MessageRowData row = make_row_data(ev, "@bot:server");

    REQUIRE(row.kind == MessageRowData::Kind::Gallery);
    CHECK(row.gallery_items.size() == MessageRowData::kMaxGalleryViewerItems);
    CHECK(row.gallery_total == n);
    CHECK(row.gallery_painted_count() == MessageRowData::kMaxGalleryItems);
    // The first items are the ones kept, in order.
    CHECK(row.gallery_items.front().body == "i0");
    CHECK(row.gallery_items.back().body ==
          "i" + std::to_string(MessageRowData::kMaxGalleryViewerItems - 1));
}

TEST_CASE("Gallery at or under the cap is not truncated",
          "[message_list][gallery]")
{
    tesseract::GalleryEvent ev;
    ev.event_id = "$ok:server";
    for (std::size_t i = 0; i < MessageRowData::kMaxGalleryItems; ++i)
    {
        tesseract::GalleryItem it;
        it.kind   = tesseract::GalleryItem::Kind::File;
        it.source = tesseract::MediaSource::plain("mxc://example.org/f");
        ev.items.push_back(std::move(it));
    }
    MessageRowData row = make_row_data(ev, "@bot:server");
    CHECK(row.gallery_items.size() == MessageRowData::kMaxGalleryItems);
    CHECK(row.gallery_total == MessageRowData::kMaxGalleryItems);
}
