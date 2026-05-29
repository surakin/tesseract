#include <catch2/catch_test_macros.hpp>
#include "tk/i18n.h"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace
{

static void write_u32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back(static_cast<uint8_t>(x));
    v.push_back(static_cast<uint8_t>(x >> 8));
    v.push_back(static_cast<uint8_t>(x >> 16));
    v.push_back(static_cast<uint8_t>(x >> 24));
}

std::vector<uint8_t> build_mo(
    const std::vector<std::pair<std::string, std::string>>& entries)
{
    uint32_t n = static_cast<uint32_t>(entries.size());

    std::vector<uint8_t> orig_strings, trans_strings;

    struct StrInfo
    {
        uint32_t len;
        uint32_t off;
    };

    std::vector<StrInfo> orig_info, trans_info;

    for (auto& [id, str] : entries)
    {
        orig_info.push_back({static_cast<uint32_t>(id.size()),
                             static_cast<uint32_t>(orig_strings.size())});
        for (char c : id)
        {
            orig_strings.push_back(static_cast<uint8_t>(c));
        }
        orig_strings.push_back(0);

        trans_info.push_back({static_cast<uint32_t>(str.size()),
                              static_cast<uint32_t>(trans_strings.size())});
        for (char c : str)
        {
            trans_strings.push_back(static_cast<uint8_t>(c));
        }
        trans_strings.push_back(0);
    }

    uint32_t header_size    = 28;
    uint32_t orig_table_off = header_size;
    uint32_t trans_table_off = header_size + n * 8;
    uint32_t orig_str_off   = header_size + n * 8 * 2;
    uint32_t trans_str_off  = orig_str_off + static_cast<uint32_t>(orig_strings.size());

    std::vector<uint8_t> mo;
    write_u32(mo, 0x950412deU);
    write_u32(mo, 0);
    write_u32(mo, n);
    write_u32(mo, orig_table_off);
    write_u32(mo, trans_table_off);
    write_u32(mo, 0);
    write_u32(mo, 0);

    for (auto& info : orig_info)
    {
        write_u32(mo, info.len);
        write_u32(mo, orig_str_off + info.off);
    }

    for (auto& info : trans_info)
    {
        write_u32(mo, info.len);
        write_u32(mo, trans_str_off + info.off);
    }

    mo.insert(mo.end(), orig_strings.begin(), orig_strings.end());
    mo.insert(mo.end(), trans_strings.begin(), trans_strings.end());

    return mo;
}

std::string write_tmp_mo(const std::vector<uint8_t>& mo, const std::string& filename)
{
    std::string path = "/tmp/" + filename;
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(mo.data()),
            static_cast<std::streamsize>(mo.size()));
    return path;
}

} // namespace

TEST_CASE("tk::tr returns source string when no catalog loaded", "[i18n]")
{
    tk::set_locale("/nonexistent/path", "en");
    CHECK(tk::tr("Hello") == "Hello");
    CHECK(tk::tr("Missing") == "Missing");
}

TEST_CASE("tk::tr returns translation from loaded MO catalog", "[i18n]")
{
    std::string meta = "Content-Type: text/plain; charset=UTF-8\n"
                       "Plural-Forms: nplurals=2; plural=(n != 1);\n";

    auto mo = build_mo({
        {"", meta},
        {"Hello", "Hola"},
    });
    write_tmp_mo(mo, "tesseract.test_es.mo");
    tk::set_locale("/tmp", "test_es");

    CHECK(tk::tr("Hello") == "Hola");
    CHECK(tk::tr("Missing") == "Missing");
}

TEST_CASE("tk::trf does positional substitution and supports reordering", "[i18n]")
{
    std::string meta = "Content-Type: text/plain; charset=UTF-8\n"
                       "Plural-Forms: nplurals=2; plural=(n != 1);\n";

    auto mo = build_mo({
        {"", meta},
        {"{0} {1}", "{1} de {0}"},
    });
    write_tmp_mo(mo, "tesseract.test_reorder.mo");
    tk::set_locale("/tmp", "test_reorder");

    CHECK(tk::trf(tk::tr("{0} {1}"), {"January", "5"}) == "5 de January");

    tk::set_locale("/nonexistent", "zz");
    CHECK(tk::trf("{0} and {1}", {"Alice", "Bob"}) == "Alice and Bob");
    CHECK(tk::trf("{1} before {0}", {"second", "first"}) == "first before second");
}

TEST_CASE("tk::trn selects plural form correctly", "[i18n]")
{
    std::string meta = "Content-Type: text/plain; charset=UTF-8\n"
                       "Plural-Forms: nplurals=2; plural=(n != 1);\n";

    std::string plural_msgid  = std::string("{0} item")     + '\0' + "{0} items";
    std::string plural_msgstr = std::string("{0} elemento") + '\0' + "{0} elementos";

    auto mo = build_mo({
        {"", meta},
        {plural_msgid, plural_msgstr},
    });
    write_tmp_mo(mo, "tesseract.test_plural.mo");
    tk::set_locale("/tmp", "test_plural");

    CHECK(tk::trf(tk::trn("{0} item", "{0} items", 1L), {std::to_string(1)}) == "1 elemento");
    CHECK(tk::trf(tk::trn("{0} item", "{0} items", 2L), {std::to_string(2)}) == "2 elementos");
    CHECK(tk::trf(tk::trn("{0} item", "{0} items", 0L), {std::to_string(0)}) == "0 elementos");
}

TEST_CASE("Plural-Forms evaluator: French-style rule (plural=(n>1))", "[i18n]")
{
    std::string meta = "Content-Type: text/plain; charset=UTF-8\n"
                       "Plural-Forms: nplurals=2; plural=(n>1);\n";

    std::string plural_msgid  = std::string("form")  + '\0' + "forms";
    std::string plural_msgstr = std::string("forme") + '\0' + "formes";

    auto mo = build_mo({
        {"", meta},
        {plural_msgid, plural_msgstr},
    });
    write_tmp_mo(mo, "tesseract.test_french.mo");
    tk::set_locale("/tmp", "test_french");

    CHECK(tk::trn("form", "forms", 0L) == "forme");
    CHECK(tk::trn("form", "forms", 1L) == "forme");
    CHECK(tk::trn("form", "forms", 2L) == "formes");
    CHECK(tk::trn("form", "forms", 5L) == "formes");
}

TEST_CASE("set_locale fallback: lang-REGION tries lang prefix", "[i18n]")
{
    std::string meta = "Content-Type: text/plain; charset=UTF-8\n"
                       "Plural-Forms: nplurals=2; plural=(n != 1);\n";

    auto mo = build_mo({
        {"", meta},
        {"Bye", "Adios"},
    });
    write_tmp_mo(mo, "tesseract.xx.mo");

    tk::set_locale("/tmp", "xx_YY");
    CHECK(tk::tr("Bye") == "Adios");
}
