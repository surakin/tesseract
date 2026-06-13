#include <catch2/catch_test_macros.hpp>
#include <tesseract/markdown.h>

#include <string>

using tesseract::markdown_inline_to_html;
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

TEST_CASE("markdown_inline: escapes plain text with no markers")
{
    // Unlike markdown_to_html, the inline form never returns "" and always
    // escapes — so it's safe to embed (e.g. in a spoiler span).
    CHECK(markdown_inline_to_html("a < b & c") == "a &lt; b &amp; c");
}

TEST_CASE("markdown_inline: renders inline emphasis without block wrapper")
{
    auto html = markdown_inline_to_html("*x*");
    CHECK(html == "<em>x</em>");
    CHECK_FALSE(contains(html, "<p>"));
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
    // pulldown-cmark (CommonMark) nests *** as <em><strong> (outermost first).
    CHECK(contains(markdown_to_html("***bi***").formatted_body,
                   "<em><strong>bi</strong></em>"));
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
    // pulldown-cmark wraps blockquote content in <p> per CommonMark spec.
    auto bq = markdown_to_html("> quoted").formatted_body;
    CHECK(contains(bq, "<blockquote>"));
    CHECK(contains(bq, "<p>quoted</p>"));
    CHECK(contains(bq, "</blockquote>"));
    // pulldown-cmark emits newlines around list items.
    auto ul = markdown_to_html("- one\n- two").formatted_body;
    CHECK(contains(ul, "<ul>"));
    CHECK(contains(ul, "<li>one</li>"));
    CHECK(contains(ul, "<li>two</li>"));
    CHECK(contains(ul, "</ul>"));
    auto ol = markdown_to_html("1. a\n2. b").formatted_body;
    CHECK(contains(ol, "<ol>"));
    CHECK(contains(ol, "<li>a</li>"));
    CHECK(contains(ol, "<li>b</li>"));
    CHECK(contains(ol, "</ol>"));
}

TEST_CASE("markdown: fenced code block escapes content")
{
    auto r = markdown_to_html("```\nlet x = a < b;\n```");
    CHECK(contains(r.formatted_body, "<pre><code>"));
    CHECK(contains(r.formatted_body, "let x = a &lt; b;"));
    CHECK(contains(r.formatted_body, "</code></pre>"));
}

TEST_CASE("markdown: fenced code block records the language as a class")
{
    auto r = markdown_to_html("```rust\nfn main() {}\n```");
    CHECK(contains(r.formatted_body, "<pre><code class=\"language-rust\">"));
}

TEST_CASE("markdown: fence language is lower-cased and first-token only")
{
    auto r = markdown_to_html("```Python title=foo\nx = 1\n```");
    CHECK(contains(r.formatted_body, "<pre><code class=\"language-python\">"));
    CHECK_FALSE(contains(r.formatted_body, "title"));
}

TEST_CASE("markdown: fence language cannot inject markup (sanitized class)")
{
    // A hostile info string must not break out of the class attribute.
    auto r = markdown_to_html("```js\"><img src=x>\nx\n```");
    CHECK(contains(r.formatted_body, "<pre><code class=\"language-js\">"));
    CHECK_FALSE(contains(r.formatted_body, "<img"));
}

TEST_CASE("markdown: a language-less fence still emits a bare code block")
{
    auto r = markdown_to_html("```\nplain\n```");
    CHECK(contains(r.formatted_body, "<pre><code>"));
    CHECK_FALSE(contains(r.formatted_body, "class=\"language-"));
}

TEST_CASE("markdown: U+2028 line separator is treated as newline (QTextEdit Shift+Enter)")
{
    // QTextEdit inserts U+2028 (UTF-8: E2 80 A8) for Shift+Enter. Without
    // normalisation the fence detector sees one long line and discards the code
    // content entirely.
    const char* ls = "\xe2\x80\xa8"; // U+2028 in UTF-8
    std::string input;
    input += "```python";
    input += ls;
    input += "x = 1";
    input += ls;
    input += "```";
    auto r = markdown_to_html(input);
    CHECK(contains(r.formatted_body, "<pre><code class=\"language-python\">"));
    CHECK(contains(r.formatted_body, "x = 1"));
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
