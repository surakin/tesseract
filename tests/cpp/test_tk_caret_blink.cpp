#include <catch2/catch_test_macros.hpp>

#include "tk/caret_blink.h"

namespace
{

// Screenshot mode flips the process-wide flag; put it back for later tests.
struct RestoreBlink
{
    ~RestoreBlink()
    {
        tk::set_caret_blink_enabled(true);
    }
};

} // namespace

TEST_CASE("the caret follows the backend's blink phase by default", "[tk][caret]")
{
    RestoreBlink restore;
    tk::set_caret_blink_enabled(true);
    CHECK(tk::caret_drawn(true, true));
    CHECK_FALSE(tk::caret_drawn(false, true));
    // The backend's phase already includes its own focus; the widget's focus
    // is not consulted while blinking (unchanged behaviour).
    CHECK(tk::caret_drawn(true, false));
}

TEST_CASE("with the blink off the caret is solid while the widget has focus", "[tk][caret]")
{
    RestoreBlink restore;
    tk::set_caret_blink_enabled(false);
    // The backend's phase no longer matters.
    CHECK(tk::caret_drawn(false, true));
    CHECK(tk::caret_drawn(true, true));
    CHECK_FALSE(tk::caret_drawn(true, false));
    CHECK_FALSE(tk::caret_drawn(false, false));
}
