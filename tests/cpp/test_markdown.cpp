#include <catch2/catch_test_macros.hpp>
#include <tesseract/markdown.h>

#include <string>

using tesseract::markdown_to_html;

namespace
{
bool contains(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}
} // namespace

TEST_CASE("markdown: plain text yields empty formatted_body")
{
    auto r = markdown_to_html("just plain text, no markers");
    CHECK(r.body == "just plain text, no markers");
    CHECK(r.formatted_body.empty());
}

TEST_CASE("markdown: body is always the unchanged input")
{
    auto r = markdown_to_html("**bold** and _em_");
    CHECK(r.body == "**bold** and _em_");
    CHECK_FALSE(r.formatted_body.empty());
}

TEST_CASE("markdown: a lone marker with no pair is not formatting")
{
    // '*' present (passes the fast-path) but no closing marker, so no
    // formatting tag is emitted -> treated as plain (empty formatted_body).
    auto r = markdown_to_html("5 * 3 = 15");
    CHECK(r.formatted_body.empty());
}

TEST_CASE("markdown: inline emphasis")
{
    CHECK(contains(markdown_to_html("**b**").formatted_body,
                   "<strong>b</strong>"));
    CHECK(contains(markdown_to_html("*i*").formatted_body, "<em>i</em>"));
    CHECK(contains(markdown_to_html("***bi***").formatted_body,
                   "<strong><em>bi</em></strong>"));
    CHECK(contains(markdown_to_html("~~s~~").formatted_body, "<del>s</del>"));
    auto code = markdown_to_html("`x<y`").formatted_body;
    CHECK(contains(code, "<code>x&lt;y</code>")); // escaped inside code
}

TEST_CASE("markdown: underscore emphasis only at word boundaries")
{
    CHECK(contains(markdown_to_html("_em_").formatted_body, "<em>em</em>"));
    CHECK(contains(markdown_to_html("__bold__").formatted_body,
                   "<strong>bold</strong>"));
    // Intra-word underscores must NOT emphasise (snake_case identifiers).
    auto r = markdown_to_html("a_b_c snake_case_name");
    CHECK(r.formatted_body.empty());
}

TEST_CASE("markdown: links are http(s) only")
{
    auto ok = markdown_to_html("[site](https://example.org)").formatted_body;
    CHECK(contains(ok, "<a href=\"https://example.org\">site</a>"));

    // javascript: / other schemes must never become an <a>.
    auto bad = markdown_to_html("[x](javascript:alert(1))");
    CHECK_FALSE(contains(bad.formatted_body, "<a "));
    CHECK_FALSE(
        contains(bad.formatted_body, "javascript:alert")); // escaped/plain
}

TEST_CASE("markdown: HTML is escaped (XSS guard)")
{
    auto r = markdown_to_html("**<script>alert('x')</script>**");
    CHECK(contains(r.formatted_body, "<strong>"));
    CHECK(contains(r.formatted_body, "&lt;script&gt;"));
    CHECK_FALSE(contains(r.formatted_body, "<script>"));
}

TEST_CASE("markdown: blockquote / lists")
{
    CHECK(contains(markdown_to_html("> quoted").formatted_body,
                   "<blockquote>quoted</blockquote>"));
    auto ul = markdown_to_html("- one\n- two").formatted_body;
    CHECK(contains(ul, "<ul><li>one</li><li>two</li></ul>"));
    auto ol = markdown_to_html("1. a\n2. b").formatted_body;
    CHECK(contains(ol, "<ol><li>a</li><li>b</li></ol>"));
}

TEST_CASE("markdown: fenced code block escapes content")
{
    auto r = markdown_to_html("```\nlet x = a < b;\n```");
    CHECK(contains(r.formatted_body, "<pre><code>"));
    CHECK(contains(r.formatted_body, "let x = a &lt; b;"));
    CHECK(contains(r.formatted_body, "</code></pre>"));
}

TEST_CASE("markdown: deep nesting is bounded (no crash/hang)")
{
    std::string s(200, '*'); // pathological run of emphasis markers
    auto r = markdown_to_html(s);
    CHECK(r.body == s); // returns; body unchanged
    // Past the recursion cap the remainder is emitted escaped, never raw.
    CHECK_FALSE(contains(r.formatted_body, "<script>"));
}
