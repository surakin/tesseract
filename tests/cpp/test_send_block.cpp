#include "views/send_block.h"

#include <catch2/catch_test_macros.hpp>

namespace sb = tesseract::views::send_block;

TEST_CASE("send_block recognises the encryption-check codes", "[send_block]")
{
    CHECK(sb::is_block_code("identity_violation"));
    CHECK(sb::is_block_code("insecure_devices"));
    CHECK(sb::is_block_code("own_verification_required"));
    CHECK_FALSE(sb::is_block_code(""));
    CHECK_FALSE(sb::is_block_code("Server returned 500"));

    // Only an identity change is cleared by removing verification: for an
    // unsigned device the user stays verified by our own signature.
    CHECK(sb::can_remove_verification("identity_violation"));
    CHECK_FALSE(sb::can_remove_verification("insecure_devices"));
    CHECK_FALSE(sb::can_remove_verification("own_verification_required"));
}

TEST_CASE("send_block decides what Retry does from the setting and the running state",
          "[send_block]")
{
    using A = sb::RetryAction;
    // Check on in this run: explain, whatever is saved.
    CHECK(sb::retry_action("insecure_devices", true, true) == A::Explain);
    CHECK(sb::retry_action("identity_violation", true, true) == A::Explain);
    // Turned off and already applied (after a restart): just retry.
    CHECK(sb::retry_action("insecure_devices", false, false) == A::RetryDirectly);
    // Turned off but this run still blocks: say a restart is needed.
    CHECK(sb::retry_action("insecure_devices", false, true) == A::NeedsRestart);
    // Turned back on before a restart: the running state is still off.
    CHECK(sb::retry_action("insecure_devices", true, false) == A::RetryDirectly);
    // The own-device block does not depend on the setting.
    CHECK(sb::retry_action("own_verification_required", false, true) == A::Explain);
}

TEST_CASE("send_block names the affected users", "[send_block]")
{
    const auto name = [](const std::string& id) -> std::string
    {
        return id == "@a:x" ? "Alice" : id == "@b:x" ? "Bob" : "";
    };

    CHECK(sb::reason("insecure_devices", {"@a:x"}, name) ==
          "Alice has a device that isn't confirmed by its owner");
    CHECK(sb::reason("identity_violation", {"@a:x"}, name) == "Alice's digital identity was reset");
    // Several users read as a list, not a repeated sentence.
    CHECK(sb::reason("insecure_devices", {"@a:x", "@b:x"}, name) ==
          "Alice and Bob have devices that aren't confirmed by their owners");
    CHECK(sb::reason("insecure_devices", {"@a:x", "@b:x", "@c:x", "@d:x"}, name) ==
          "Alice, Bob and 2 others have devices that aren't confirmed by their owners");
}

TEST_CASE("send_block falls back to the user id and handles missing users", "[send_block]")
{
    CHECK(sb::reason("insecure_devices", {"@c:x"}, {}) ==
          "@c:x has a device that isn't confirmed by its owner");
    // A name resolver that knows nothing behaves the same.
    CHECK(sb::reason("insecure_devices", {"@c:x"},
                     [](const std::string&)
                     {
                         return std::string();
                     }) == "@c:x has a device that isn't confirmed by its owner");
    CHECK(sb::reason("insecure_devices", {}, {}) ==
          "someone in this room has a device that isn't confirmed by its owner");
}

TEST_CASE("send_block gives nothing for other errors", "[send_block]")
{
    CHECK(sb::reason("", {}, {}).empty());
    CHECK(sb::reason("Server returned 500", {}, {}).empty());
    CHECK(sb::explanation("Server returned 500", {}, {}).empty());
    CHECK(sb::reason("own_verification_required", {}, {}) ==
          "This device must be confirmed before it can send messages");
}

TEST_CASE("send_block explains what removing verification does", "[send_block]")
{
    const std::string e = sb::explanation("identity_violation", {"@c:x"}, {});
    CHECK(e.find("@c:x's digital identity was reset") == 0);
    CHECK(e.find("verify them again later") != std::string::npos);
    // An unsigned device can only be fixed by its owner: say so, don't offer removal.
    const std::string d = sb::explanation("insecure_devices", {"@c:x"}, {});
    CHECK(d.find("@c:x has a device") == 0);
    CHECK(d.find("confirm or sign out that device") != std::string::npos);
    CHECK(d.find("verify them again later") == std::string::npos);
    // Nothing to remove for the own-device case.
    CHECK(sb::explanation("own_verification_required", {}, {}).find("verify them") ==
          std::string::npos);
}
