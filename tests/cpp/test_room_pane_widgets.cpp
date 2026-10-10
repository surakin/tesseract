// RoomPane.cpp with the optional widgets attached: image / video lightboxes,
// the forward picker round trip, the room media gallery open / close, and the
// video fetch (disk cache, classification, full-buffer fallback).

#include <catch2/catch_test_macros.hpp>

#include "app/RoomPane.h"
#include "app/ShellBase.h"
#include "fake_media_players.h"
#include "settings_guard.h"
#include "shell_test_double.h"
#include "views/ForwardRoomPicker.h"
#include "views/MediaViewerOverlay.h"
#include "views/MessageListView.h"
#include "views/RoomMediaView.h"
#include "views/RoomView.h"

#include <tesseract/account_session.h>
#include <tesseract/client.h>
#include <tesseract/media_source.h>

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

using tesseract::RoomPane;
using tesseract::ShellBase;

namespace
{

struct RwShell : tesseract::test::TestShellBase
{
    ~RwShell() override
    {
        pool_.wait_idle(std::chrono::seconds(5));
        mut_pool_.wait_idle(std::chrono::seconds(5));
        main_room_pane_.reset();
        pool_.drain();
        mut_pool_.drain();
        media_prefetch_pool_.drain();
    }
    void apply_thread_messages_(
        const std::string&, std::vector<tesseract::views::MessageRowData>,
        bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}
    void post_to_ui_(std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back(std::move(fn));
    }
    void post_to_ui_after_(int, std::function<void()> fn) override
    {
        std::lock_guard<std::mutex> lk(mu);
        queue.push_back(std::move(fn));
    }
    void on_show_status_message_ui_(const std::string& m) override
    {
        statuses.push_back(m);
    }
    void pump()
    {
        for (int i = 0; i < 50; ++i)
        {
            pool_.wait_idle(std::chrono::seconds(5));
            mut_pool_.wait_idle(std::chrono::seconds(5));
            std::vector<std::function<void()>> q;
            {
                std::lock_guard<std::mutex> lk(mu);
                q = std::move(queue);
                queue.clear();
            }
            if (q.empty())
                break;
            for (auto& f : q)
                f();
        }
    }
    std::mutex mu;
    std::vector<std::function<void()>> queue;
    std::vector<std::string> statuses;

    using ShellBase::active_account_;
    using ShellBase::client_;
    using ShellBase::current_room_id_;
    using ShellBase::handle_media_ready_ui_;
    using ShellBase::main_room_pane_;
    using ShellBase::mark_room_index_dirty_;
    using ShellBase::my_user_id_;
    using ShellBase::next_request_id_;
    using ShellBase::pending_media_;
    using ShellBase::rooms_;
};

struct RwFx
{
    tesseract::test::SettingsGuard guard;
    tesseract::Client client;
    RwShell s;
    std::unique_ptr<tesseract::views::RoomView> view =
        tk::create_root_widget<tesseract::views::RoomView>(nullptr);
    std::unique_ptr<tesseract::views::MediaViewerOverlay> viewer =
        tk::create_root_widget<tesseract::views::MediaViewerOverlay>(nullptr);
    std::unique_ptr<tesseract::views::ForwardRoomPicker> fwd =
        tk::create_root_widget<tesseract::views::ForwardRoomPicker>(nullptr);
    std::unique_ptr<tesseract::views::RoomMediaView> gallery =
        tk::create_root_widget<tesseract::views::RoomMediaView>(nullptr);
    int relayouts = 0, field_focus = 0, field_hide = 0;

    RwFx()
    {
        tesseract::RoomInfo info;
        info.id = "!r:x";
        info.name = "Room";
        view->set_room(info);
        tesseract::RoomInfo other;
        other.id = "!other:x";
        other.name = "Other";
        s.rooms_ = {info, other};
        s.mark_room_index_dirty_();
        s.current_room_id_ = "!r:x";
        s.my_user_id_ = "@me:x";
        s.active_account_ = std::make_shared<tesseract::AccountSession>();
        s.active_account_->user_id = "@me:x";
        s.active_account_->client = std::make_unique<tesseract::Client>();
        s.client_ = s.active_account_->client.get();
        RoomPane::Widgets w;
        w.room_view = view.get();
        w.media_viewer = viewer.get();
        w.forward_picker = fwd.get();
        w.room_media_view = gallery.get();
        w.focus_forward_picker_field = [this] { ++field_focus; };
        w.hide_forward_picker_field = [this] { ++field_hide; };
        s.main_room_pane_ = std::make_unique<RoomPane>(
            RoomPane::Deps{.shell = &s,
                           .repaint = [] {},
                           .relayout = [this] { ++relayouts; }},
            "!r:x");
        s.main_room_pane_->attach(std::move(w));
    }
    RoomPane& pane() { return *s.main_room_pane_; }
};

} // namespace

TEST_CASE("clicking an image opens the lightbox and Escape-style close hides it",
          "[roompane][widgets]")
{
    RwFx f;
    tesseract::views::MessageListView::ImageHit hit;
    hit.event_id = "$i";
    hit.source = tesseract::MediaSource::plain("mxc://hs/full");
    hit.thumbnail = tesseract::MediaSource::plain("mxc://hs/thumb");
    hit.body = "caption";
    hit.natural_w = 400;
    hit.natural_h = 300;
    f.view->on_image_clicked(hit);
    CHECK(f.viewer->is_open());
    CHECK(f.relayouts >= 1);

    f.viewer->on_close();
    CHECK_FALSE(f.viewer->visible());

    f.view->on_avatar_clicked("mxc://hs/avatar", "Alice");
    CHECK(f.viewer->is_open());
    f.view->on_avatar_clicked("", "nobody"); // empty url: ignored
}

namespace
{
tesseract::views::MessageRowData::GalleryItemRow
rp_gallery_item(tesseract::views::MessageRowData::GalleryItemRow::Kind kind,
                const char* mxc)
{
    tesseract::views::MessageRowData::GalleryItemRow r;
    r.kind = kind;
    r.source = tesseract::MediaSource::plain(mxc);
    r.media_w = 640;
    r.media_h = 360;
    r.mime_type = kind == tesseract::views::MessageRowData::GalleryItemRow::Kind::Video
                      ? "video/mp4"
                      : "image/png";
    return r;
}
} // namespace

TEST_CASE("clicking a gallery cell opens the viewer at that cell, audio and files included",
          "[roompane][widgets][gallery]")
{
    using Row = tesseract::views::MessageRowData::GalleryItemRow;
    RwFx f;
    tesseract::views::MessageListView::GalleryHit hit;
    hit.event_id = "$g";
    hit.caption = "trip";
    hit.items = {rp_gallery_item(Row::Kind::File, "mxc://hs/gf"),
                 rp_gallery_item(Row::Kind::Image, "mxc://hs/g1"),
                 rp_gallery_item(Row::Kind::Audio, "mxc://hs/ga"),
                 rp_gallery_item(Row::Kind::Video, "mxc://hs/g2"),
                 rp_gallery_item(Row::Kind::Image, "mxc://hs/g3")};

    // A file cell opens the viewer on the file page (nothing to fetch).
    hit.index = 0;
    f.view->on_gallery_item_clicked(hit);
    REQUIRE(f.viewer->is_open());
    CHECK(f.viewer->count() == 5);
    CHECK(f.viewer->index() == 0);
    CHECK(f.viewer->current_item().kind ==
          tesseract::views::MediaViewerItem::Kind::File);
    f.viewer->on_close();

    hit.index = 3; // the video
    f.view->on_gallery_item_clicked(hit);
    REQUIRE(f.viewer->is_open());
    CHECK(f.viewer->count() == 5);
    CHECK(f.viewer->index() == 3);
    CHECK(f.viewer->current_item().kind ==
          tesseract::views::MediaViewerItem::Kind::Video);
    CHECK(f.viewer->current_item().caption == "trip");
    CHECK(f.relayouts >= 1);

    // Stepping on shows the next image and drops the old video's fetch.
    f.s.pump();
    std::vector<std::uint64_t> before;
    for (const auto& [id, req] : f.s.pending_media_)
    {
        before.push_back(id);
    }
    REQUIRE_FALSE(before.empty()); // at least the video's prefix request
    f.viewer->on_key_down(tk::KeyEvent{tk::Key::PageDown});
    CHECK(f.viewer->index() == 4);
    CHECK(f.viewer->current_item().kind ==
          tesseract::views::MediaViewerItem::Kind::Image);
    std::size_t still_pending = 0;
    for (std::uint64_t id : before)
    {
        still_pending += f.s.pending_media_.count(id);
    }
    CHECK(still_pending < before.size()); // the video's fetch group was cancelled

    f.viewer->on_close();
}

TEST_CASE("viewer playback stops the timeline's active audio clip",
          "[roompane][widgets][audio]")
{
    using tesseract::views::MediaViewerItem;
    using tesseract::views::MessageRowData;
    RwFx f;
    auto* mlv = f.view->message_list();
    REQUIRE(mlv != nullptr);

    auto timeline_player = std::make_unique<tesseract::test::FakeAudioPlayer>();
    auto* timeline_fake = timeline_player.get();
    auto& ctrl = mlv->playback_controller();
    ctrl.set_player(std::move(timeline_player));
    ctrl.set_bytes_provider([](const std::string&) { return std::vector<std::uint8_t>{1, 2, 3}; });

    MessageRowData voice;
    voice.kind = MessageRowData::Kind::Voice;
    voice.event_id = "$voice";
    voice.audio_source = tesseract::MediaSource::plain("mxc://hs/voice");
    voice.audio_mime = "audio/ogg";
    ctrl.handle_voice_play_click(voice);
    REQUIRE(timeline_fake->is_playing());

    auto viewer_player = std::make_unique<tesseract::test::FakeAudioPlayer>();
    auto* viewer_fake = viewer_player.get();
    f.viewer->set_audio_player(std::move(viewer_player));

    // Opening an audio item alone does not interrupt the timeline clip...
    MediaViewerItem item;
    item.kind = MediaViewerItem::Kind::Audio;
    item.source = tesseract::MediaSource::plain("mxc://hs/aud")->fetch_token();
    item.mime_type = "audio/mpeg";
    f.viewer->open(item);
    CHECK(timeline_fake->is_playing());

    // ...starting viewer playback does.
    f.viewer->load_audio_bytes(f.viewer->load_token(), {1, 2, 3});
    CHECK(viewer_fake->is_playing());
    CHECK_FALSE(timeline_fake->is_playing());
    f.viewer->on_close();
}

TEST_CASE("clicking a video opens the player and fetches through the cache, "
          "classification and buffer stages",
          "[roompane][widgets][video]")
{
    RwFx f;
    tesseract::views::MessageListView::VideoHit hit;
    hit.event_id = "$v";
    hit.source = tesseract::MediaSource::plain("mxc://hs/video-widgets");
    hit.thumbnail = tesseract::MediaSource::plain("mxc://hs/vthumb");
    hit.mime_type = "video/mp4";
    hit.duration_ms = 1000;
    hit.natural_w = 640;
    hit.natural_h = 360;
    f.view->on_video_clicked(hit);
    CHECK(f.viewer->is_open());

    f.s.pump(); // disk miss -> prefix request registered
    REQUIRE(f.s.pending_media_.size() == 1);
    // An empty prefix classifies as non-fast-start: the full buffer is fetched.
    f.s.handle_media_ready_ui_(f.s.pending_media_.begin()->first, {});
    REQUIRE(f.s.pending_media_.size() == 1);
    f.s.handle_media_ready_ui_(f.s.pending_media_.begin()->first, {});
    f.s.pump();

    f.viewer->on_close(); // cancels any fetch still pending for this video
    CHECK_FALSE(f.viewer->visible());
}

TEST_CASE("a late video cache miss for a closed or stepped-past item starts no download",
          "[roompane][widgets][video]")
{
    using Row = tesseract::views::MessageRowData::GalleryItemRow;
    {
        // Closed before the (off-thread) cache lookup returned.
        RwFx f;
        tesseract::views::MessageListView::GalleryHit hit;
        hit.event_id = "$g";
        hit.items = {rp_gallery_item(Row::Kind::Video, "mxc://hs/late-a"),
                     rp_gallery_item(Row::Kind::File, "mxc://hs/late-b")};
        hit.index = 0;
        f.view->on_gallery_item_clicked(hit);
        REQUIRE(f.viewer->is_open());
        f.viewer->close();
        f.s.pump();
        CHECK(f.s.pending_media_.empty());
    }
    {
        // Stepped to another video before the first lookup returned: only the
        // current item's fetch is in flight afterwards.
        RwFx f;
        tesseract::views::MessageListView::GalleryHit hit;
        hit.event_id = "$g2";
        hit.items = {rp_gallery_item(Row::Kind::Video, "mxc://hs/late-c"),
                     rp_gallery_item(Row::Kind::Video, "mxc://hs/late-d")};
        hit.index = 0;
        f.view->on_gallery_item_clicked(hit);
        REQUIRE(f.viewer->is_open());
        f.viewer->on_key_down(tk::KeyEvent{tk::Key::PageDown});
        REQUIRE(f.viewer->index() == 1);
        f.s.pump();
        CHECK(f.s.pending_media_.size() == 1);
        f.viewer->on_close();
    }
}

TEST_CASE("install_media_viewer_save_ asks the shell for a path and saves the item",
          "[roompane][widgets][save]")
{
    using tesseract::views::MediaViewerItem;
    RwFx f;
    std::string asked_name;
    std::function<void(std::string)> deferred;
    f.pane().install_media_viewer_save_(
        [&](const tesseract::views::MediaSaveSpec& spec,
            std::function<void(std::string)> done)
        {
            asked_name = spec.suggested_name;
            deferred = std::move(done);
        });
    MediaViewerItem item;
    item.kind = MediaViewerItem::Kind::File;
    item.source = tesseract::MediaSource::plain("mxc://hs/save-me")->fetch_token();
    item.filename = "report.pdf";
    f.viewer->open(item);
    REQUIRE(f.viewer->on_save);

    f.viewer->on_save(item);
    CHECK(asked_name == "report.pdf");
    REQUIRE(deferred);
    // Cancelled dialog (empty path) or no completion: nothing is fetched.
    deferred("");
    CHECK(f.s.pending_media_.empty());
    // Chosen path (possibly delivered later, as async dialogs do): fetch starts.
    deferred("/tmp/tesseract-test-save-never-written.pdf");
    CHECK(f.s.pending_media_.size() == 1);
    f.viewer->on_close();
}

TEST_CASE("the forward picker confirms, tracks each request and reports failures",
          "[roompane][widgets][forward]")
{
    RwFx f;
    f.view->on_forward_requested("$e");
    CHECK(f.fwd->is_open());
    CHECK(f.field_focus == 1);
    f.view->on_forward_requested("$e"); // already open: ignored
    CHECK(f.field_focus == 1);

    const auto first = f.s.next_request_id_;
    REQUIRE(f.fwd->on_confirmed);
    f.fwd->on_confirmed({"!other:x", "!r:x"});
    CHECK_FALSE(f.pane().handle_forward_done_(9999)); // not ours

    CHECK(f.pane().handle_forward_failed_(first, "denied"));
    CHECK(f.fwd->is_open()); // one still pending
    CHECK_FALSE(f.pane().handle_forward_failed_(first, "again"));
    CHECK(f.pane().handle_forward_done_(first + 1));
    CHECK_FALSE(f.fwd->is_open()); // all resolved: closed

    const int hidden = f.field_hide;
    CHECK(hidden >= 1); // closing the picker hid its native field
    f.fwd->on_close();
    CHECK(f.field_hide == hidden + 1);
}

TEST_CASE("forwarding without a client does nothing", "[roompane][widgets][forward]")
{
    RwFx f;
    f.view->on_forward_requested("$e");
    f.s.active_account_.reset();
    f.s.client_ = nullptr;
    const auto before = f.s.next_request_id_;
    f.fwd->on_confirmed({"!other:x"});
    CHECK(f.s.next_request_id_ == before);
}

TEST_CASE("the media gallery opens for the room and closes cleanly",
          "[roompane][widgets][gallery]")
{
    RwFx f;
    f.view->on_media_view_requested("!r:x");
    CHECK(f.gallery->is_open());
    REQUIRE(f.gallery->on_close);
    f.gallery->close(); // fires on_close -> the pane's own cleanup
    CHECK_FALSE(f.gallery->is_open());
    f.s.pump();
}

TEST_CASE("gallery feeds only apply to the room the gallery shows",
          "[roompane][widgets][gallery]")
{
    RwFx f;
    tesseract::views::MessageRowData img;
    img.kind = tesseract::views::MessageRowData::Kind::Image;
    img.event_id = "$img";
    tesseract::views::MessageRowData text;
    text.kind = tesseract::views::MessageRowData::Kind::Text;
    text.event_id = "$txt";

    // Closed gallery: every feed is ignored.
    f.pane().feed_gallery_live_("!r:x", img, false);
    f.pane().feed_gallery_prepend_batch_("!r:x", {img});
    f.pane().feed_gallery_reset_("!r:x", {img});

    f.view->on_media_view_requested("!r:x");
    f.pane().feed_gallery_live_("!other:x", img, false); // another room
    f.pane().feed_gallery_live_("!r:x", img, false);
    f.pane().feed_gallery_live_("!r:x", img, true);
    f.pane().feed_gallery_prepend_batch_("!r:x", {text}); // no media: ignored
    f.pane().feed_gallery_prepend_batch_("!r:x", {img, text});
    f.pane().feed_gallery_reset_("!r:x", {img});
    f.pane().feed_gallery_reset_("!other:x", {});
    f.s.pump();
    SUCCEED();
}
