#include "views/EmoticonReplacer.h"
#include <tesseract/emoji.h>

#include <algorithm>
#include <cctype>

namespace tesseract::views
{

namespace
{

struct Emoticon
{
    std::string_view token;
    std::string_view glyph;
};

constexpr std::string_view kSmile = "\xF0\x9F\x99\x82";          // 🙂
constexpr std::string_view kFrown = "\xF0\x9F\x99\x81";          // 🙁
constexpr std::string_view kGrin = "\xF0\x9F\x98\x83";           // 😃
constexpr std::string_view kLaugh = "\xF0\x9F\x98\x86";          // 😆
constexpr std::string_view kWink = "\xF0\x9F\x98\x89";           // 😉
constexpr std::string_view kTongue = "\xF0\x9F\x98\x9B";         // 😛
constexpr std::string_view kWinkTongue = "\xF0\x9F\x98\x9C";     // 😜
constexpr std::string_view kOpenMouth = "\xF0\x9F\x98\xAE";      // 😮
constexpr std::string_view kNeutral = "\xF0\x9F\x98\x90";        // 😐
constexpr std::string_view kConfused = "\xF0\x9F\x98\x95";       // 😕
constexpr std::string_view kCry = "\xF0\x9F\x98\xA2";            // 😢
constexpr std::string_view kSmileTear = "\xF0\x9F\xA5\xB2";      // 🥲 (Emoji 13)
constexpr std::string_view kKiss = "\xF0\x9F\x98\x98";           // 😘
constexpr std::string_view kHeart = "\xE2\x9D\xA4\xEF\xB8\x8F";  // ❤️
constexpr std::string_view kBrokenHeart = "\xF0\x9F\x92\x94";    // 💔
constexpr std::string_view kCool = "\xF0\x9F\x98\x8E";           // 😎
constexpr std::string_view kAngry = "\xF0\x9F\x98\xA0";          // 😠
constexpr std::string_view kHalo = "\xF0\x9F\x98\x87";           // 😇
constexpr std::string_view kConfounded = "\xF0\x9F\x98\x96";     // 😖
constexpr std::string_view kFlushed = "\xF0\x9F\x98\xB3";        // 😳
constexpr std::string_view kDevil = "\xF0\x9F\x98\x88";          // 😈
constexpr std::string_view kCat = "\xF0\x9F\x98\xBA";            // 😺
constexpr std::string_view kAnguished = "\xF0\x9F\x98\xA7";      // 😧
constexpr std::string_view kExpressionless = "\xF0\x9F\x98\x91"; // 😑
constexpr std::string_view kHappy = "\xF0\x9F\x98\x8A";          // 😊

// Matched case-sensitively: ":P" and ":p" are both listed, but "xd" is not.
// "B)" and "8)" are deliberately absent — they collide with list markers
// ("option B) ...", "step 8) ...").
constexpr Emoticon kEmoticons[] = {
    {":)", kSmile},       {":-)", kSmile},       {"(:", kSmile},
    {":]", kSmile},       {"=)", kSmile},        {":(", kFrown},
    {":-(", kFrown},      {":[", kFrown},        {"=(", kFrown},
    {":D", kGrin},        {":-D", kGrin},        {"=D", kGrin},
    {"xD", kLaugh},       {"XD", kLaugh},        {";)", kWink},
    {";-)", kWink},       {":P", kTongue},       {":-P", kTongue},
    {":p", kTongue},      {":-p", kTongue},      {";P", kWinkTongue},
    {";p", kWinkTongue},  {":O", kOpenMouth},    {":-O", kOpenMouth},
    {":o", kOpenMouth},   {":-o", kOpenMouth},   {":|", kNeutral},
    {":-|", kNeutral},    {":/", kConfused},     {":-/", kConfused},
    {":\\", kConfused},   {":-\\", kConfused},   {":'(", kCry},
    {":')", kSmileTear},  {":*", kKiss},         {":-*", kKiss},
    {"<3", kHeart},       {"</3", kBrokenHeart}, {"B-)", kCool},
    {">:(", kAngry},      {">:-(", kAngry},      {"O:)", kHalo},
    {"O:-)", kHalo},      {"0:)", kHalo},        {":S", kConfounded},
    {":-S", kConfounded}, {":s", kConfounded},   {":$", kFlushed},
    {":-$", kFlushed},    {">:)", kDevil},       {">:-)", kDevil},
    {":3", kCat},         {"D:", kAnguished},    {"-_-", kExpressionless},
    {"^_^", kHappy},      {"^^", kHappy},        {"o_O", kFlushed},
    {"O_o", kFlushed},
};

bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// A token ending in one of these may still be growing into a :shortcode:,
// a word, or a path, so at end of text it waits for a delimiter.
bool may_continue(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '+' || c == '-' ||
           c == ':';
}

// True when byte offset `pos` lies inside a Markdown code span or fence:
// a backtick run opens code and only a run of the same length closes it
// (CommonMark), so ``` fences and `` spans are both covered.
bool inside_code(std::string_view text, int pos)
{
    int open_run = 0;
    int i = 0;
    while (i < pos)
    {
        if (text[i] == '\\' && open_run == 0)
        {
            i += 2; // escaped character outside code
            continue;
        }
        if (text[i] != '`')
        {
            ++i;
            continue;
        }
        int run = 0;
        while (i < pos && text[i] == '`')
        {
            ++run;
            ++i;
        }
        if (open_run == 0)
        {
            open_run = run;
        }
        else if (run == open_run)
        {
            open_run = 0;
        }
    }
    return open_run != 0;
}

} // namespace

std::optional<EmoticonMatch> find_emoticon(std::string_view text, int cursor,
                                           std::uint16_t max_emoji_version)
{
    cursor = std::clamp(cursor, 0, static_cast<int>(text.size()));
    int token_end = cursor;
    bool delimited = false;
    if (token_end > 0 && (text[token_end - 1] == ' ' || text[token_end - 1] == '\t'))
    {
        --token_end;
        delimited = true;
    }
    else if (cursor < static_cast<int>(text.size()))
    {
        return std::nullopt;
    }

    int token_start = token_end;
    while (token_start > 0 && !is_space(text[token_start - 1]))
    {
        --token_start;
    }
    if (token_start == token_end)
    {
        return std::nullopt;
    }
    const std::string_view token = text.substr(token_start, token_end - token_start);
    if (!delimited && may_continue(token.back()))
    {
        return std::nullopt;
    }

    const auto* hit = std::find_if(std::begin(kEmoticons), std::end(kEmoticons),
                                   [token](const Emoticon& e)
                                   {
                                       return e.token == token;
                                   });
    if (hit == std::end(kEmoticons))
    {
        return std::nullopt;
    }
    if (tesseract::emoji::emoji_version(hit->glyph) > max_emoji_version)
    {
        return std::nullopt;
    }
    if (inside_code(text, token_start))
    {
        return std::nullopt;
    }
    return EmoticonMatch{token_start, token_end, hit->glyph};
}

} // namespace tesseract::views
