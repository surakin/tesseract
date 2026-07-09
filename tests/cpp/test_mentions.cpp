#include <catch2/catch_test_macros.hpp>
#include <tesseract/mentions.h>

#include <string>
#include <vector>

using tesseract::build_mention_message;
using tesseract::MentionSeg;

namespace
{
bool contains_mentions(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}

MentionSeg text(std::string s)
{
    MentionSeg m;
    m.kind = MentionSeg::Kind::Text;
    m.text = std::move(s);
    return m;
}

MentionSeg mention(std::string user_id, std::string display)
{
    MentionSeg m;
    m.kind = MentionSeg::Kind::Mention;
    m.user_id = std::move(user_id);
    m.display_name = std::move(display);
    return m;
}

MentionSeg room_mention()
{
    MentionSeg m;
    m.kind = MentionSeg::Kind::Mention;
    m.is_room = true;
    return m;
}

MentionSeg emoticon(std::string shortcode, std::string mxc_url)
{
    MentionSeg m;
    m.kind = MentionSeg::Kind::Emoticon;
    m.shortcode = std::move(shortcode);
    m.mxc_url = std::move(mxc_url);
    return m;
}
} // namespace

TEST_CASE("mentions: no mention segments behaves like markdown_to_html")
{
    std::vector<MentionSeg> segs = {text("just **bold** text")};
    auto r = build_mention_message(segs);
    CHECK(r.body == "just **bold** text");
    CHECK(contains_mentions(r.formatted_body, "<strong>bold</strong>"));
}

TEST_CASE("mentions: plain text with no markdown yields empty formatted_body")
{
    std::vector<MentionSeg> segs = {text("hello there")};
    auto r = build_mention_message(segs);
    CHECK(r.body == "hello there");
    CHECK(r.formatted_body.empty());
}

TEST_CASE("mentions: single user mention without markdown still formats")
{
    std::vector<MentionSeg> segs = {
        text("hi "), mention("@alice:example.org", "Alice"), text("!")};
    auto r = build_mention_message(segs);
    CHECK(r.body == "hi Alice!");
    // formatted_body is forced (mentions need anchors for m.mentions derivation)
    CHECK(contains_mentions(r.formatted_body,
                   "<a href=\"https://matrix.to/#/@alice:example.org\">Alice</a>"));
    CHECK(contains_mentions(r.formatted_body, "hi "));
}

TEST_CASE("mentions: markdown around a mention is preserved")
{
    std::vector<MentionSeg> segs = {
        text("**hey** "), mention("@bob:example.org", "Bob")};
    auto r = build_mention_message(segs);
    CHECK(contains_mentions(r.formatted_body, "<strong>hey</strong>"));
    CHECK(contains_mentions(r.formatted_body,
                   "<a href=\"https://matrix.to/#/@bob:example.org\">Bob</a>"));
    // The placeholder sentinel must not leak into output.
    CHECK_FALSE(contains_mentions(r.formatted_body, "\xEE\x80\x80"));
}

TEST_CASE("mentions: @room emits the sentinel anchor and plain body")
{
    std::vector<MentionSeg> segs = {room_mention(), text(" ship it")};
    auto r = build_mention_message(segs);
    CHECK(r.body == "@room ship it");
    CHECK(contains_mentions(r.formatted_body,
                   "<a href=\"https://matrix.to/#/@room\">@room</a>"));
}

TEST_CASE("mentions: multiple mentions get distinct anchors")
{
    std::vector<MentionSeg> segs = {mention("@a:x.org", "A"), text(" "),
                                    mention("@b:x.org", "B")};
    auto r = build_mention_message(segs);
    CHECK(r.body == "A B");
    CHECK(contains_mentions(r.formatted_body, "https://matrix.to/#/@a:x.org"));
    CHECK(contains_mentions(r.formatted_body, "https://matrix.to/#/@b:x.org"));
}

TEST_CASE("mentions: display names are HTML-escaped")
{
    std::vector<MentionSeg> segs = {mention("@a:x.org", "<b>&you</b>")};
    auto r = build_mention_message(segs);
    CHECK(contains_mentions(r.formatted_body, "&lt;b&gt;&amp;you&lt;/b&gt;"));
    CHECK_FALSE(contains_mentions(r.formatted_body, "<b>&you"));
}

TEST_CASE("mentions: a lone emoticon segment round-trips to literal "
          "shortcode text and an MSC2545 img tag")
{
    std::vector<MentionSeg> segs = {text("wave "),
                                    emoticon("wave", "mxc://x.org/abc")};
    auto r = build_mention_message(segs);
    CHECK(r.body == "wave :wave:");
    CHECK(contains_mentions(r.formatted_body, "<img data-mx-emoticon src=\"mxc://x.org/abc\""));
    CHECK(contains_mentions(r.formatted_body, "alt=\":wave:\""));
    CHECK(contains_mentions(r.formatted_body, "title=\":wave:\""));
    // The placeholder sentinel must not leak into output.
    CHECK_FALSE(contains_mentions(r.formatted_body, "\xEE\x80\x80"));
}

TEST_CASE("mentions: plain emoticon-only message still forces a non-empty "
          "formatted_body")
{
    std::vector<MentionSeg> segs = {emoticon("smile", "mxc://x.org/def")};
    auto r = build_mention_message(segs);
    CHECK(r.body == ":smile:");
    CHECK_FALSE(r.formatted_body.empty());
    CHECK(contains_mentions(r.formatted_body, "mxc://x.org/def"));
}

TEST_CASE("mentions: mixed mention + emoticon + text produce correct body "
          "and formatted_body together")
{
    std::vector<MentionSeg> segs = {
        mention("@a:x.org", "Alice"), text(" say "),
        emoticon("wave", "mxc://x.org/abc"), text(" back")};
    auto r = build_mention_message(segs);
    CHECK(r.body == "Alice say :wave: back");
    CHECK(contains_mentions(r.formatted_body,
                   "<a href=\"https://matrix.to/#/@a:x.org\">Alice</a>"));
    CHECK(contains_mentions(r.formatted_body, "<img data-mx-emoticon src=\"mxc://x.org/abc\""));
    CHECK(contains_mentions(r.formatted_body, " say "));
    CHECK(contains_mentions(r.formatted_body, " back"));
}

TEST_CASE("mentions: emoticon mxc url is HTML-escaped")
{
    std::vector<MentionSeg> segs = {
        emoticon("x", "mxc://x.org/\"onload=alert(1)")};
    auto r = build_mention_message(segs);
    CHECK_FALSE(contains_mentions(r.formatted_body, "\"onload=alert(1)\""));
}

TEST_CASE("mentions: emoticon shortcode is HTML-escaped in both alt and "
          "title (a shortcode is an attacker-controlled pack JSON key, not "
          "a fixed vocabulary)")
{
    std::vector<MentionSeg> segs = {
        emoticon("x\" onerror=\"alert(1)", "mxc://x.org/abc")};
    auto r = build_mention_message(segs);
    CHECK_FALSE(contains_mentions(r.formatted_body, "onerror=\"alert(1)\""));
    CHECK(contains_mentions(r.formatted_body, "&quot;"));
}
