#include "views/WorldConsentDialog.h"

#include "tk/i18n.h"
#include "views/ConfirmDialog.h"

#include <cstdint>
#include <memory>

namespace tesseract::views
{

namespace
{

// Decodes one UTF-8 sequence at `i`; returns the code point (0xFFFD when invalid) and advances.
char32_t next_code_point(std::string_view s, std::size_t& i)
{
    const auto b0 = static_cast<unsigned char>(s[i]);
    if (b0 < 0x80)
    {
        ++i;
        return b0;
    }
    int len = 0;
    char32_t cp = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF)
    {
        len = 2;
        cp = b0 & 0x1F;
    }
    else if (b0 >= 0xE0 && b0 <= 0xEF)
    {
        len = 3;
        cp = b0 & 0x0F;
    }
    else if (b0 >= 0xF0 && b0 <= 0xF4)
    {
        len = 4;
        cp = b0 & 0x07;
    }
    else
    {
        ++i;
        return 0xFFFD;
    }
    if (i + static_cast<std::size_t>(len) > s.size())
    {
        ++i;
        return 0xFFFD;
    }
    for (int k = 1; k < len; ++k)
    {
        const auto b = static_cast<unsigned char>(s[i + static_cast<std::size_t>(k)]);
        if ((b & 0xC0) != 0x80)
        {
            ++i;
            return 0xFFFD;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    const bool overlong = (len == 3 && cp < 0x800) || (len == 4 && cp < 0x10000);
    if (overlong || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
    {
        ++i;
        return 0xFFFD;
    }
    i += static_cast<std::size_t>(len);
    return cp;
}

bool is_stripped(char32_t cp)
{
    return cp == 0xFFA0 || cp == 0x180E || cp == 0x2800 || (cp >= 0x1D173 && cp <= 0x1D17A) ||
           cp < 0x20 || (cp >= 0x7F && cp <= 0x9F) || cp == 0x00AD || cp == 0x034F ||
           cp == 0x115F || cp == 0x1160 || cp == 0x3164 || (cp >= 0xFFF9 && cp <= 0xFFFB) ||
           (cp >= 0xE0000 && cp <= 0xE007F) || cp == 0x061C || (cp >= 0x200B && cp <= 0x200F) ||
           (cp >= 0x202A && cp <= 0x202E) || cp == 0x2028 || cp == 0x2029 ||
           (cp >= 0x2060 && cp <= 0x2064) || (cp >= 0x2066 && cp <= 0x206F) || cp == 0xFEFF;
}

bool is_combining(char32_t cp)
{
    return (cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
           (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
           (cp >= 0xFE20 && cp <= 0xFE2F);
}

constexpr std::size_t kMaxCombiningRun = 3;

void append_utf8(std::string& out, char32_t cp)
{
    if (cp < 0x80)
    {
        out += static_cast<char>(cp);
    }
    else if (cp < 0x800)
    {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else if (cp < 0x10000)
    {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

} // namespace

std::string sanitize_world_text(std::string_view text, std::size_t max_chars)
{
    std::string out;
    std::size_t count = 0;
    std::size_t combining_run = 0;
    std::size_t i = 0;
    while (i < text.size())
    {
        const char32_t cp = next_code_point(text, i);
        if (is_stripped(cp))
        {
            continue;
        }
        combining_run = is_combining(cp) ? combining_run + 1 : 0;
        if (combining_run > kMaxCombiningRun)
        {
            continue; // a stack of marks that would paint over the lines around it
        }
        if (count == max_chars)
        {
            append_utf8(out, 0x2026);
            return out;
        }
        append_utf8(out, cp);
        ++count;
    }
    return out;
}

void open_world_consent_dialog(ConfirmDialog& dialog, const WorldConsentRequest& request,
                               std::function<void(ConsentChoice)> answer)
{
    // One answer only: Run once and Always run go through their own callbacks, everything else
    // (Cancel, backdrop, Escape, a replacing dialog) is on_dismiss.
    auto once = std::make_shared<std::function<void(ConsentChoice)>>(std::move(answer));
    auto reply = [once](ConsentChoice c)
    {
        if (auto fn = std::move(*once))
        {
            *once = nullptr;
            fn(c);
        }
    };

    const std::string name = sanitize_world_text(request.name);
    const std::string sender = sanitize_world_text(request.sender, kMaxSenderChars);
    const std::string hash = sanitize_world_text(request.hash.substr(0, 16), 16);

    ConfirmDialog::Options opts;
    opts.title = tk::tr("Run this room's world?");
    opts.body =
        tk::trf(tk::tr("{0} published the world \"{1}\" (id {2}) for this room. It contains "
                       "scripts that will run on your computer. Run it only if you trust "
                       "{0}."),
                {sender, name, hash});
    opts.confirm_label = tk::tr("Run once");
    opts.secondary_label = tk::tr("Always run this world");
    opts.cancel_label = tk::tr("Don't run");
    opts.on_secondary = [reply]
    {
        reply(ConsentChoice::AlwaysRun);
    };
    opts.on_dismiss = [reply]
    {
        reply(ConsentChoice::DontRun);
    };
    dialog.open(std::move(opts),
                [reply]
                {
                    reply(ConsentChoice::RunOnce);
                });
}

} // namespace tesseract::views
