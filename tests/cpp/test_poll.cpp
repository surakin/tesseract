#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include "views/MessageListView.h"
#include "views/poll_logic.h"
#include "views/PollCardDisplay.h"
#include "views/CreatePollDialog.h"
#include "app/SlashCommands.h"
#include "tk_test_host.h"
#include "tk/access_tree.h"
#include "access_test_util.h"
#include "tk/theme.h"
#include "tk/widget.h"
#include "tk_test_surface.h"
#include <tesseract/types.h>

using namespace tesseract::views;

TEST_CASE("normalize_poll_draft trims and drops blank options", "[poll]")
{
    PollDraft d;
    d.question = "  Lunch?  ";
    d.options = {" Pizza ", "", "   ", "Sushi"};
    auto n = normalize_poll_draft(d);
    CHECK(n.question == "Lunch?");
    CHECK(n.options == std::vector<std::string>{"Pizza", "Sushi"});
}

TEST_CASE("poll_draft_valid", "[poll]")
{
    PollDraft ok{"Q", {"a", "b"}, false, false};
    CHECK(poll_draft_valid(ok));
    CHECK_FALSE(poll_draft_valid(PollDraft{"", {"a", "b"}, false, false}));
    CHECK_FALSE(poll_draft_valid(PollDraft{"Q", {"a"}, false, false}));
    CHECK_FALSE(poll_draft_valid(PollDraft{"Q", {"a", "a"}, false, false})); // duplicate text
    std::vector<std::string> many;
    for (int i = 0; i < 21; ++i) many.push_back("o" + std::to_string(i));
    CHECK_FALSE(poll_draft_valid(PollDraft{"Q", many, false, false}));
    many.pop_back();
    CHECK(poll_draft_valid(PollDraft{"Q", many, false, false})); // exactly 20
}

TEST_CASE("poll_draft_max_selections", "[poll]")
{
    CHECK(poll_draft_max_selections(PollDraft{"Q", {"a", "b", "c"}, false, false}) == 1);
    CHECK(poll_draft_max_selections(PollDraft{"Q", {"a", "b", "c"}, true, false}) == 3);
}

TEST_CASE("next_poll_selection single choice", "[poll]")
{
    CHECK(*next_poll_selection({}, "a", 1) == std::vector<std::string>{"a"});
    CHECK(*next_poll_selection({"a"}, "b", 1) == std::vector<std::string>{"b"});
    CHECK_FALSE(next_poll_selection({"a"}, "a", 1).has_value()); // no-op
    CHECK_FALSE(next_poll_selection({}, "", 1).has_value());
}

TEST_CASE("next_poll_selection multiple choice", "[poll]")
{
    CHECK(*next_poll_selection({"a"}, "b", 2) == std::vector<std::string>{"a", "b"});
    CHECK_FALSE(next_poll_selection({"a", "b"}, "c", 2).has_value()); // over the cap
    CHECK(*next_poll_selection({"a", "b"}, "a", 2) == std::vector<std::string>{"b"});
    auto cleared = next_poll_selection({"a"}, "a", 2);
    REQUIRE(cleared.has_value());
    CHECK(cleared->empty()); // untoggling the last one clears the vote
}

TEST_CASE("poll_percent never divides by zero", "[poll]")
{
    CHECK(poll_percent(0, 0) == 0);
    CHECK(poll_percent(5, 0) == 0);
    CHECK(poll_percent(1, 3) == 33);
    CHECK(poll_percent(2, 3) == 67);
    CHECK(poll_percent(3, 3) == 100);
}

static tesseract::PollEvent make_poll_event()
{
    tesseract::PollEvent ev;
    ev.event_id = "$poll:example.org";
    ev.sender = "@alice:example.org";
    ev.sender_name = "Alice";
    ev.body = "Lunch?";
    ev.timestamp = 1000;
    ev.max_selections = 2;
    ev.results_visible = true;
    ev.total_votes = 3;
    ev.answers = {{"a", "Pizza", 2, true}, {"b", "Sushi", 1, false}};
    return ev;
}

TEST_CASE("make_row_data maps PollEvent to Kind::Poll", "[poll]")
{
    auto row = make_row_data(make_poll_event(), "@me:example.org");
    CHECK(row.kind == MessageRowData::Kind::Poll);
    CHECK(row.body == "Lunch?");
    CHECK(row.poll_max_selections == 2);
    CHECK(row.poll_results_visible);
    CHECK(row.poll_total_votes == 3);
    REQUIRE(row.poll_answers.size() == 2);
    CHECK(row.poll_answers[0].text == "Pizza");
    CHECK(row.poll_answers[0].mine);
}

TEST_CASE("poll status text", "[poll]")
{
    auto row = make_row_data(make_poll_event(), "@me:example.org");
    CHECK(poll_status_text(row) == "3 votes");
    row.poll_ended = true;
    CHECK(poll_status_text(row) == "Poll ended \xC2\xB7 3 votes");
    row.poll_total_votes = 1;
    row.poll_ended = false;
    CHECK(poll_status_text(row) == "1 vote");
    row.poll_total_votes = 3;
    row.poll_results_visible = false;
    row.poll_total_votes = 0;
    CHECK(poll_status_text(row) == "Results will be shown when the poll ends");
    row.poll_ended = false;
    CHECK(poll_hint_text(row) == "Select up to 2 options");
    row.poll_max_selections = 1;
    CHECK(poll_hint_text(row) == "Select one option");
    // A foreign poll may declare more selections than it has answers.
    row.poll_max_selections = 9;
    CHECK(poll_hint_text(row) == "Select up to 2 options");
    row.poll_answers.resize(1);
    CHECK(poll_hint_text(row) == "Select one option");
    row.poll_ended = true;
    CHECK(poll_hint_text(row).empty());
}

TEST_CASE("poll_card_interactive and poll_show_end_button", "[poll]")
{
    auto row = make_row_data(make_poll_event(), "@me:example.org");
    CHECK(poll_card_interactive(row, true));
    CHECK_FALSE(poll_card_interactive(row, false)); // thread panel: no callbacks
    auto pending = row;
    pending.pending_state = MessageRowData::PendingState::Sending;
    CHECK_FALSE(poll_card_interactive(pending, true));
    auto noid = row;
    noid.event_id.clear();
    CHECK_FALSE(poll_card_interactive(noid, true));
    auto ended = row;
    ended.poll_ended = true;
    CHECK_FALSE(poll_card_interactive(ended, true));

    row.is_own = false;
    CHECK_FALSE(poll_show_end_button(row, false));
    CHECK(poll_show_end_button(row, true));
    row.is_own = true;
    CHECK(poll_show_end_button(row, false));
    row.poll_ended = true;
    CHECK_FALSE(poll_show_end_button(row, true));
}

TEST_CASE("poll card height grows with options and never shrinks below the 2-option card", "[poll]")
{
    auto surf = TestSurface::create(400, 400);
    PollCardDisplay card;
    auto row = make_row_data(make_poll_event(), "@me:example.org");
    const float h2 = card.height(row, surf->factory(), 360.0f, false);
    row.poll_answers.resize(20, row.poll_answers[0]);
    const float h20 = card.height(row, surf->factory(), 360.0f, false);
    CHECK(h20 > h2);
    row.poll_answers.clear();
    CHECK(card.height(row, surf->factory(), 360.0f, false) > 0.0f);
}

TEST_CASE("poll card wraps very long text and stays finite", "[poll]")
{
    auto surf = TestSurface::create(400, 400);
    PollCardDisplay card;
    auto row = make_row_data(make_poll_event(), "@me:example.org");
    row.body = std::string(4000, 'x');
    row.poll_answers[0].text = std::string(4000, 'y');
    const float h = card.height(row, surf->factory(), 200.0f, true);
    CHECK(h > 100.0f);
    CHECK(h < 1.0e6f);
}

TEST_CASE("poll card paints exactly its measured height and records hits only when interactive", "[poll]")
{
    auto surf = TestSurface::create(500, 800);
    const tk::Theme& theme = tk::Theme::light();
    tk::PaintCtx ctx{surf->canvas(), surf->factory(), theme};
    PollCardDisplay card;
    auto row = make_row_data(make_poll_event(), "@me:example.org");

    const float h = card.height(row, surf->factory(), 360.0f, true);
    // Reserved-space invariant: show_end must not change the measured height.
    CHECK(card.height(row, surf->factory(), 360.0f, false) == Catch::Approx(h));
    const float bottom = card.paint(row, ctx, 10.0f, 20.0f, 360.0f, false, true);
    CHECK(bottom == Catch::Approx(20.0f + h));
    CHECK(card.geometry().empty());

    card.paint(row, ctx, 10.0f, 20.0f, 360.0f, true, true);
    REQUIRE(card.geometry().size() == row.poll_answers.size() + 1);
    const auto& first = card.geometry().front();
    REQUIRE(first.type == PollCardDisplay::Hit::Type::Answer);
    const auto* hit = card.hit_test({first.rect.x + 1.0f, first.rect.y + 1.0f});
    REQUIRE(hit != nullptr);
    CHECK(hit->answer_id == row.poll_answers[0].id);
    CHECK(hit->event_id == row.event_id);
    CHECK(card.hit_test({first.rect.x - 5.0f, first.rect.y + 1.0f}) == nullptr);
    CHECK(card.geometry().back().type == PollCardDisplay::Hit::Type::EndButton);
    card.clear_geometry();
    CHECK(card.geometry().empty());

    // show_end == false: answers only.
    card.paint(row, ctx, 10.0f, 20.0f, 360.0f, true, false);
    CHECK(card.geometry().size() == row.poll_answers.size());
    CHECK(card.hit_test({-5.0f, -5.0f}) == nullptr);
}

TEST_CASE("/poll is a registered slash command", "[poll][slash]")
{
    bool found = false;
    for (const auto& c : tesseract::available_commands())
        if (c.name == "poll") { found = true; CHECK(c.args_hint.empty()); }
    CHECK(found);
    CHECK(tesseract::is_slash_command_no_arg("/poll", "poll"));
    CHECK(tesseract::is_slash_command_no_arg("/poll  ", "poll"));
    CHECK_FALSE(tesseract::is_slash_command_no_arg("/pollx", "poll"));
}

TEST_CASE("CreatePollDialog gates Create on a valid draft", "[poll][dialog]")
{
    StubHost host;
    auto dlg = tk::create_root_widget<CreatePollDialog>(&host);
    REQUIRE(dlg->question_field());
    CHECK_FALSE(dlg->is_open());

    int calls = 0;
    PollDraft got;
    dlg->open([&](PollDraft d) { ++calls; got = std::move(d); });
    CHECK(dlg->is_open());
    REQUIRE(dlg->option_count() == 2);
    CHECK_FALSE(dlg->create_enabled());

    dlg->question_field()->set_text("  Lunch?  ");
    dlg->option_field(0)->set_text("Pizza");
    // Native backends only notify on user edits; the dialog also re-checks
    // on every arrange, so lay out once.
    auto surface = TestSurface::create(800, 600);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    auto relayout = [&] {
        dlg->measure(lc, {800, 600});
        dlg->arrange(lc, {0, 0, 800, 600});
    };
    relayout();
    CHECK_FALSE(dlg->create_enabled());
    CHECK_FALSE(dlg->create_button()->enabled());

    dlg->option_field(1)->set_text("Sushi");
    relayout();
    CHECK(dlg->create_enabled());
    CHECK(dlg->create_button()->enabled());

    dlg->option_field(1)->set_text("Pizza"); // duplicate
    relayout();
    CHECK_FALSE(dlg->create_enabled());

    dlg->option_field(1)->set_text("Sushi");
    relayout();
    dlg->create_button()->click();
    CHECK(calls == 1);
    CHECK_FALSE(dlg->is_open());
    CHECK(got.question == "Lunch?");
    CHECK(got.options == std::vector<std::string>{"Pizza", "Sushi"});
    CHECK(poll_draft_valid(got));
}

TEST_CASE("CreatePollDialog add/remove options and checkboxes", "[poll][dialog]")
{
    StubHost host;
    auto dlg = tk::create_root_widget<CreatePollDialog>(&host);
    dlg->open([](PollDraft) {});
    for (int i = 0; i < 30; ++i)
        dlg->add_option();
    CHECK(dlg->option_count() == static_cast<std::size_t>(kPollMaxOptions));
    dlg->remove_option(5);
    CHECK(dlg->option_count() == static_cast<std::size_t>(kPollMaxOptions) - 1);
    while (dlg->option_count() > 2)
        dlg->remove_option(dlg->option_count() - 1);
    dlg->remove_option(0); // never below the minimum
    CHECK(dlg->option_count() == 2);

    dlg->multiple_check()->set_checked(true);
    dlg->question_field()->set_text("Q");
    dlg->option_field(0)->set_text("a");
    dlg->option_field(1)->set_text("b");
    CHECK(dlg->current_draft().multiple);
    CHECK_FALSE(dlg->current_draft().hide_results);
    CHECK(dlg->current_draft().options == std::vector<std::string>{"a", "b"});

    // Reopening resets the form.
    dlg->close();
    dlg->open([](PollDraft) {});
    CHECK(dlg->current_draft().question.empty());
    CHECK_FALSE(dlg->current_draft().multiple);
}

TEST_CASE("CreatePollDialog Enter in the last option reveals the new row", "[poll][dialog]")
{
    StubHost host;
    auto dlg = tk::create_root_widget<CreatePollDialog>(&host);
    dlg->open([](PollDraft) {});
    for (int i = 0; i < 8; ++i)
        dlg->add_option();
    REQUIRE(dlg->option_count() == 10);

    auto surface = TestSurface::create(800, 360); // short: options must scroll
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    auto relayout = [&] {
        dlg->measure(lc, {800, 360});
        dlg->arrange(lc, {0, 0, 800, 360});
    };
    relayout();
    auto* vp = dlg->options_scroll();
    REQUIRE(vp->bounds().h < 10 * 40.0f); // really clamped

    // Enter in the last option.
    dlg->enter_in_option(9);
    CHECK(dlg->option_count() == 11);
    relayout();
    const tk::Rect fb = dlg->option_field(10)->bounds();
    const tk::Rect vb = vp->bounds();
    CHECK(fb.y >= vb.y - 0.5f);
    CHECK(fb.y + fb.h <= vb.y + vb.h + 0.5f);
}

TEST_CASE("poll with an empty question reads as \"Poll\" and measures finitely", "[poll]")
{
    auto surf = TestSurface::create(400, 400);
    PollCardDisplay card;
    auto row = make_row_data(make_poll_event(), "@me:example.org");
    row.body.clear();
    CHECK(message_access_body(row) == "Poll");
    const float h = card.height(row, surf->factory(), 360.0f, false);
    CHECK(h > 0.0f);
    CHECK(h < 1.0e6f);
}

TEST_CASE("CreatePollDialog names the target room in its title", "[poll][dialog]")
{
    StubHost host;
    auto dlg = tk::create_root_widget<CreatePollDialog>(&host);
    dlg->open([](PollDraft) {});
    CHECK(dlg->access_name() == "Create poll");
    dlg->open([](PollDraft) {}, "Book Club");
    CHECK(dlg->access_name() == "Create poll in Book Club");
    dlg->open([](PollDraft) {});
    CHECK(dlg->access_name() == "Create poll");
}

namespace
{
// Row accessibility subtree of a poll row, as exposed by the message list.
const tk::AccessNode* poll_group(const tk::AccessNode& tree, const std::string& question)
{
    return access_test::find_named(tree, question);
}

MessageRowData ended_poll_row()
{
    auto row = make_row_data(make_poll_event(), "@me:example.org");
    row.poll_ended = true;
    return row;
}
} // namespace

TEST_CASE("read-only poll still exposes options, results and status to AT", "[poll][accessibility]")
{
    MessageListView v; // no on_poll_vote / on_poll_end_requested
    std::vector<MessageRowData> rows{ended_poll_row()};
    v.set_messages(std::move(rows), false);
    tk::AccessNode tree = tk::build_access_tree(&v);
    const tk::AccessNode* g = poll_group(tree, "Lunch?");
    REQUIRE(g != nullptr);
    CHECK(g->role == tk::Role::Group);
    CHECK(g->description == "Poll ended \xC2\xB7 3 votes");
    REQUIRE(g->children.size() == 2);
    CHECK(g->children[0].role == tk::Role::CheckBox); // max_selections == 2
    CHECK(g->children[0].name == "Pizza, 67%");
    CHECK(g->children[0].state.checked);
    CHECK_FALSE(g->children[1].state.checked);
    for (const auto& c : g->children)
        CHECK_FALSE(static_cast<bool>(c.activate));
}

TEST_CASE("interactive poll options carry activate; no options means no group",
          "[poll][accessibility]")
{
    MessageListView v;
    v.on_poll_vote = [](const std::string&, std::vector<std::string>) {};
    v.on_poll_end_requested = [](const std::string&) {};
    auto row = make_row_data(make_poll_event(), "@me:example.org");
    row.poll_max_selections = 1;
    v.set_messages({row}, false);
    {
        tk::AccessNode tree = tk::build_access_tree(&v);
        const tk::AccessNode* g = poll_group(tree, "Lunch?");
        REQUIRE(g != nullptr);
        CHECK(g->children[0].role == tk::Role::RadioButton);
        CHECK(static_cast<bool>(g->children[0].activate));
    }
    row.poll_answers.clear();
    v.set_messages({row}, false);
    {
        tk::AccessNode tree = tk::build_access_tree(&v);
        CHECK(poll_group(tree, "Lunch?") == nullptr);
    }
}

TEST_CASE("poll options with duplicate answer ids get distinct subtree ids",
          "[poll][accessibility]")
{
    MessageListView v;
    auto row = ended_poll_row();
    row.poll_answers[1].id = row.poll_answers[0].id;
    v.set_messages({row}, false);
    tk::AccessNode tree = tk::build_access_tree(&v);
    const tk::AccessNode* g = poll_group(tree, "Lunch?");
    REQUIRE(g != nullptr);
    REQUIRE(g->children.size() == 2);
    CHECK(g->children[0].subtree_id != g->children[1].subtree_id);
}

TEST_CASE("a poll cannot be forwarded", "[poll][accessibility]")
{
    MessageListView v;
    bool can_forward = true;
    bool called = false;
    v.on_more_requested = [&](const std::string&, tk::Rect, bool, bool, bool, bool fwd)
    {
        called = true;
        can_forward = fwd;
    };
    v.set_messages({ended_poll_row()}, false);
    tk::AccessNode tree = tk::build_access_tree(&v);
    const tk::AccessNode* more = access_test::find_named(tree, "More options");
    REQUIRE(more != nullptr);
    REQUIRE(static_cast<bool>(more->activate));
    more->activate();
    CHECK(called);
    CHECK_FALSE(can_forward);
}
