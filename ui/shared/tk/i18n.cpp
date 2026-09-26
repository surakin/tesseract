#include "i18n.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace tk
{

namespace
{

// ---------------------------------------------------------------------------
// MO binary reader
// ---------------------------------------------------------------------------

const uint32_t kMoMagicLE = 0x950412deU;
const uint32_t kMoMagicBE = 0xde120495U;

// Documents the MO file header layout. Parsing uses read_u32() calls on raw
// offsets rather than casting the buffer to this struct (avoids alignment UB).
struct MoHeader
{
    uint32_t magic;
    uint32_t revision;
    uint32_t num_strings;
    uint32_t orig_offset;
    uint32_t trans_offset;
    uint32_t hash_size;
    uint32_t hash_offset;
};

// Read a 4-byte little-endian or big-endian uint32 from a byte buffer.
uint32_t read_u32(const uint8_t* p, bool swap)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    if (swap)
    {
        v = ((v & 0x000000FFU) << 24) |
            ((v & 0x0000FF00U) << 8)  |
            ((v & 0x00FF0000U) >> 8)  |
            ((v & 0xFF000000U) >> 24);
    }
    return v;
}

// ---------------------------------------------------------------------------
// Plural-Forms expression evaluator (recursive descent)
// ---------------------------------------------------------------------------
//
// Grammar (C-like ternary expression):
//   expr     := ternary
//   ternary  := or ('?' or ':' ternary)?
//   or       := and ('||' and)*
//   and      := equality ('&&' equality)*
//   equality := relational (('=='|'!=') relational)*
//   relational := additive (('<'|'>'|'<='|'>=') additive)*
//   additive := unary (('+'|'-') unary)*
//   unary    := ('!')? multiplicative
//   multiplicative := primary (('*'|'/'|'%') primary)*
//   primary  := '(' expr ')' | integer | 'n'

// The 'expr' string must outlive this PluralExpr. Only safe to construct
// from a long-lived catalog member (g_catalog.plural_expr_).
struct PluralExpr
{
    const char* src_;   // original expression string
    const char* pos_;   // current parse position
    long n_;            // value of 'n' during evaluation

    explicit PluralExpr(const std::string& expr, long n)
        : src_(expr.c_str()), pos_(expr.c_str()), n_(n)
    {
    }

    void skip_ws()
    {
        while (*pos_ == ' ' || *pos_ == '\t')
        {
            ++pos_;
        }
    }

    long parse_primary()
    {
        skip_ws();
        if (*pos_ == '(')
        {
            ++pos_;
            long val = parse_ternary();
            skip_ws();
            if (*pos_ == ')')
            {
                ++pos_;
            }
            return val;
        }
        if (*pos_ == 'n')
        {
            ++pos_;
            return n_;
        }
        // Integer literal
        long val = 0;
        bool got_digit = false;
        while (*pos_ >= '0' && *pos_ <= '9')
        {
            val = val * 10 + (*pos_ - '0');
            ++pos_;
            got_digit = true;
        }
        if (!got_digit)
        {
            ++pos_; // skip unknown character
        }
        return val;
    }

    long parse_unary()
    {
        skip_ws();
        if (*pos_ == '!')
        {
            ++pos_;
            return parse_unary() == 0 ? 1 : 0;
        }
        return parse_multiplicative();
    }

    long parse_multiplicative()
    {
        long val = parse_primary();
        for (;;)
        {
            skip_ws();
            if (*pos_ == '*')
            {
                ++pos_;
                val *= parse_primary();
            }
            else if (*pos_ == '/')
            {
                ++pos_;
                long rhs = parse_primary();
                val = (rhs != 0) ? (val / rhs) : 0;
            }
            else if (*pos_ == '%')
            {
                ++pos_;
                long rhs = parse_primary();
                val = (rhs != 0) ? (val % rhs) : 0;
            }
            else
            {
                break;
            }
        }
        return val;
    }

    long parse_additive()
    {
        long val = parse_unary();
        for (;;)
        {
            skip_ws();
            if (*pos_ == '+')
            {
                ++pos_;
                val += parse_unary();
            }
            else if (*pos_ == '-')
            {
                ++pos_;
                val -= parse_unary();
            }
            else
            {
                break;
            }
        }
        return val;
    }

    long parse_relational()
    {
        long val = parse_additive();
        for (;;)
        {
            skip_ws();
            if (pos_[0] == '<' && pos_[1] == '=')
            {
                pos_ += 2;
                val = (val <= parse_additive()) ? 1 : 0;
            }
            else if (pos_[0] == '>' && pos_[1] == '=')
            {
                pos_ += 2;
                val = (val >= parse_additive()) ? 1 : 0;
            }
            else if (pos_[0] == '<')
            {
                ++pos_;
                val = (val < parse_additive()) ? 1 : 0;
            }
            else if (pos_[0] == '>')
            {
                ++pos_;
                val = (val > parse_additive()) ? 1 : 0;
            }
            else
            {
                break;
            }
        }
        return val;
    }

    long parse_equality()
    {
        long val = parse_relational();
        for (;;)
        {
            skip_ws();
            if (pos_[0] == '=' && pos_[1] == '=')
            {
                pos_ += 2;
                val = (val == parse_relational()) ? 1 : 0;
            }
            else if (pos_[0] == '!' && pos_[1] == '=')
            {
                pos_ += 2;
                val = (val != parse_relational()) ? 1 : 0;
            }
            else
            {
                break;
            }
        }
        return val;
    }

    long parse_and()
    {
        long val = parse_equality();
        for (;;)
        {
            skip_ws();
            if (pos_[0] == '&' && pos_[1] == '&')
            {
                pos_ += 2;
                long rhs = parse_equality();
                val = (val != 0 && rhs != 0) ? 1 : 0;
            }
            else
            {
                break;
            }
        }
        return val;
    }

    long parse_or()
    {
        long val = parse_and();
        for (;;)
        {
            skip_ws();
            if (pos_[0] == '|' && pos_[1] == '|')
            {
                pos_ += 2;
                long rhs = parse_and();
                val = (val != 0 || rhs != 0) ? 1 : 0;
            }
            else
            {
                break;
            }
        }
        return val;
    }

    long parse_ternary()
    {
        long cond = parse_or();
        skip_ws();
        if (*pos_ == '?')
        {
            ++pos_;
            // Both branches are always evaluated — intentional, since Plural-Forms
            // expressions are pure integer arithmetic with no side effects.
            long if_true = parse_ternary();
            skip_ws();
            if (*pos_ == ':')
            {
                ++pos_;
            }
            long if_false = parse_ternary();
            return (cond != 0) ? if_true : if_false;
        }
        return cond;
    }

    long evaluate()
    {
        return parse_ternary();
    }
};

long eval_plural(const std::string& expr, long n)
{
    PluralExpr evaluator(expr, n);
    return evaluator.evaluate();
}

// ---------------------------------------------------------------------------
// Catalog
// ---------------------------------------------------------------------------

struct Catalog
{
    bool identity_mode_ = true; // true = no MO file loaded, pass through
    std::string locale_;

    // Singular lookup: msgid -> msgstr (first form for singular-only entries)
    std::unordered_map<std::string, std::string> singular_;

    // Plural lookup: sing msgid -> all plural forms
    std::unordered_map<std::string, std::vector<std::string>> plural_;

    long nplurals_ = 2;
    std::string plural_expr_ = "(n != 1)"; // default English rule

    long pick_plural_form(long n) const
    {
        long form = eval_plural(plural_expr_, n);
        if (form < 0)
        {
            form = 0;
        }
        if (form >= nplurals_)
        {
            form = nplurals_ - 1;
        }
        return form;
    }
};

// Global catalog — written once by set_locale(), read-only thereafter.
Catalog g_catalog;

// ---------------------------------------------------------------------------
// Parse Plural-Forms header
// ---------------------------------------------------------------------------

void parse_plural_forms(Catalog& cat, const std::string& metadata)
{
    // Find "Plural-Forms:" line
    std::istringstream ss(metadata);
    std::string line;
    while (std::getline(ss, line))
    {
        const char* prefix = "Plural-Forms:";
        auto pos = line.find(prefix);
        if (pos == std::string::npos)
        {
            continue;
        }
        std::string value = line.substr(pos + std::strlen(prefix));

        // Extract nplurals=N
        auto np_pos = value.find("nplurals=");
        if (np_pos != std::string::npos)
        {
            const char* p = value.c_str() + np_pos + 9; // skip "nplurals="
            long v = 0;
            while (*p >= '0' && *p <= '9')
            {
                v = v * 10 + (*p - '0');
                ++p;
            }
            if (v > 0)
            {
                cat.nplurals_ = v;
            }
        }

        // Extract plural=<expr>
        // find("plural=") never matches inside "nplurals=" because "nplurals="
        // contains 's' before '=', so the substring "plural=" does not match there.
        auto pe_pos = value.find("plural=");
        if (pe_pos != std::string::npos)
        {
            std::string expr = value.substr(pe_pos + 7); // skip "plural="
            // Trim trailing whitespace, semicolons, newlines
            while (!expr.empty() &&
                   (expr.back() == ';' || expr.back() == '\n' ||
                    expr.back() == '\r' || expr.back() == ' '))
            {
                expr.pop_back();
            }
            if (!expr.empty())
            {
                cat.plural_expr_ = expr;
            }
        }
        return;
    }
}

// ---------------------------------------------------------------------------
// MO file loader
// ---------------------------------------------------------------------------

bool load_mo(Catalog& cat, const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f.is_open())
    {
        return false;
    }
    auto size = static_cast<std::size_t>(f.tellg());
    if (size < sizeof(MoHeader))
    {
        return false;
    }
    f.seekg(0);
    std::vector<uint8_t> data(size);
    if (!f.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size)))
    {
        return false;
    }

    const uint8_t* buf = data.data();

    // Determine byte order
    uint32_t magic;
    std::memcpy(&magic, buf, 4);
    if (magic != kMoMagicLE && magic != kMoMagicBE)
    {
        return false;
    }
    bool swap = (magic == kMoMagicBE);

    if (size < 7 * 4)
    {
        return false;
    }
    uint32_t num_strings  = read_u32(buf + 8,  swap);
    uint32_t orig_offset  = read_u32(buf + 12, swap);
    uint32_t trans_offset = read_u32(buf + 16, swap);

    // Sanity: each table entry is 8 bytes (length + offset)
    if ((uint64_t)orig_offset + (uint64_t)num_strings * 8 > size ||
        (uint64_t)trans_offset + (uint64_t)num_strings * 8 > size)
    {
        return false;
    }

    for (uint32_t i = 0; i < num_strings; ++i)
    {
        uint32_t orig_len  = read_u32(buf + orig_offset  + i * 8,     swap);
        uint32_t orig_off  = read_u32(buf + orig_offset  + i * 8 + 4, swap);
        uint32_t trans_len = read_u32(buf + trans_offset + i * 8,     swap);
        uint32_t trans_off = read_u32(buf + trans_offset + i * 8 + 4, swap);

        if ((uint64_t)orig_off + orig_len > size || (uint64_t)trans_off + trans_len > size)
        {
            continue;
        }

        const char* orig_str  = reinterpret_cast<const char*>(buf + orig_off);
        const char* trans_str = reinterpret_cast<const char*>(buf + trans_off);

        if (orig_len == 0)
        {
            // Index 0: empty-string metadata entry
            std::string metadata(trans_str, trans_len);
            parse_plural_forms(cat, metadata);
            continue;
        }

        // Check for plural entry: msgid contains a NUL separator
        // (sing\0plural form)
        const char* nul = static_cast<const char*>(
            std::memchr(orig_str, '\0', orig_len));
        if (nul != nullptr)
        {
            // Plural entry
            std::string sing(orig_str, static_cast<std::size_t>(nul - orig_str));

            std::vector<std::string> forms;
            const char* fp = trans_str;
            std::size_t remaining = trans_len;
            while (remaining > 0)
            {
                std::size_t form_len = std::strlen(fp);
                forms.emplace_back(fp, form_len);
                if (form_len + 1 > remaining)
                {
                    break;
                }
                fp        += form_len + 1;
                remaining -= form_len + 1;
            }
            if (!forms.empty())
            {
                cat.plural_[sing] = std::move(forms);
                // Also populate singular map with the first form for tr() lookups
                cat.singular_[sing] = cat.plural_[sing][0];
            }
        }
        else
        {
            // Singular entry
            std::string key(orig_str, orig_len);
            std::string val(trans_str, trans_len);
            cat.singular_[key] = std::move(val);
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Fallback path resolution
// ---------------------------------------------------------------------------

bool try_load(Catalog& cat, const std::string& dir, const std::string& lang)
{
    std::string path = dir + "/tesseract." + lang + ".mo";
    return load_mo(cat, path);
}

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

void set_locale(const std::string& i18n_dir, const std::string& lang)
{
    Catalog cat;
    cat.locale_ = lang;

    // Step 1: try full tag (e.g. "es_MX")
    if (!try_load(cat, i18n_dir, lang))
    {
        // Step 2: try language prefix (e.g. "es")
        auto sep = lang.find_first_of("_-");
        if (sep != std::string::npos)
        {
            std::string prefix = lang.substr(0, sep);
            if (!try_load(cat, i18n_dir, prefix))
            {
                cat.identity_mode_ = true;
            }
            else
            {
                cat.identity_mode_ = false;
            }
        }
        else
        {
            cat.identity_mode_ = true;
        }
    }
    else
    {
        cat.identity_mode_ = false;
    }

    g_catalog = std::move(cat);
}

const std::string& current_locale()
{
    return g_catalog.locale_;
}

std::string tr(const char* source)
{
    if (g_catalog.identity_mode_)
    {
        return source;
    }
    auto it = g_catalog.singular_.find(source);
    if (it != g_catalog.singular_.end())
    {
        return it->second;
    }
    return source;
}

std::string trn(const char* sing, const char* plur, long n)
{
    if (g_catalog.identity_mode_)
    {
        return (n == 1) ? sing : plur;
    }

    auto it = g_catalog.plural_.find(sing);
    if (it != g_catalog.plural_.end())
    {
        const auto& forms = it->second;
        if (!forms.empty())
        {
            long form = g_catalog.pick_plural_form(n);
            if (forms.empty())
                return sing;
            if (static_cast<std::size_t>(form) >= forms.size())
                form = static_cast<long>(forms.size()) - 1;
            return forms[static_cast<std::size_t>(form)];
        }
    }

    // Fallback to English logic
    return (n == 1) ? sing : plur;
}

namespace
{

constexpr const char* kMonthNames[12] = {
    N_("January"), N_("February"), N_("March"),     N_("April"),
    N_("May"),     N_("June"),     N_("July"),      N_("August"),
    N_("September"), N_("October"), N_("November"), N_("December")};
constexpr const char* kMonthAbbrs[12] = {
    N_("Jan"), N_("Feb"), N_("Mar"), N_("Apr"), N_("May"), N_("Jun"),
    N_("Jul"), N_("Aug"), N_("Sep"), N_("Oct"), N_("Nov"), N_("Dec")};
constexpr const char* kWeekdayNames[7] = {
    N_("Sunday"), N_("Monday"), N_("Tuesday"), N_("Wednesday"),
    N_("Thursday"), N_("Friday"), N_("Saturday")};
constexpr const char* kWeekdayAbbrs[7] = {
    N_("Sun"), N_("Mon"), N_("Tue"), N_("Wed"), N_("Thu"), N_("Fri"), N_("Sat")};
constexpr const char* kWeekdayInitials[7] = {
    N_("Su"), N_("Mo"), N_("Tu"), N_("We"), N_("Th"), N_("Fr"), N_("Sa")};

int clamp_index(int i, int n)
{
    return (i >= 0 && i < n) ? i : 0;
}

std::string two_digits(int v)
{
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d", v);
    return buf;
}

} // namespace

std::string format_date(const std::tm& tm, std::string_view pattern)
{
    std::string out;
    out.reserve(pattern.size() + 16);
    for (std::size_t i = 0; i < pattern.size(); ++i)
    {
        if (pattern[i] != '%' || i + 1 >= pattern.size())
        {
            out += pattern[i];
            continue;
        }
        bool no_pad = false;
        std::size_t j = i + 1;
        if (pattern[j] == '-' && j + 1 < pattern.size())
        {
            no_pad = true;
            ++j;
        }
        switch (pattern[j])
        {
        case 'A': out += tr(kWeekdayNames[clamp_index(tm.tm_wday, 7)]); break;
        case 'a': out += tr(kWeekdayAbbrs[clamp_index(tm.tm_wday, 7)]); break;
        case 'B': out += tr(kMonthNames[clamp_index(tm.tm_mon, 12)]); break;
        case 'b': out += tr(kMonthAbbrs[clamp_index(tm.tm_mon, 12)]); break;
        case 'd':
            out += no_pad ? std::to_string(tm.tm_mday) : two_digits(tm.tm_mday);
            break;
        case 'm':
            out += no_pad ? std::to_string(tm.tm_mon + 1) : two_digits(tm.tm_mon + 1);
            break;
        case 'Y': out += std::to_string(tm.tm_year + 1900); break;
        case 'y': out += two_digits((tm.tm_year + 1900) % 100); break;
        case '%': out += '%'; break;
        default:
            // Unknown directive: copy it through untouched.
            out.append(pattern.substr(i, j - i + 1));
            break;
        }
        i = j;
    }
    return out;
}

std::string weekday_initials(int wday)
{
    return tr(kWeekdayInitials[clamp_index(wday, 7)]);
}

std::string format_size(std::uint64_t bytes)
{
    if (bytes < 1024)
        return trf(tr("{0} B"), {std::to_string(bytes)});
    if (bytes < 1024 * 1024)
        return trf(tr("{0} KB"), {std::to_string(bytes / 1024)});
    if (bytes < 1024ull * 1024 * 1024)
        return trf(tr("{0} MB"), {std::to_string(bytes / (1024 * 1024))});
    return trf(tr("{0} GB"), {std::to_string(bytes / (1024ull * 1024 * 1024))});
}

std::string trf(const std::string& fmt, std::initializer_list<std::string> args)
{
    std::string result;
    result.reserve(fmt.size());

    const std::vector<std::string> argv(args);

    for (std::size_t i = 0; i < fmt.size(); ++i)
    {
        if (fmt[i] == '{')
        {
            // Look for closing '}'
            std::size_t j = i + 1;
            while (j < fmt.size() && fmt[j] != '}')
            {
                ++j;
            }
            if (j < fmt.size())
            {
                // Extract index string between braces
                std::string idx_str = fmt.substr(i + 1, j - i - 1);
                // Verify it is all digits
                bool all_digits = !idx_str.empty();
                for (char c : idx_str)
                {
                    if (c < '0' || c > '9')
                    {
                        all_digits = false;
                        break;
                    }
                }
                if (all_digits)
                {
                    std::size_t idx = static_cast<std::size_t>(std::stoul(idx_str));
                    if (idx < argv.size())
                    {
                        result += argv[idx];
                    }
                    // If index out of range, substitute nothing (drop the placeholder)
                    i = j; // skip past '}'
                    continue;
                }
            }
        }
        result += fmt[i];
    }

    return result;
}

} // namespace tk
