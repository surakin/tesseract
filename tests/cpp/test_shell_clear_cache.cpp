#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "shell_test_double.h"

#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct ShellClearCacheTestShell : tesseract::test::TestShellBase
{

    // Drop deferred work so save_settings_debounced_() never touches the disk.
    void post_to_ui_after_(int, std::function<void()>) override
    {
    }
    void apply_thread_messages_(const std::string&, std::vector<tesseract::views::MessageRowData>,
                                bool) override
    {
    }
    void apply_thread_message_insert_(const std::string&, std::size_t,
                                      tesseract::views::MessageRowData) override
    {
    }
    void apply_thread_message_remove_(const std::string&, std::size_t) override
    {
    }

    using ShellBase::close_all_popouts_;
    using ShellBase::my_user_id_;
    using ShellBase::pending_restore_popouts_;
};

} // namespace

TEST_CASE("close_all_popouts_ clears pending restores and forgets this "
          "account's saved pop-outs only",
          "[shell][clear_cache]")
{
    ShellClearCacheTestShell s;
    s.my_user_id_ = "@alice:x";

    auto& pops = tesseract::Settings::instance().popout_windows;
    pops.clear();
    pops.push_back({.room_id = "!a:x", .user_id = "@alice:x", .geometry = {}});
    pops.push_back({.room_id = "!b:x", .user_id = "@alice:x", .geometry = {}});
    pops.push_back({.room_id = "!c:x", .user_id = "@bob:x", .geometry = {}});

    s.pending_restore_popouts_ = {"!a:x", "!z:x"};

    s.close_all_popouts_();

    CHECK(s.pending_restore_popouts_.empty());
    REQUIRE(pops.size() == 1);
    CHECK(pops[0].user_id == "@bob:x");
    CHECK(pops[0].room_id == "!c:x");

    pops.clear();
}

TEST_CASE("close_all_popouts_ is a no-op when nothing belongs to this account",
          "[shell][clear_cache]")
{
    ShellClearCacheTestShell s;
    s.my_user_id_ = "@alice:x";

    auto& pops = tesseract::Settings::instance().popout_windows;
    pops.clear();
    pops.push_back({.room_id = "!c:x", .user_id = "@bob:x", .geometry = {}});

    s.close_all_popouts_();

    REQUIRE(pops.size() == 1);
    CHECK(pops[0].user_id == "@bob:x");

    pops.clear();
}
