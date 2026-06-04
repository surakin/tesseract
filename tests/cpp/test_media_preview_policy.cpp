#include "app/media_preview_policy.h"

#include <catch2/catch_test_macros.hpp>

using tesseract::app::media_allowed;
using Mode = tesseract::Settings::MediaPreviews;

// MSC4278 suppression with the own-media exemption. Truth table over
// (mode × room publicity × is_own × revealed).

TEST_CASE("On mode always shows media", "[media_preview_policy]")
{
    for (bool is_own : {false, true})
    {
        for (const char* jr : {"public", "invite", ""})
        {
            CHECK(media_allowed(Mode::On, jr, is_own, /*revealed=*/false));
        }
    }
}

TEST_CASE("Off mode always suppresses, even own media", "[media_preview_policy]")
{
    CHECK_FALSE(media_allowed(Mode::Off, "public", /*is_own=*/false, false));
    CHECK_FALSE(media_allowed(Mode::Off, "public", /*is_own=*/true, false));
    CHECK_FALSE(media_allowed(Mode::Off, "invite", /*is_own=*/true, false));
    CHECK_FALSE(media_allowed(Mode::Off, "", /*is_own=*/true, false));
}

TEST_CASE("Private mode shows media in non-public rooms", "[media_preview_policy]")
{
    // Everyone's media shows in a private (non-public) room.
    CHECK(media_allowed(Mode::Private, "invite", /*is_own=*/false, false));
    CHECK(media_allowed(Mode::Private, "restricted", /*is_own=*/false, false));
    CHECK(media_allowed(Mode::Private, "private", /*is_own=*/true, false));
}

TEST_CASE("Private mode suppresses other people's media in public rooms",
          "[media_preview_policy]")
{
    CHECK_FALSE(media_allowed(Mode::Private, "public", /*is_own=*/false, false));
    // Empty / unknown join rule is treated as public.
    CHECK_FALSE(media_allowed(Mode::Private, "", /*is_own=*/false, false));
}

TEST_CASE("Private mode exempts the user's own media in public rooms",
          "[media_preview_policy]")
{
    CHECK(media_allowed(Mode::Private, "public", /*is_own=*/true, false));
    // Even when the join rule is unknown (treated as public).
    CHECK(media_allowed(Mode::Private, "", /*is_own=*/true, false));
}

TEST_CASE("Revealed media is always shown regardless of mode",
          "[media_preview_policy]")
{
    CHECK(media_allowed(Mode::Off, "public", /*is_own=*/false, /*revealed=*/true));
    CHECK(media_allowed(Mode::Private, "public", /*is_own=*/false,
                        /*revealed=*/true));
}
