#include <catch2/catch_test_macros.hpp>

#include "tesseract/media_source.h"
#include "view_test_util.h"
#include "views/RoomMediaView.h"

#include <ctime>
#include <string>
#include <vector>

using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;
using tesseract::views::RoomMediaView;

namespace
{

// timegm() is POSIX only; MSVC spells it _mkgmtime().
std::time_t rmv_timegm(std::tm* tm)
{
#ifdef _WIN32
    return _mkgmtime(tm);
#else
    return timegm(tm);
#endif
}

// 2026-<month>-15 12:00 UTC.
std::uint64_t rmv_ts(int month)
{
    std::tm tm{};
    tm.tm_year = 126;
    tm.tm_mon = month - 1;
    tm.tm_mday = 15;
    tm.tm_hour = 12;
    return static_cast<std::uint64_t>(rmv_timegm(&tm)) * 1000ull;
}

MessageRowData rmv_row(const std::string& id, MessageRowData::Kind kind, int month,
                       const std::string& sender = "")
{
    MessageRowData r;
    r.kind = kind;
    r.event_id = id;
    r.timestamp_ms = rmv_ts(month);
    r.sender_name = sender;
    r.body = id + ".png";
    r.source = tesseract::MediaSource::plain("mxc://x/" + id);
    r.media_w = 640;
    r.media_h = 480;
    return r;
}

struct RmvStage : vt::Stage
{
    std::unique_ptr<RoomMediaView> view = tk::create_root_widget<RoomMediaView>(&host);
    void run() { mount(*view, {0, 0, 800, 600}); }
    std::vector<tk::AccessNode> cells()
    {
        std::vector<tk::AccessNode> out;
        std::function<void(const tk::AccessNode&)> walk = [&](const tk::AccessNode& n)
        {
            if (n.role == tk::Role::Button && n.name != "Close")
                out.push_back(n);
            for (const auto& c : n.children)
                walk(c);
        };
        walk(tk::build_access_tree(view.get()));
        return out;
    }
};

} // namespace

TEST_CASE("RoomMediaView filters non-media rows, de-duplicates and groups by "
          "month",
          "[tk][view][room_media_grid]")
{
    RmvStage s;
    s.view->open("!r:x", "Room");
    CHECK(s.view->is_open());
    CHECK(s.view->room_id() == "!r:x");
    s.run();

    MessageRowData text;
    text.kind = MessageRowData::Kind::Text;
    text.event_id = "$t";
    s.view->set_media({rmv_row("$a", MessageRowData::Kind::Image, 3),
                       rmv_row("$a", MessageRowData::Kind::Image, 3), // duplicate
                       text,
                       rmv_row("$b", MessageRowData::Kind::Video, 4),
                       rmv_row("$c", MessageRowData::Kind::Image, 3)});
    CHECK(s.view->item_count() == 3);
    s.run();

    auto names = vt::all_names(*s.view);
    auto has = [&](const std::string& n)
    { return std::find(names.begin(), names.end(), n) != names.end(); };
    // Two month headers (March, April) are exposed as static text rows.
    int headers = 0;
    for (const auto& n : names)
        if (n.find("2026") != std::string::npos)
            ++headers;
    CHECK(headers >= 2);

    // prepend/append respect the same filters.
    s.view->prepend_media({rmv_row("$a", MessageRowData::Kind::Image, 3), // already present
                           rmv_row("$d", MessageRowData::Kind::Image, 1)});
    CHECK(s.view->item_count() == 4);
    s.view->prepend_media({});
    s.view->prepend_media({text});
    CHECK(s.view->item_count() == 4);
    s.view->append_live_media(rmv_row("$e", MessageRowData::Kind::Image, 5));
    s.view->append_live_media(rmv_row("$e", MessageRowData::Kind::Image, 5));
    s.view->append_live_media(text);
    CHECK(s.view->item_count() == 5);
    s.run();
}

TEST_CASE("RoomMediaView wraps cells into strips by available width",
          "[tk][view][room_media_grid]")
{
    RmvStage s;
    s.view->open("!r:x", "Room");
    std::vector<MessageRowData> rows;
    for (int i = 0; i < 20; ++i)
        rows.push_back(rmv_row("$m" + std::to_string(i), MessageRowData::Kind::Image, 3));
    s.view->set_media(std::move(rows));
    s.run();
    // 800px wide: (800 - 32) / 122 = 6 columns; capacity is rows * cols.
    CHECK(s.view->estimated_capacity() % 6 == 0);
    s.relayout_paint(*s.view, {0, 0, 400, 600}); // narrower: re-wraps
    CHECK(s.view->estimated_capacity() % 3 == 0);
    CHECK(s.view->content_fills_viewport());
    auto keys = s.view->collect_prefetchable_media_keys();
    CHECK_FALSE(keys.empty());
}

TEST_CASE("RoomMediaView activating cells routes images and videos to the "
          "right callbacks",
          "[tk][view][room_media_grid]")
{
    RmvStage s;
    s.view->open("!r:x", "Room");
    auto video = rmv_row("$v", MessageRowData::Kind::Video, 3, "Bob");
    video.video_mime = "video/mp4";
    video.duration_ms = 4000;
    video.video_gif = true;
    s.view->set_media({rmv_row("$i", MessageRowData::Kind::Image, 3), video});
    s.run();

    std::vector<std::string> images, videos;
    s.view->on_image_clicked = [&](const MessageListView::ImageHit& h) { images.push_back(h.event_id); };
    s.view->on_video_clicked = [&](const MessageListView::VideoHit& h)
    {
        videos.push_back(h.event_id + ":" + h.mime_type);
        CHECK(h.gif);
        CHECK(h.duration_ms == 4000);
    };

    auto cells = s.cells();
    REQUIRE(cells.size() == 2);
    for (const auto& c : cells)
        CHECK(tk::invoke_default_action(c));
    CHECK(images == std::vector<std::string>{"$i"});
    CHECK(videos == std::vector<std::string>{"$v:video/mp4"});
    bool saw_sender = false;
    for (const auto& c : cells)
        saw_sender |= c.name.find("from Bob") != std::string::npos;
    CHECK(saw_sender);

    // Real pointer click on a cell.
    images.clear();
    videos.clear();
    for (const auto& c : s.cells())
    {
        const tk::Point p{c.rect.x + c.rect.w / 2, c.rect.y + c.rect.h / 2};
        s.host.dispatch_pointer_down(p);
        s.host.dispatch_pointer_up(p);
    }
    CHECK(images.size() + videos.size() == 2);

    // Missing callbacks are tolerated.
    s.view->on_image_clicked = nullptr;
    s.view->on_video_clicked = nullptr;
    for (const auto& c : s.cells())
        tk::invoke_default_action(c);
}

TEST_CASE("RoomMediaView paints thumbnails via the image provider and shows "
          "placeholders",
          "[tk][view][room_media_grid]")
{
    RmvStage s;
    s.view->open("!r:x", "Room");
    s.run(); // empty + still loading
    s.view->set_reached_start(true);
    s.run(); // empty + reached start
    CHECK(s.view->on_wheel({1, 1}, 0, 1));

    std::vector<std::uint8_t> px(8 * 8 * 4, 90);
    auto img = s.surface->factory().create_image_rgba(px.data(), 8, 8);
    REQUIRE(img != nullptr);
    std::vector<std::string> asked;
    s.view->set_image_provider([&](const std::string& key) -> const tk::Image*
                               {
                                   asked.push_back(key);
                                   return asked.size() % 2 ? img.get() : nullptr;
                               });
    s.view->set_media({rmv_row("$i", MessageRowData::Kind::Image, 3),
                       rmv_row("$v", MessageRowData::Kind::Video, 3)});
    s.run();
    CHECK_FALSE(asked.empty());
    s.view->on_theme_changed(tk::Theme::dark());
    s.run();
}

TEST_CASE("RoomMediaView keyboard navigation moves between cells and "
          "activates",
          "[tk][view][room_media_grid]")
{
    RmvStage s;
    s.view->open("!r:x", "Room");
    s.view->set_media({rmv_row("$a", MessageRowData::Kind::Image, 3),
                       rmv_row("$b", MessageRowData::Kind::Image, 3),
                       rmv_row("$c", MessageRowData::Kind::Image, 4)});
    s.run();
    std::vector<std::string> images;
    s.view->on_image_clicked = [&](const MessageListView::ImageHit& h) { images.push_back(h.event_id); };

    auto* list = vt::find<tk::ListView>(*s.view);
    REQUIRE(list != nullptr);
    s.host.request_focus(list);
    if (!list->has_focus())
        SKIP("grid list not focusable in this configuration");
    s.host.dispatch_key_down(vt::key(tk::Key::Right)); // lands on first cell
    s.host.dispatch_key_down(vt::key(tk::Key::Right));
    s.host.dispatch_key_down(vt::key(tk::Key::Left));
    s.host.dispatch_key_down(vt::key(tk::Key::Enter));
    s.host.dispatch_key_down(vt::key(tk::Key::Right));
    s.host.dispatch_key_down(vt::key(tk::Key::Right));
    s.host.dispatch_key_down(vt::key(tk::Key::Space));
    s.run();
    CHECK_FALSE(images.empty());
}

TEST_CASE("RoomMediaView near-top callback and close",
          "[tk][view][room_media_grid]")
{
    RmvStage s;
    int closed = 0;
    s.view->on_close = [&] { ++closed; };
    s.view->close(); // not open: no-op
    CHECK(closed == 0);
    s.view->open("!r:x", "Room");
    REQUIRE(vt::press(*s.view, "Close"));
    CHECK(closed == 1);
    CHECK_FALSE(s.view->is_open());
    CHECK_FALSE(s.view->on_wheel({1, 1}, 0, 1));
    s.run(); // closed: paint is a no-op
}
