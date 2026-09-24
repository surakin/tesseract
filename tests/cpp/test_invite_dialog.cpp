#include <catch2/catch_test_macros.hpp>

#include "tk_test_host.h"
#include "views/InviteDialog.h"
#include "views/InviteFieldModel.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using tesseract::MentionSeg;
using tesseract::views::InviteDialog;
using tesseract::views::InviteFieldParse;
using tesseract::views::is_complete_mxid;
using tesseract::views::parse_invite_field;

namespace
{

const std::string kPill = "\xEF\xBF\xBC"; // U+FFFC

MentionSeg text_seg(std::string t)
{
    MentionSeg s;
    s.text = std::move(t);
    return s;
}

MentionSeg mention_seg(std::string uid, std::string name)
{
    MentionSeg s;
    s.kind         = MentionSeg::Kind::Mention;
    s.user_id      = std::move(uid);
    s.display_name = std::move(name);
    return s;
}

// A NativeTextArea with real pill semantics, modelled on the Qt6 backend:
// each pill is a U+FFFC in text(), insert_mention appends a trailing space
// and moves the caret after it, and every mutation fires on_changed
// synchronously (re-entrantly into the owner).
struct PillStubArea : StubTextArea
{
    struct Pill
    {
        std::string user_id;
        std::string name;
    };
    std::vector<Pill> pills;
    int cursor = 0;

    int placeholders_before(int pos) const
    {
        int n = 0;
        for (std::size_t i = 0; i + 3 <= static_cast<std::size_t>(pos) && i + 3 <= text_.size();)
        {
            if (text_.compare(i, 3, kPill) == 0)
            {
                ++n;
                i += 3;
            }
            else
                ++i;
        }
        return n;
    }

    void set_text(std::string t) override
    {
        text_ = std::move(t);
        pills.clear();
        cursor = static_cast<int>(text_.size());
    }
    void edit_(int start, int end, const std::string& with)
    {
        const int first = placeholders_before(start);
        const int last  = placeholders_before(end);
        pills.erase(pills.begin() + first, pills.begin() + last);
        text_ = text_.substr(0, start) + with + text_.substr(end);
        cursor = start + static_cast<int>(with.size());
    }
    void replace_range(int start, int end, std::string with) override
    {
        edit_(start, end, with);
        if (on_changed)
            on_changed(text_);
    }
    void insert_mention(int start, int end, const std::string& user_id,
                        const std::string& display_name, bool,
                        const tk::Image*) override
    {
        edit_(start, end, "");
        const int idx = placeholders_before(start);
        pills.insert(pills.begin() + idx, {user_id, display_name});
        text_.insert(static_cast<std::size_t>(start), kPill + " ");
        cursor = start + 4;
        if (on_changed)
            on_changed(text_);
    }
    std::vector<MentionSeg> composer_draft() const override
    {
        std::vector<MentionSeg> segs;
        std::size_t p = 0;
        std::string pending;
        for (std::size_t i = 0; i < text_.size();)
        {
            if (text_.compare(i, 3, kPill) == 0)
            {
                if (!pending.empty())
                    segs.push_back(text_seg(std::exchange(pending, {})));
                segs.push_back(mention_seg(pills[p].user_id, pills[p].name));
                ++p;
                i += 3;
                continue;
            }
            pending += text_[i++];
        }
        if (!pending.empty())
            segs.push_back(text_seg(pending));
        return segs;
    }
    int cursor_byte_pos() const override { return cursor; }
    void set_cursor_byte_pos(int pos) override { cursor = pos; }

    // Simulate the user typing/pasting at the caret.
    void type(const std::string& s)
    {
        text_.insert(static_cast<std::size_t>(cursor), s);
        cursor += static_cast<int>(s.size());
        if (on_changed)
            on_changed(text_);
    }
};

struct PillHost : StubHost
{
    std::unique_ptr<tk::NativeTextArea> make_text_area() override
    {
        auto a = std::make_unique<PillStubArea>();
        area = a.get();
        return a;
    }
    PillStubArea* area = nullptr;
};

struct Fixture
{
    PillHost host;
    std::unique_ptr<InviteDialog> dlg;
    std::vector<InviteDialog::UserEntry> roster{
        {"@alice:example.org", "Alice", ""},
        {"@bob:example.org", "Bob", ""},
        {"@carol:example.org", "Carol", ""},
    };
    std::vector<std::pair<std::string, bool>> resolves;

    Fixture()
    {
        dlg = tk::create_root_widget<InviteDialog>(&host);
        host.set_root(dlg.get());
        dlg->set_users_filter(
            [this](const std::string& needle)
            {
                std::vector<InviteDialog::UserEntry> out;
                for (const auto& e : roster)
                    if (needle.empty() || e.user_id.find(needle) != std::string::npos ||
                        e.display_name.find(needle) != std::string::npos)
                        out.push_back(e);
                return out;
            });
        dlg->set_user_lookup(
            [this](const std::string& id) -> std::optional<InviteDialog::UserEntry>
            {
                for (const auto& e : roster)
                    if (e.user_id == id)
                        return e;
                return std::nullopt;
            });
        dlg->on_resolve_user = [this](const std::string& id, bool debounce)
        { resolves.emplace_back(id, debounce); };
        dlg->open("!room:example.org", "Room");
        REQUIRE(host.area != nullptr);
    }
    PillStubArea& area() { return *host.area; }
};

} // namespace

// ── Parser ────────────────────────────────────────────────────────────────

TEST_CASE("is_complete_mxid accepts only @local:server", "[invite_dialog]")
{
    CHECK(is_complete_mxid("@a:b"));
    CHECK(is_complete_mxid("@alice:example.org"));
    CHECK_FALSE(is_complete_mxid("@alice"));
    CHECK_FALSE(is_complete_mxid("@:example.org"));
    CHECK_FALSE(is_complete_mxid("@alice:"));
    CHECK_FALSE(is_complete_mxid("alice:example.org"));
    CHECK_FALSE(is_complete_mxid("@al ice:example.org"));
}

TEST_CASE("parse_invite_field: token under the caret is the query, others commit",
          "[invite_dialog]")
{
    const std::string t = "@a:x.org, @b:y.org @c:z";
    InviteFieldParse p = parse_invite_field(t, {text_seg(t)}, static_cast<int>(t.size()));
    REQUIRE(p.committed.size() == 2);
    CHECK(p.committed[0].text == "@a:x.org");
    CHECK(p.committed[0].start == 0);
    CHECK(p.committed[0].end == 8);
    CHECK(p.committed[1].text == "@b:y.org");
    CHECK(p.query.text == "@c:z");
    CHECK(p.query.end == static_cast<int>(t.size()));
}

TEST_CASE("parse_invite_field: pills map to mention segments with byte offsets",
          "[invite_dialog]")
{
    const std::string t = kPill + " ali";
    auto p = parse_invite_field(
        t, {mention_seg("@alice:x", "Alice"), text_seg(" ali")}, static_cast<int>(t.size()));
    REQUIRE(p.pills.size() == 1);
    CHECK(p.pills[0].user_id == "@alice:x");
    CHECK(p.pills[0].start == 0);
    CHECK(p.query.text == "ali");
    CHECK(p.query.start == 4);
    CHECK(p.committed.empty());
}

TEST_CASE("parse_invite_field: non-mxid text and partial mxids never commit",
          "[invite_dialog]")
{
    const std::string t = "hello @bob world";
    auto p = parse_invite_field(t, {text_seg(t)}, static_cast<int>(t.size()));
    CHECK(p.committed.empty());
    CHECK(p.query.text == "world");

    // Caret right after a separator: empty query at the caret.
    const std::string u = "@a:x ";
    auto q = parse_invite_field(u, {text_seg(u)}, static_cast<int>(u.size()));
    CHECK(q.query.text.empty());
    CHECK(q.query.start == 5);
    REQUIRE(q.committed.size() == 1);
}

// ── Dialog ────────────────────────────────────────────────────────────────

TEST_CASE("InviteDialog: a known mxid followed by a space becomes a pill",
          "[invite_dialog]")
{
    Fixture f;
    f.area().type("@alice:example.org ");
    REQUIRE(f.area().pills.size() == 1);
    CHECK(f.area().pills[0].user_id == "@alice:example.org");
    CHECK(f.area().text() == kPill + " ");
    CHECK(f.dlg->selected_user_ids() == std::vector<std::string>{"@alice:example.org"});
    CHECK(f.dlg->rows().front().state == InviteDialog::RowState::Selected);
}

TEST_CASE("InviteDialog: pasting several mxids pills all but the one being typed",
          "[invite_dialog]")
{
    Fixture f;
    f.area().type("@alice:example.org, @bob:example.org @carol:example.org");
    REQUIRE(f.area().pills.size() == 2);
    CHECK(f.area().pills[0].user_id == "@alice:example.org");
    CHECK(f.area().pills[1].user_id == "@bob:example.org");
    CHECK(f.dlg->parse().query.text == "@carol:example.org");
    // The known mxid still being typed is invited too.
    CHECK(f.dlg->selected_user_ids() ==
          std::vector<std::string>{"@alice:example.org", "@bob:example.org",
                                   "@carol:example.org"});
    // Caret stays at the end, after the query.
    CHECK(f.area().cursor == static_cast<int>(f.area().text().size()));
}

TEST_CASE("InviteDialog: unknown mxids are resolved, then pilled or flagged",
          "[invite_dialog]")
{
    Fixture f;
    f.area().type("@dave:other.org @eve:other.org ");
    CHECK(std::count(f.resolves.begin(), f.resolves.end(),
                     std::pair<std::string, bool>{"@dave:other.org", false}) >= 1);
    CHECK(f.area().pills.empty());

    f.dlg->set_resolved_user("@eve:other.org", std::nullopt);
    f.dlg->set_resolved_user("@dave:other.org",
                             InviteDialog::UserEntry{"@dave:other.org", "Dave", ""});
    REQUIRE(f.area().pills.size() == 1);
    CHECK(f.area().pills[0].user_id == "@dave:other.org");

    bool not_found = false;
    for (const auto& r : f.dlg->rows())
        if (r.user.user_id == "@eve:other.org")
            not_found = r.state == InviteDialog::RowState::NotFound;
    CHECK(not_found);
    CHECK(f.dlg->selected_user_ids() == std::vector<std::string>{"@dave:other.org"});
}

TEST_CASE("InviteDialog: the mxid being typed is looked up with debounce",
          "[invite_dialog]")
{
    Fixture f;
    f.area().type("@dave:other.org");
    REQUIRE_FALSE(f.resolves.empty());
    CHECK(f.resolves.back() == std::pair<std::string, bool>{"@dave:other.org", true});
    CHECK(f.dlg->rows().front().state == InviteDialog::RowState::Resolving);
}

TEST_CASE("InviteDialog: toggling rows inserts and removes pills", "[invite_dialog]")
{
    Fixture f;
    f.area().type("Ali");
    REQUIRE(f.dlg->rows().size() == 1);
    f.dlg->toggle_highlighted();
    REQUIRE(f.area().pills.size() == 1);
    CHECK(f.area().text() == kPill + " "); // filter text replaced by the pill
    CHECK(f.dlg->parse().query.text.empty());

    // Empty filter lists everyone; Alice is pinned first and checked.
    REQUIRE(f.dlg->rows().size() == 3);
    CHECK(f.dlg->rows()[0].user.user_id == "@alice:example.org");
    CHECK(f.dlg->rows()[0].state == InviteDialog::RowState::Selected);

    // Unchecking removes the pill again.
    f.dlg->move_highlight(-10);
    f.dlg->toggle_highlighted();
    CHECK(f.area().pills.empty());
    CHECK(f.area().text().empty());
}

TEST_CASE("InviteDialog: deleting a pill in the field unchecks its row",
          "[invite_dialog]")
{
    Fixture f;
    f.area().type("@bob:example.org ");
    REQUIRE(f.area().pills.size() == 1);
    f.area().replace_range(0, 3, ""); // Backspace over the pill
    CHECK(f.area().pills.empty());
    for (const auto& r : f.dlg->rows())
        CHECK(r.state != InviteDialog::RowState::Selected);
    CHECK(f.dlg->selected_user_ids().empty());
}

TEST_CASE("InviteDialog: existing members are listed but not selectable",
          "[invite_dialog]")
{
    Fixture f;
    f.dlg->set_existing_members({"@bob:example.org"});
    f.area().type("@bob:example.org ");
    CHECK(f.area().pills.empty());
    CHECK(f.dlg->selected_user_ids().empty());
    bool member = false;
    for (const auto& r : f.dlg->rows())
        if (r.user.user_id == "@bob:example.org")
            member = r.state == InviteDialog::RowState::AlreadyMember;
    CHECK(member);
}

TEST_CASE("InviteDialog: confirm reports the selection and tracks completion",
          "[invite_dialog]")
{
    Fixture f;
    std::vector<std::string> sent;
    f.dlg->on_invite_confirmed = [&](std::vector<std::string> ids) { sent = ids; };
    f.area().type("@alice:example.org @bob:example.org ");
    f.dlg->confirm();
    CHECK(sent == std::vector<std::string>{"@alice:example.org", "@bob:example.org"});

    f.dlg->set_inviting(2);
    f.dlg->add_invite_error("@bob:example.org", "banned");
    f.dlg->mark_complete();
    CHECK(f.dlg->is_open()); // errors keep it open until dismissed

    f.dlg->close();
    f.dlg->open("!room:example.org", "Room");
    f.dlg->set_inviting(1);
    f.dlg->mark_complete();
    CHECK_FALSE(f.dlg->is_open());
}

TEST_CASE("InviteDialog: confirm waits for pending lookups", "[invite_dialog]")
{
    Fixture f;
    int fired = 0;
    f.dlg->on_invite_confirmed = [&](std::vector<std::string>) { ++fired; };
    f.area().type("@alice:example.org @dave:other.org ");
    f.dlg->confirm();
    CHECK(fired == 0);
    f.dlg->set_resolved_user("@dave:other.org", std::nullopt);
    f.dlg->confirm();
    CHECK(fired == 1);
}
