#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "shell_test_double.h"

#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct ShellWindowTitleTestShell : tesseract::test::TestShellBase
{

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

    void apply_window_title_ui_(const std::string& title) override
    {
        last_title = title;
    }

    void set_room(const std::string& id, const std::string& name)
    {
        tesseract::RoomInfo r;
        r.id = id;
        r.name = name;
        rooms_.push_back(std::move(r));
        mark_room_index_dirty_();
        current_room_id_ = id;
    }

    using ShellBase::current_room_id_;
    using ShellBase::rooms_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::app_settings_open_;
    using ShellBase::compose_window_title_;
    using ShellBase::refresh_window_title_;
    using ShellBase::set_app_settings_open_;

    std::string last_title;
};

} // namespace

TEST_CASE("window title is bare product name with no active room", "[shell][window_title]")
{
    ShellWindowTitleTestShell s;
    CHECK(s.compose_window_title_() == "Tesseract");
}

TEST_CASE("window title reflects the active room name", "[shell][window_title]")
{
    ShellWindowTitleTestShell s;
    s.set_room("!r:x", "General");
    s.refresh_window_title_();
    CHECK(s.last_title == "Tesseract - General");
}

TEST_CASE("opening app settings drops the room name, closing restores it", "[shell][window_title]")
{
    ShellWindowTitleTestShell s;
    s.set_room("!r:x", "General");
    s.refresh_window_title_();
    REQUIRE(s.last_title == "Tesseract - General");

    s.set_app_settings_open_(true);
    CHECK(s.last_title == "Tesseract");

    s.set_app_settings_open_(false);
    CHECK(s.last_title == "Tesseract - General");
}

TEST_CASE("title stays bare while settings is open even if rooms refresh", "[shell][window_title]")
{
    ShellWindowTitleTestShell s;
    s.set_room("!r:x", "General");
    s.set_app_settings_open_(true);
    REQUIRE(s.last_title == "Tesseract");

    // A sync tick would call refresh_window_title_() again.
    s.refresh_window_title_();
    CHECK(s.last_title == "Tesseract");
}
