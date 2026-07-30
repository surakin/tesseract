#include <catch2/catch_test_macros.hpp>

#include "linux_notification_reply.h"

using tesseract::linux_notify::supports_inline_reply;

TEST_CASE("supports_inline_reply detects the KDE capability string",
          "[linux_notify]")
{
    CHECK(supports_inline_reply(
        {"body", "actions", "inline-reply", "persistence"}));
}

TEST_CASE("supports_inline_reply is false when the capability is absent",
          "[linux_notify]")
{
    CHECK_FALSE(supports_inline_reply({"body", "actions", "persistence"}));
}

TEST_CASE("supports_inline_reply is false for an empty capability list",
          "[linux_notify]")
{
    CHECK_FALSE(supports_inline_reply({}));
}

TEST_CASE("supports_inline_reply is case-sensitive (KDE's string is always "
          "lowercase)",
          "[linux_notify]")
{
    CHECK_FALSE(supports_inline_reply({"Inline-Reply", "INLINE-REPLY"}));
}
