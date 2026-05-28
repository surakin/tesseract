#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

struct TestShell : ShellBase
{
    void post_to_ui_(std::function<void()> fn) override { fn(); }
    void post_to_ui_after_(int, std::function<void()> fn) override { fn(); }
    void request_relayout_() override {}
    void request_repaint_() override {}
    void on_rooms_updated_() override {}
    void on_media_bytes_ready_(const std::string&, MediaKind,
                               std::vector<uint8_t>) override {}
    void on_tab_state_changed_ui_() override {}
    DecodedImage decode_image_(const std::vector<uint8_t>&, int, int) override
    {
        return {};
    }
    std::int64_t monotonic_ms_() override { return 1000; }
    void start_anim_tick_() override {}
    void repaint_pickers_() override {}
    void navigate_to_room_(const std::string&) override {}

    void apply_thread_messages_(const std::string& root,
                                std::vector<tesseract::views::MessageRowData> rows,
                                bool switch_) override
    {
        last_reset_root  = root;
        last_reset_rows  = std::move(rows);
        last_reset_switch = switch_;
    }
    void apply_thread_message_insert_(
        const std::string& root, std::size_t idx,
        tesseract::views::MessageRowData row) override
    {
        ++insert_calls;
        last_insert_root = root;
        last_insert_idx = idx;
        last_insert_row = std::move(row);
    }
    void apply_thread_message_remove_(const std::string& root,
                                      std::size_t idx) override
    {
        ++remove_calls;
        last_remove_root = root;
        last_remove_idx  = idx;
    }

    using ShellBase::current_room_id_;
    using ShellBase::current_thread_root_;
    using ShellBase::handle_thread_reset_ui_;
    using ShellBase::handle_thread_inserted_ui_;
    using ShellBase::handle_thread_removed_ui_;
    using ShellBase::on_threads_button_clicked;
    using ShellBase::on_thread_open_requested;
    using ShellBase::on_thread_close_requested;
    using ShellBase::thread_panel_;

    std::string last_reset_root;
    std::vector<tesseract::views::MessageRowData> last_reset_rows;
    bool last_reset_switch = false;
    int insert_calls = 0;
    std::string last_insert_root;
    std::size_t last_insert_idx = 0;
    tesseract::views::MessageRowData last_insert_row;
    int remove_calls = 0;
    std::string last_remove_root;
    std::size_t last_remove_idx = 0;
};

std::unique_ptr<tesseract::Event> text_event(const std::string& id)
{
    auto ev = std::make_unique<tesseract::Event>();
    ev->type = tesseract::EventType::Text;
    ev->event_id = id;
    return ev;
}

} // namespace

TEST_CASE("handle_thread_reset_ui_ dispatches when room+root match",
          "[shell][thread_panel]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    s.current_thread_root_ = "$root";
    tesseract::EventList snap;
    snap.push_back(text_event("$reply1"));
    snap.push_back(text_event("$reply2"));
    s.handle_thread_reset_ui_("!r:x", "$root", std::move(snap));

    CHECK(s.last_reset_root == "$root");
    CHECK(s.last_reset_rows.size() == 2);
    CHECK(s.last_reset_switch);
}

TEST_CASE("handle_thread_reset_ui_ drops mismatched room",
          "[shell][thread_panel]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    s.current_thread_root_ = "$root";
    s.handle_thread_reset_ui_("!other:x", "$root", {});
    CHECK(s.last_reset_root.empty());
}

TEST_CASE("handle_thread_reset_ui_ drops mismatched root",
          "[shell][thread_panel]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    s.current_thread_root_ = "$root";
    s.handle_thread_reset_ui_("!r:x", "$other_root", {});
    CHECK(s.last_reset_root.empty());
}

TEST_CASE("handle_thread_inserted_ui_ skips Unhandled events",
          "[shell][thread_panel]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    s.current_thread_root_ = "$root";
    auto ev = std::make_unique<tesseract::Event>();
    ev->type = tesseract::EventType::Unhandled;
    s.handle_thread_inserted_ui_("!r:x", "$root", 0, std::move(ev));
    CHECK(s.insert_calls == 0);
}

TEST_CASE("handle_thread_inserted_ui_ dispatches when room+root match",
          "[shell][thread_panel]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    s.current_thread_root_ = "$root";
    s.handle_thread_inserted_ui_("!r:x", "$root", 3, text_event("$r"));
    CHECK(s.insert_calls == 1);
    CHECK(s.last_insert_root == "$root");
    CHECK(s.last_insert_idx == 3);
}

TEST_CASE("handle_thread_removed_ui_ dispatches when room+root match",
          "[shell][thread_panel]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    s.current_thread_root_ = "$root";
    s.handle_thread_removed_ui_("!r:x", "$root", 2);
    CHECK(s.remove_calls == 1);
    CHECK(s.last_remove_idx == 2);
}

TEST_CASE("on_threads_button_clicked is callable with null client",
          "[shell][thread_panel]")
{
    TestShell s;
    s.current_room_id_ = "!r:x";
    // No client_ wired → applier short-circuits the SDK calls but still
    // updates local thread_panel_ state. Smoke-validates the public API.
    s.on_threads_button_clicked();
    CHECK(s.thread_panel_ == tesseract::ShellBase::ThreadPanel::List);
    s.on_threads_button_clicked();
    CHECK(s.thread_panel_ == tesseract::ShellBase::ThreadPanel::Closed);
}
