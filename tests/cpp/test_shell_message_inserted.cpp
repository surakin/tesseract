#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include "views/RoomView.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace
{

// A minimal ShellBase test double that points room_view_ at a real RoomView and
// pins current_room_id_, so the shared handle_message_inserted_ui_ runs its full
// guard logic against an inspectable message list. prep_row_media_ is stubbed to
// a no-op so the test isolates the in-thread guard from media-prefetch side
// effects (disk caches, avatar fetches).
struct WithAccountManager
{
    tesseract::AccountManager am_;
};

struct InsertShell : WithAccountManager, ShellBase
{
    InsertShell() : ShellBase(am_) {}

    // ── Pure-virtual surface (no-op for this test) ───────────────────────────
    void post_to_ui_(std::function<void()> fn) override
    {
        queue.push_back(std::move(fn));
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        queue.push_back(std::move(fn));
    }
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
    void pick_image_file_(
        std::function<void(std::vector<uint8_t>, std::string)>) override {}
    void show_encryption_setup_overlay_(
        tesseract::views::EncryptionSetupOverlay::Mode) override {}
    void raise_and_activate_() override {}
    bool is_ctrl_held_() const override { return false; }
    void switch_active_account_(const std::string&) override {}
    void request_relogin_(const std::string&) override {}
    void bind_settings_controller_() override {}
    void spawn_main_window_(std::shared_ptr<tesseract::AccountSession>) override
    {
    }
    void apply_thread_messages_(
        const std::string&, std::vector<tesseract::views::MessageRowData>,
        bool) override {}
    void apply_thread_message_insert_(const std::string&, std::size_t,
                                      tesseract::views::MessageRowData) override
    {
    }
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}

    // Isolate the guard under test from media-prefetch machinery, and record
    // which events the handler chose to process. The base handler only calls
    // prep_row_media_ inside the `!in_thread` branch, so this counter observes
    // the in-thread guard *before* MessageListView's own defence-in-depth drop —
    // i.e. it fails if the guard is removed, which the old Win/macOS overrides
    // did.
    void prep_row_media_(const tesseract::Event& ev) override
    {
        prepped_event_ids.push_back(ev.event_id);
    }

    using ShellBase::current_room_id_;
    using ShellBase::handle_message_inserted_ui_;
    using ShellBase::room_view_;

    std::vector<std::function<void()>> queue;
    std::vector<std::string> prepped_event_ids;
};

std::unique_ptr<tesseract::Event> make_event(const std::string& event_id,
                                             const std::string& thread_root)
{
    auto ev = std::make_unique<tesseract::Event>();
    ev->event_id = event_id;
    ev->sender = "@alice:example.org";
    ev->body = "hi";
    ev->type = tesseract::EventType::Text;
    ev->thread_root_id = thread_root;
    return ev;
}

} // namespace

TEST_CASE("handle_message_inserted_ui_ excludes in-thread replies from the "
          "main timeline",
          "[shell][message_inserted]")
{
    InsertShell s;
    tesseract::views::RoomView view;
    tesseract::RoomInfo info;
    info.id = "!room:example.org";
    view.set_room(info);

    s.room_view_ = &view;
    s.current_room_id_ = info.id;

    REQUIRE(view.message_list()->messages().empty());

    // An in-thread reply (non-empty thread_root_id) must NOT be processed for
    // the main timeline — the override-free shared path applies this guard for
    // all shells. Before this fix, the Windows/macOS overrides dropped the guard
    // and these rows leaked into the main list (and to pop-out windows). The
    // base handler short-circuits *before* prep_row_media_, so no work runs and
    // no row is added.
    s.handle_message_inserted_ui_(info.id, 0,
                                  make_event("$t1", "$root:example.org"));
    CHECK(s.prepped_event_ids.empty());
    CHECK(view.message_list()->messages().empty());

    // A normal (non-thread) message in the current room is processed and added.
    s.handle_message_inserted_ui_(info.id, 0, make_event("$m1", ""));
    REQUIRE(s.prepped_event_ids.size() == 1);
    CHECK(s.prepped_event_ids[0] == "$m1");
    REQUIRE(view.message_list()->messages().size() == 1);
    CHECK(view.message_list()->messages()[0].event_id == "$m1");

    // A second in-thread reply still does no work and adds no row.
    s.handle_message_inserted_ui_(info.id, 1,
                                  make_event("$t2", "$root:example.org"));
    CHECK(s.prepped_event_ids.size() == 1);
    CHECK(view.message_list()->messages().size() == 1);
}
