#include <catch2/catch_test_macros.hpp>

#include "views/html_spans.h"

#include <string>

using tesseract::views::html_to_spans;

namespace
{
std::string joined(const std::vector<tk::TextSpan>& spans)
{
    std::string s;
    for (const auto& sp : spans)
    {
        s += sp.text;
    }
    return s;
}

bool any_colored(const std::vector<tk::TextSpan>& spans)
{
    for (const auto& sp : spans)
    {
        if (sp.has_color)
        {
            return true;
        }
    }
    return false;
}
} // namespace

TEST_CASE("code highlight: a recognized language yields colored code spans",
          "[html_spans][highlight]")
{
    // Matrix convention: language class on the inner <code>.
    auto spans = html_to_spans(
        "<pre><code class=\"language-rust\">fn main() {}\n</code></pre>", false);
    REQUIRE_FALSE(spans.empty());
    CHECK(any_colored(spans));
    for (const auto& sp : spans)
    {
        CHECK(sp.code); // every run of the block is monospace
    }
    // The visible text round-trips (trailing newline trimmed by the parser).
    CHECK(joined(spans) == "fn main() {}");
}

TEST_CASE("code highlight: more than one color appears for highlighted code",
          "[html_spans][highlight]")
{
    auto spans = html_to_spans(
        "<pre><code class=\"language-rust\">fn x() {}\n</code></pre>", false);
    REQUIRE_FALSE(spans.empty());
    bool seen_color = false;
    tk::Color first{};
    bool have_first = false;
    bool differs = false;
    for (const auto& sp : spans)
    {
        if (!sp.has_color)
        {
            continue;
        }
        seen_color = true;
        if (!have_first)
        {
            first = sp.color;
            have_first = true;
        }
        else if (sp.color.r != first.r || sp.color.g != first.g ||
                 sp.color.b != first.b)
        {
            differs = true;
        }
    }
    CHECK(seen_color);
    CHECK(differs);
}

TEST_CASE("code highlight: unknown language falls back to one plain code span",
          "[html_spans][highlight]")
{
    auto spans = html_to_spans(
        "<pre><code class=\"language-nonsense-xyz\">abc\n</code></pre>", false);
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].code);
    CHECK_FALSE(spans[0].has_color);
    CHECK(spans[0].text == "abc");
}

TEST_CASE("code highlight: a language-less code block is not colored",
          "[html_spans][highlight]")
{
    auto spans = html_to_spans("<pre><code>plain code\n</code></pre>", false);
    REQUIRE(spans.size() == 1);
    CHECK(spans[0].code);
    CHECK_FALSE(spans[0].has_color);
    CHECK(spans[0].text == "plain code");
}

TEST_CASE("code highlight: language class on <pre> is also honored",
          "[html_spans][highlight]")
{
    auto spans = html_to_spans(
        "<pre class=\"language-rust\"><code>fn main() {}\n</code></pre>", false);
    REQUIRE_FALSE(spans.empty());
    CHECK(any_colored(spans));
}

TEST_CASE("code highlight: inline code stays uncolored",
          "[html_spans][highlight]")
{
    auto spans = html_to_spans("see <code>x = 1</code> here", false);
    REQUIRE_FALSE(spans.empty());
    CHECK_FALSE(any_colored(spans));
}

TEST_CASE("code highlight: entities inside a code block are decoded",
          "[html_spans][highlight]")
{
    // The highlighter must see real source ('<'), not the &lt; entity.
    auto spans = html_to_spans(
        "<pre><code class=\"language-rust\">let a = b &lt; c;\n</code></pre>",
        false);
    REQUIRE_FALSE(spans.empty());
    CHECK(joined(spans) == "let a = b < c;");
}
