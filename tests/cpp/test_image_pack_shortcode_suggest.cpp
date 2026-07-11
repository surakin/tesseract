#include <catch2/catch_test_macros.hpp>

#include "views/ImagePackTileGridBase.h"

using tesseract::views::dedupe_pack_shortcode;
using tesseract::views::StagedPackImage;
using tesseract::views::suggest_pack_shortcode_from_filename;

TEST_CASE("suggest_pack_shortcode_from_filename: strips extension and lowercases",
         "[image_pack][shortcode_suggest]")
{
    CHECK(suggest_pack_shortcode_from_filename("HappyFace.png") == "happyface");
}

TEST_CASE("suggest_pack_shortcode_from_filename: spaces/dashes/underscores fold "
         "to a single underscore",
         "[image_pack][shortcode_suggest]")
{
    CHECK(suggest_pack_shortcode_from_filename("happy face-2.jpg") == "happy_face_2");
}

TEST_CASE("suggest_pack_shortcode_from_filename: drops other punctuation",
         "[image_pack][shortcode_suggest]")
{
    CHECK(suggest_pack_shortcode_from_filename("wow!!.gif") == "wow");
}

TEST_CASE("suggest_pack_shortcode_from_filename: falls back to sticker when "
         "nothing usable remains",
         "[image_pack][shortcode_suggest]")
{
    CHECK(suggest_pack_shortcode_from_filename("!!!.png") == "sticker");
    CHECK(suggest_pack_shortcode_from_filename("") == "sticker");
}

TEST_CASE("suggest_pack_shortcode_from_filename: a leading dot (dotfile, no "
         "real extension) is kept, not treated as an extension separator",
         "[image_pack][shortcode_suggest]")
{
    CHECK(suggest_pack_shortcode_from_filename(".gitignore") == "gitignore");
}

TEST_CASE("dedupe_pack_shortcode: returns base unchanged when free",
         "[image_pack][shortcode_suggest]")
{
    std::vector<StagedPackImage> siblings;
    CHECK(dedupe_pack_shortcode(siblings, "happy") == "happy");
}

TEST_CASE("dedupe_pack_shortcode: appends a numeric suffix on collision",
         "[image_pack][shortcode_suggest]")
{
    std::vector<StagedPackImage> siblings(1);
    siblings[0].shortcode = "happy";
    CHECK(dedupe_pack_shortcode(siblings, "happy") == "happy_2");
}

TEST_CASE("dedupe_pack_shortcode: skips past multiple existing collisions",
         "[image_pack][shortcode_suggest]")
{
    std::vector<StagedPackImage> siblings(3);
    siblings[0].shortcode = "happy";
    siblings[1].shortcode = "happy_2";
    siblings[2].shortcode = "happy_3";
    CHECK(dedupe_pack_shortcode(siblings, "happy") == "happy_4");
}
