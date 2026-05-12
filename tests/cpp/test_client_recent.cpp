#include <catch2/catch_test_macros.hpp>
#include "tesseract/client.h"

// `recent_emoji_top` reads `io.element.recent_emoji` from the SDK's local
// sync cache. Before login there is no client and no cache, so the call
// must short-circuit to an empty vector instead of crashing or blocking.
TEST_CASE("recent_emoji_top is empty before login", "[client][recent_emoji]") {
    tesseract::Client client;
    auto top = client.recent_emoji_top(10);
    CHECK(top.empty());
}

// Bump before login must be a no-op (must not crash). Mirrors `send_*`
// methods that all return early without a session.
TEST_CASE("recent_emoji_bump is harmless before login", "[client][recent_emoji]") {
    tesseract::Client client;
    client.recent_emoji_bump("\xF0\x9F\x98\x80");  // 😀
    // No assert beyond surviving the call: the runtime spawn() must not
    // try to use a null client. The follow-up top() should still be empty.
    CHECK(client.recent_emoji_top(1).empty());
}
