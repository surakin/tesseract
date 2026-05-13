#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/theme.h"
#include "views/ComposeBar.h"
#include "views/MessageListView.h"
#include "tk_test_surface.h"

#include <string>

using namespace tk;
using Catch::Approx;
using tesseract::views::ComposeBar;
using tesseract::views::MessageRowData;

namespace {

struct Stage {
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 300);
    void run(Widget& root, Rect bounds) {
        LayoutCtx lc{ surface->factory(), Theme::light() };
        root.measure(lc, { bounds.w, bounds.h });
        root.arrange(lc, bounds);
        PaintCtx pc{ surface->canvas(), surface->factory(), Theme::light() };
        root.paint(pc);
    }
};

Button* find_send(ComposeBar& bar) {
    for (auto& ch : bar.children())
        if (auto* b = dynamic_cast<Button*>(ch.get()))
            if (b->label() == "Send") return b;
    return nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// MessageRowData
// ---------------------------------------------------------------------------

TEST_CASE("MessageRowData has_reply is false when in_reply_to_id is empty",
          "[reply][row]") {
    MessageRowData row;
    CHECK_FALSE(row.has_reply());
}

TEST_CASE("MessageRowData has_reply is true when in_reply_to_id is set",
          "[reply][row]") {
    MessageRowData row;
    row.in_reply_to_id          = "$abc123";
    row.in_reply_to_sender_name = "Alice";
    row.in_reply_to_body        = "Hello there";
    CHECK(row.has_reply());
}

// ---------------------------------------------------------------------------
// ComposeBar reply state
// ---------------------------------------------------------------------------

TEST_CASE("ComposeBar set_reply_to enables has_reply",
          "[reply][compose]") {
    ComposeBar bar;
    CHECK_FALSE(bar.has_reply());
    bar.set_reply_to("$evt1", "Alice", "Hello");
    CHECK(bar.has_reply());
    CHECK(bar.reply_event_id() == "$evt1");
}

TEST_CASE("ComposeBar clear_reply disables has_reply",
          "[reply][compose]") {
    ComposeBar bar;
    bar.set_reply_to("$evt1", "Bob", "Hi");
    REQUIRE(bar.has_reply());
    bar.clear_reply();
    CHECK_FALSE(bar.has_reply());
    CHECK(bar.reply_event_id().empty());
}

TEST_CASE("ComposeBar natural_height grows by kReplyBandH + kReplyBandGap when reply is set",
          "[reply][compose]") {
    ComposeBar bar;
    const float baseline = bar.natural_height();
    bar.set_reply_to("$evt1", "Alice", "Hello there");
    CHECK(bar.natural_height() ==
          Approx(baseline + ComposeBar::kReplyBandH + ComposeBar::kReplyBandGap));
}

TEST_CASE("ComposeBar natural_height shrinks back to baseline when reply is cleared",
          "[reply][compose]") {
    ComposeBar bar;
    const float baseline = bar.natural_height();
    bar.set_reply_to("$evt2", "Carol", "Something");
    REQUIRE(bar.natural_height() > baseline);
    bar.clear_reply();
    CHECK(bar.natural_height() == Approx(baseline));
}

TEST_CASE("ComposeBar on_send_reply fires with reply_event_id and body when reply is pending",
          "[reply][compose]") {
    Stage st;
    ComposeBar bar;
    bar.set_reply_to("$reply_target", "Dave", "Original message");
    bar.set_current_text("My reply");

    std::string got_id, got_body;
    bar.on_send_reply = [&](const std::string& id, const std::string& body) {
        got_id   = id;
        got_body = body;
    };
    int plain_fires = 0;
    bar.on_send = [&](const std::string&) { ++plain_fires; };

    st.run(bar, { 0, 0, 640, bar.natural_height() });

    Button* send = find_send(bar);
    REQUIRE(send);
    REQUIRE(send->enabled());
    send->click();

    CHECK(got_id   == "$reply_target");
    CHECK(got_body == "My reply");
    CHECK(plain_fires == 0);
}

TEST_CASE("ComposeBar on_send fires normally when no reply is pending",
          "[reply][compose]") {
    Stage st;
    ComposeBar bar;
    bar.set_current_text("plain text");

    std::string got_plain;
    bar.on_send = [&](const std::string& t) { got_plain = t; };
    int reply_fires = 0;
    bar.on_send_reply = [&](const std::string&, const std::string&) { ++reply_fires; };

    st.run(bar, { 0, 0, 640, ComposeBar::kMinHeight });

    Button* send = find_send(bar);
    REQUIRE(send);
    send->click();

    CHECK(got_plain  == "plain text");
    CHECK(reply_fires == 0);
}

TEST_CASE("ComposeBar send clears reply state afterward",
          "[reply][compose]") {
    Stage st;
    ComposeBar bar;
    bar.set_reply_to("$evt3", "Eve", "Snippet");
    bar.set_current_text("reply text");

    bar.on_send_reply = [](const std::string&, const std::string&) {};

    st.run(bar, { 0, 0, 640, bar.natural_height() });

    Button* send = find_send(bar);
    REQUIRE(send);
    send->click();

    CHECK_FALSE(bar.has_reply());
}

TEST_CASE("ComposeBar on_size_changed fires when reply is set and cleared",
          "[reply][compose]") {
    ComposeBar bar;
    int changes = 0;
    bar.on_size_changed = [&] { ++changes; };

    bar.set_reply_to("$evt4", "Frank", "Hello");
    CHECK(changes == 1);

    bar.clear_reply();
    CHECK(changes == 2);
}

// ---------------------------------------------------------------------------
// ComposeBar edit mode
// ---------------------------------------------------------------------------

TEST_CASE("MessageRowData is_edited defaults to false",
          "[edit][row]") {
    MessageRowData row;
    CHECK_FALSE(row.is_edited);
}

TEST_CASE("ComposeBar set_editing enables has_editing",
          "[edit][compose]") {
    ComposeBar bar;
    CHECK_FALSE(bar.has_editing());
    bar.set_editing("$edit_evt");
    CHECK(bar.has_editing());
    CHECK(bar.edit_event_id() == "$edit_evt");
}

TEST_CASE("ComposeBar clear_editing disables has_editing",
          "[edit][compose]") {
    ComposeBar bar;
    bar.set_editing("$edit_evt");
    REQUIRE(bar.has_editing());
    bar.clear_editing();
    CHECK_FALSE(bar.has_editing());
    CHECK(bar.edit_event_id().empty());
}

TEST_CASE("ComposeBar natural_height grows by kEditBandH + kEditBandGap when editing",
          "[edit][compose]") {
    ComposeBar bar;
    const float baseline = bar.natural_height();
    bar.set_editing("$edit_evt");
    CHECK(bar.natural_height() ==
          Approx(baseline + ComposeBar::kEditBandH + ComposeBar::kEditBandGap));
}

TEST_CASE("ComposeBar natural_height shrinks back when editing is cleared",
          "[edit][compose]") {
    ComposeBar bar;
    const float baseline = bar.natural_height();
    bar.set_editing("$edit_evt");
    REQUIRE(bar.natural_height() > baseline);
    bar.clear_editing();
    CHECK(bar.natural_height() == Approx(baseline));
}

TEST_CASE("ComposeBar on_send_edit fires with event_id and new body when editing",
          "[edit][compose]") {
    Stage st;
    ComposeBar bar;
    bar.set_editing("$edit_target");
    bar.set_current_text("edited content");

    std::string got_id, got_body;
    bar.on_send_edit = [&](const std::string& id, const std::string& body) {
        got_id   = id;
        got_body = body;
    };
    int plain_fires = 0;
    bar.on_send = [&](const std::string&) { ++plain_fires; };

    st.run(bar, { 0, 0, 640, bar.natural_height() });

    Button* send = find_send(bar);
    REQUIRE(send);
    REQUIRE(send->enabled());
    send->click();

    CHECK(got_id   == "$edit_target");
    CHECK(got_body == "edited content");
    CHECK(plain_fires == 0);
}

TEST_CASE("ComposeBar send clears edit state afterward",
          "[edit][compose]") {
    Stage st;
    ComposeBar bar;
    bar.set_editing("$edit_evt");
    bar.set_current_text("fixed text");
    bar.on_send_edit = [](const std::string&, const std::string&) {};

    st.run(bar, { 0, 0, 640, bar.natural_height() });

    Button* send = find_send(bar);
    REQUIRE(send);
    send->click();

    CHECK_FALSE(bar.has_editing());
}

TEST_CASE("ComposeBar set_editing clears any active reply mode",
          "[edit][compose]") {
    ComposeBar bar;
    bar.set_reply_to("$reply", "Alice", "Hi");
    REQUIRE(bar.has_reply());

    bar.set_editing("$edit");
    CHECK_FALSE(bar.has_reply());
    CHECK(bar.has_editing());
}

TEST_CASE("ComposeBar on_size_changed fires when editing is set and cleared",
          "[edit][compose]") {
    ComposeBar bar;
    int changes = 0;
    bar.on_size_changed = [&] { ++changes; };

    bar.set_editing("$edit_evt");
    CHECK(changes == 1);

    bar.clear_editing();
    CHECK(changes == 2);
}
