#include <catch2/catch_test_macros.hpp>

#include "access_test_util.h"
#include "tk/access_tree.h"
#include "tk/theme.h"
#include "tk_test_surface.h"
#include "views/MessageListView.h"

#include <string>
#include <vector>

using tesseract::views::MessageListView;
using tesseract::views::MessageRowData;
using namespace tk;

namespace
{
MessageRowData code_collapse_row(const std::string& id, int lines,
                                 bool with_prose)
{
    std::string code;
    for (int i = 0; i < lines; ++i)
        code += "line " + std::to_string(i) + "\n";
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = id;
    r.sender_name = "Alice";
    r.body = "code";
    r.formatted_body = std::string(with_prose ? "<p>intro</p>" : "") +
                       "<pre><code>" + code + "</code></pre>";
    return r;
}

// Few '\n' but very long lines: collapsible only through wrapping.
MessageRowData code_collapse_wrapped_row(const std::string& id)
{
    std::string code;
    for (int i = 0; i < 4; ++i)
    {
        for (int j = 0; j < 60; ++j)
            code += "word" + std::to_string(j) + " ";
        code += "\n";
    }
    MessageRowData r;
    r.kind = MessageRowData::Kind::Text;
    r.event_id = id;
    r.sender_name = "Alice";
    r.body = "code";
    r.formatted_body = "<p>intro</p><pre><code>" + code + "</code></pre>";
    return r;
}

struct CodeCollapseFixture
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(500, 900);
    MessageListView v;
    LayoutCtx lc{surface->factory(), Theme::light()};

    explicit CodeCollapseFixture(const MessageRowData& row)
    {
        v.set_messages({row}, false);
        relayout();
    }
    void relayout()
    {
        v.measure(lc, {500, 900});
        v.arrange(lc, {0, 0, 500, 900});
    }
    AccessNode tree() { return build_access_tree(&v); }
};
} // namespace

TEST_CASE("a long code block is shorter collapsed than expanded",
          "[message_list][code_collapse]")
{
    CodeCollapseFixture f(code_collapse_row("$a", 30, true));
    const float collapsed = f.v.content_height();

    AccessNode t = f.tree();
    const AccessNode* expand = access_test::find_named(t, "Expand code block");
    REQUIRE(expand != nullptr);
    CHECK(invoke_default_action(*expand));
    f.relayout();
    const float expanded = f.v.content_height();
    CHECK(expanded > collapsed);

    AccessNode t2 = f.tree();
    const AccessNode* collapse =
        access_test::find_named(t2, "Collapse code block");
    REQUIRE(collapse != nullptr);
    CHECK(access_test::find_named(t2, "Expand code block") == nullptr);
    CHECK(invoke_default_action(*collapse));
    f.relayout();
    CHECK(f.v.content_height() == collapsed);
}

TEST_CASE("a code-only message also gets the Expand action",
          "[message_list][code_collapse]")
{
    CodeCollapseFixture f(code_collapse_row("$a", 30, false));
    AccessNode t = f.tree();
    CHECK(access_test::find_named(t, "Expand code block") != nullptr);
}

TEST_CASE("a short code block is not collapsible",
          "[message_list][code_collapse]")
{
    CodeCollapseFixture f(code_collapse_row("$a", 5, true));
    AccessNode t = f.tree();
    CHECK(access_test::find_named(t, "Expand code block") == nullptr);
    CHECK(access_test::find_named(t, "Collapse code block") == nullptr);
}

TEST_CASE("a code block at the threshold stays expanded",
          "[message_list][code_collapse]")
{
    CodeCollapseFixture f(code_collapse_row("$a", 15, true));
    AccessNode t = f.tree();
    CHECK(access_test::find_named(t, "Expand code block") == nullptr);
}

TEST_CASE("wrapped lines count: a few very long lines collapse when narrow",
          "[message_list][code_collapse]")
{
    CodeCollapseFixture f(code_collapse_wrapped_row("$a"));
    AccessNode t = f.tree();
    CHECK(access_test::find_named(t, "Expand code block") != nullptr);
}
