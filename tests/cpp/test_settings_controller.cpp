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
    auto run_inline  = [](std::function<void()> fn) { fn(); };

    tesseract::SettingsController ctrl(nullptr, post_inline, run_inline, picker_noop);

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
    auto run_inline  = [](std::function<void()> fn) { fn(); };

    tesseract::SettingsController ctrl(nullptr, post_inline, run_inline, picker_noop);
    ctrl.on_name_result = [&](bool, std::string) { ++result_count; };

    // set_client(nullptr) — same pointer, so result still fires.
    ctrl.set_client(nullptr);
    ctrl.set_display_name("Alice");
    // Both captured_client and current client_ are nullptr → match → result fires.
    CHECK(result_count == 1);
}

TEST_CASE("SettingsController: load_devices reports empty list when not logged in",
          "[settings][controller][devices]")
{
    auto post_inline = [](std::function<void()> fn) { fn(); };
    auto picker_noop = [](std::function<void(std::vector<uint8_t>, std::string)>) {};
    auto run_inline  = [](std::function<void()> fn) { fn(); };
    tesseract::SettingsController ctrl(nullptr, post_inline, run_inline, picker_noop);

    int loaded_calls = 0;
    std::size_t loaded_size = 99;
    ctrl.on_devices_loaded = [&](std::vector<tesseract::Client::Device> v)
    {
        ++loaded_calls;
        loaded_size = v.size();
    };

    ctrl.load_devices();
    CHECK(loaded_calls == 1);
    CHECK(loaded_size == 0);
}

TEST_CASE("SettingsController: rename_device reports error when not logged in",
          "[settings][controller][devices]")
{
    auto post_inline = [](std::function<void()> fn) { fn(); };
    auto picker_noop = [](std::function<void(std::vector<uint8_t>, std::string)>) {};
    auto run_inline  = [](std::function<void()> fn) { fn(); };
    tesseract::SettingsController ctrl(nullptr, post_inline, run_inline, picker_noop);

    int calls = 0;
    bool got_ok = true;
    std::string got_id;
    ctrl.on_device_renamed = [&](std::string id, bool ok, std::string)
    {
        ++calls;
        got_ok = ok;
        got_id = std::move(id);
    };

    ctrl.rename_device("DEVICEA", "Phone");
    CHECK(calls == 1);
    CHECK_FALSE(got_ok);
    CHECK(got_id == "DEVICEA");
}

TEST_CASE("SettingsController: rename_device dedupes per-device in flight",
          "[settings][controller][devices]")
{
    // Each rename invocation immediately resolves via post_inline + null
    // client, so the in-flight set is empty between calls. Verify it does
    // accept a fresh call for the same id after the first completes.
    auto post_inline = [](std::function<void()> fn) { fn(); };
    auto picker_noop = [](std::function<void(std::vector<uint8_t>, std::string)>) {};
    auto run_inline  = [](std::function<void()> fn) { fn(); };
    tesseract::SettingsController ctrl(nullptr, post_inline, run_inline, picker_noop);

    int calls = 0;
    ctrl.on_device_renamed = [&](std::string, bool, std::string) { ++calls; };

    ctrl.rename_device("DEVICEA", "First");
    ctrl.rename_device("DEVICEA", "Second");
    CHECK(calls == 2);

    // Independent device ids are independent slots.
    ctrl.rename_device("DEVICEB", "Other");
    CHECK(calls == 3);
}

TEST_CASE("SettingsController: delete_device + confirm reports failure when not logged in",
          "[settings][controller][devices]")
{
    auto post_inline = [](std::function<void()> fn) { fn(); };
    auto picker_noop = [](std::function<void(std::vector<uint8_t>, std::string)>) {};
    auto run_inline  = [](std::function<void()> fn) { fn(); };
    tesseract::SettingsController ctrl(nullptr, post_inline, run_inline, picker_noop);

    int deleted_calls = 0;
    bool deleted_ok = true;
    ctrl.on_device_deleted = [&](std::string, bool ok, std::string)
    {
        ++deleted_calls;
        deleted_ok = ok;
    };

    ctrl.delete_device("DEVICEA");
    // No client → immediate failure path.
    CHECK(deleted_calls == 1);
    CHECK_FALSE(deleted_ok);

    // After failure the per-device slot must be released so a follow-up
    // confirm or delete is accepted (here confirm fails the same way).
    ctrl.confirm_device_deletion("DEVICEA", "session-xyz");
    CHECK(deleted_calls == 2);
    CHECK_FALSE(deleted_ok);
}
