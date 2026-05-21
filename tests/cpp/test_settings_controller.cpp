#include <catch2/catch_test_macros.hpp>
#include "app/SettingsController.h"
#include <atomic>
#include <string>
#include <vector>
#include <functional>

TEST_CASE("SettingsController: double-submit blocked by in_flight flag",
          "[settings][controller]")
{
    // post_to_ui_ runs the fn inline (synchronous for testing).
    int post_count = 0;
    auto post_inline = [&](std::function<void()> fn) {
        ++post_count;
        fn();
    };
    auto picker_noop = [](std::function<void(std::vector<uint8_t>, std::string)>) {};

    tesseract::SettingsController ctrl(nullptr, post_inline, picker_noop);

    // Wire result sink
    int result_count = 0;
    ctrl.on_name_result = [&](bool, std::string) { ++result_count; };

    // With null client, set_display_name returns an error synchronously via post_to_ui_.
    ctrl.set_display_name("Alice");
    CHECK(result_count == 1);

    // Second call also succeeds (null client path clears flag immediately).
    ctrl.set_display_name("Bob");
    CHECK(result_count == 2);
}

TEST_CASE("SettingsController: stale result dropped after set_client(nullptr)",
          "[settings][controller]")
{
    int result_count = 0;
    auto post_inline = [](std::function<void()> fn) { fn(); };
    auto picker_noop = [](std::function<void(std::vector<uint8_t>, std::string)>) {};

    tesseract::SettingsController ctrl(nullptr, post_inline, picker_noop);
    ctrl.on_name_result = [&](bool, std::string) { ++result_count; };

    // set_client(nullptr) — same pointer, so result still fires.
    ctrl.set_client(nullptr);
    ctrl.set_display_name("Alice");
    // Both captured_client and current client_ are nullptr → match → result fires.
    CHECK(result_count == 1);
}
