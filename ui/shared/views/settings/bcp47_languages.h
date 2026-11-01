#pragma once

// Static table of common BCP-47/ISO 639-1 language codes + English display
// names, backing LanguagePicker's searchable dropdown for MSC4247 pronoun
// language tags. Names are msgids: marked with tk::N_ for extraction and
// translated with tk::tr where they are shown (bcp47_language_name(),
// LanguagePicker).
//
// Not an exhaustive BCP-47/ISO 639-1 enumeration — a practical, commonly-used
// subset. Extend the table below if a language is missing.

#include "tk/i18n.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace tesseract::views
{

struct Bcp47Language
{
    std::string_view code;
    std::string_view name;
};

inline constexpr Bcp47Language kBcp47Languages[] = {
    {"aa", tk::N_("Afar")},
    {"ab", tk::N_("Abkhazian")},
    {"af", tk::N_("Afrikaans")},
    {"ak", tk::N_("Akan")},
    {"sq", tk::N_("Albanian")},
    {"am", tk::N_("Amharic")},
    {"ar", tk::N_("Arabic")},
    {"an", tk::N_("Aragonese")},
    {"hy", tk::N_("Armenian")},
    {"as", tk::N_("Assamese")},
    {"av", tk::N_("Avaric")},
    {"ae", tk::N_("Avestan")},
    {"ay", tk::N_("Aymara")},
    {"az", tk::N_("Azerbaijani")},
    {"bm", tk::N_("Bambara")},
    {"ba", tk::N_("Bashkir")},
    {"eu", tk::N_("Basque")},
    {"be", tk::N_("Belarusian")},
    {"bn", tk::N_("Bengali")},
    {"bi", tk::N_("Bislama")},
    {"bs", tk::N_("Bosnian")},
    {"br", tk::N_("Breton")},
    {"bg", tk::N_("Bulgarian")},
    {"my", tk::N_("Burmese")},
    {"ca", tk::N_("Catalan")},
    {"ch", tk::N_("Chamorro")},
    {"ce", tk::N_("Chechen")},
    {"ny", tk::N_("Chichewa")},
    {"zh", tk::N_("Chinese")},
    {"zh-Hans", tk::N_("Chinese (Simplified)")},
    {"zh-Hant", tk::N_("Chinese (Traditional)")},
    {"cu", tk::N_("Church Slavic")},
    {"cv", tk::N_("Chuvash")},
    {"kw", tk::N_("Cornish")},
    {"co", tk::N_("Corsican")},
    {"cr", tk::N_("Cree")},
    {"hr", tk::N_("Croatian")},
    {"cs", tk::N_("Czech")},
    {"da", tk::N_("Danish")},
    {"dv", tk::N_("Divehi")},
    {"nl", tk::N_("Dutch")},
    {"dz", tk::N_("Dzongkha")},
    {"en", tk::N_("English")},
    {"en-GB", tk::N_("English (UK)")},
    {"en-US", tk::N_("English (US)")},
    {"eo", tk::N_("Esperanto")},
    {"et", tk::N_("Estonian")},
    {"ee", tk::N_("Ewe")},
    {"fo", tk::N_("Faroese")},
    {"fj", tk::N_("Fijian")},
    {"fi", tk::N_("Finnish")},
    {"fr", tk::N_("French")},
    {"fy", tk::N_("Western Frisian")},
    {"ff", tk::N_("Fulah")},
    {"gd", tk::N_("Scottish Gaelic")},
    {"gl", tk::N_("Galician")},
    {"lg", tk::N_("Ganda")},
    {"ka", tk::N_("Georgian")},
    {"de", tk::N_("German")},
    {"el", tk::N_("Greek")},
    {"kl", tk::N_("Kalaallisut")},
    {"gn", tk::N_("Guarani")},
    {"gu", tk::N_("Gujarati")},
    {"ht", tk::N_("Haitian Creole")},
    {"ha", tk::N_("Hausa")},
    {"he", tk::N_("Hebrew")},
    {"hz", tk::N_("Herero")},
    {"hi", tk::N_("Hindi")},
    {"ho", tk::N_("Hiri Motu")},
    {"hu", tk::N_("Hungarian")},
    {"is", tk::N_("Icelandic")},
    {"io", tk::N_("Ido")},
    {"ig", tk::N_("Igbo")},
    {"id", tk::N_("Indonesian")},
    {"ia", tk::N_("Interlingua")},
    {"ie", tk::N_("Interlingue")},
    {"iu", tk::N_("Inuktitut")},
    {"ik", tk::N_("Inupiaq")},
    {"ga", tk::N_("Irish")},
    {"it", tk::N_("Italian")},
    {"ja", tk::N_("Japanese")},
    {"jv", tk::N_("Javanese")},
    {"kn", tk::N_("Kannada")},
    {"kr", tk::N_("Kanuri")},
    {"ks", tk::N_("Kashmiri")},
    {"kk", tk::N_("Kazakh")},
    {"km", tk::N_("Khmer")},
    {"ki", tk::N_("Kikuyu")},
    {"rw", tk::N_("Kinyarwanda")},
    {"ky", tk::N_("Kyrgyz")},
    {"kv", tk::N_("Komi")},
    {"kg", tk::N_("Kongo")},
    {"ko", tk::N_("Korean")},
    {"kj", tk::N_("Kuanyama")},
    {"ku", tk::N_("Kurdish")},
    {"lo", tk::N_("Lao")},
    {"la", tk::N_("Latin")},
    {"lv", tk::N_("Latvian")},
    {"li", tk::N_("Limburgish")},
    {"ln", tk::N_("Lingala")},
    {"lt", tk::N_("Lithuanian")},
    {"lu", tk::N_("Luba-Katanga")},
    {"lb", tk::N_("Luxembourgish")},
    {"mk", tk::N_("Macedonian")},
    {"mg", tk::N_("Malagasy")},
    {"ms", tk::N_("Malay")},
    {"ml", tk::N_("Malayalam")},
    {"mt", tk::N_("Maltese")},
    {"gv", tk::N_("Manx")},
    {"mi", tk::N_("Maori")},
    {"mr", tk::N_("Marathi")},
    {"mh", tk::N_("Marshallese")},
    {"mn", tk::N_("Mongolian")},
    {"na", tk::N_("Nauru")},
    {"nv", tk::N_("Navajo")},
    {"nd", tk::N_("North Ndebele")},
    {"nr", tk::N_("South Ndebele")},
    {"ng", tk::N_("Ndonga")},
    {"ne", tk::N_("Nepali")},
    {"no", tk::N_("Norwegian")},
    {"nb", tk::N_("Norwegian Bokmål")},
    {"nn", tk::N_("Norwegian Nynorsk")},
    {"ii", tk::N_("Nuosu")},
    {"oc", tk::N_("Occitan")},
    {"oj", tk::N_("Ojibwe")},
    {"or", tk::N_("Odia")},
    {"om", tk::N_("Oromo")},
    {"os", tk::N_("Ossetian")},
    {"pi", tk::N_("Pali")},
    {"ps", tk::N_("Pashto")},
    {"fa", tk::N_("Persian")},
    {"pl", tk::N_("Polish")},
    {"pt", tk::N_("Portuguese")},
    {"pt-BR", tk::N_("Portuguese (Brazil)")},
    {"pa", tk::N_("Punjabi")},
    {"qu", tk::N_("Quechua")},
    {"ro", tk::N_("Romanian")},
    {"rm", tk::N_("Romansh")},
    {"rn", tk::N_("Rundi")},
    {"ru", tk::N_("Russian")},
    {"se", tk::N_("Northern Sami")},
    {"sm", tk::N_("Samoan")},
    {"sg", tk::N_("Sango")},
    {"sa", tk::N_("Sanskrit")},
    {"sc", tk::N_("Sardinian")},
    {"sr", tk::N_("Serbian")},
    {"sn", tk::N_("Shona")},
    {"sd", tk::N_("Sindhi")},
    {"si", tk::N_("Sinhala")},
    {"sk", tk::N_("Slovak")},
    {"sl", tk::N_("Slovenian")},
    {"so", tk::N_("Somali")},
    {"st", tk::N_("Southern Sotho")},
    {"es", tk::N_("Spanish")},
    {"es-MX", tk::N_("Spanish (Mexico)")},
    {"su", tk::N_("Sundanese")},
    {"sw", tk::N_("Swahili")},
    {"ss", tk::N_("Swati")},
    {"sv", tk::N_("Swedish")},
    {"tl", tk::N_("Tagalog")},
    {"ty", tk::N_("Tahitian")},
    {"tg", tk::N_("Tajik")},
    {"ta", tk::N_("Tamil")},
    {"tt", tk::N_("Tatar")},
    {"te", tk::N_("Telugu")},
    {"th", tk::N_("Thai")},
    {"bo", tk::N_("Tibetan")},
    {"ti", tk::N_("Tigrinya")},
    {"to", tk::N_("Tongan")},
    {"ts", tk::N_("Tsonga")},
    {"tn", tk::N_("Tswana")},
    {"tr", tk::N_("Turkish")},
    {"tk", tk::N_("Turkmen")},
    {"tw", tk::N_("Twi")},
    {"ug", tk::N_("Uyghur")},
    {"uk", tk::N_("Ukrainian")},
    {"ur", tk::N_("Urdu")},
    {"uz", tk::N_("Uzbek")},
    {"ve", tk::N_("Venda")},
    {"vi", tk::N_("Vietnamese")},
    {"vo", tk::N_("Volapük")},
    {"wa", tk::N_("Walloon")},
    {"cy", tk::N_("Welsh")},
    {"wo", tk::N_("Wolof")},
    {"xh", tk::N_("Xhosa")},
    {"yi", tk::N_("Yiddish")},
    {"yo", tk::N_("Yoruba")},
    {"za", tk::N_("Zhuang")},
    {"zu", tk::N_("Zulu")},
};

// Looks up the translated display name for a BCP-47 code (exact match against
// kBcp47Languages). Falls back to returning `code` itself if not found —
// pronoun language tags may come from other Matrix clients that used a code
// outside this table.
inline std::string bcp47_language_name(std::string_view code)
{
    for (const auto& lang : kBcp47Languages)
    {
        if (lang.code == code)
        {
            return tk::tr(lang.name.data());
        }
    }
    return std::string(code);
}

// Region (ISO 3166-1 alpha-2 / UN M.49) and script (ISO 15924) subtags that
// commonly follow the language in a room-language tag. Names are msgids like
// the language table; unknown subtags are shown as-is.
inline constexpr Bcp47Language kBcp47Regions[] = {
    {"US", tk::N_("United States")},
    {"GB", tk::N_("United Kingdom")},
    {"AU", tk::N_("Australia")},
    {"CA", tk::N_("Canada")},
    {"IE", tk::N_("Ireland")},
    {"NZ", tk::N_("New Zealand")},
    {"IN", tk::N_("India")},
    {"ZA", tk::N_("South Africa")},
    {"BR", tk::N_("Brazil")},
    {"PT", tk::N_("Portugal")},
    {"ES", tk::N_("Spain")},
    {"MX", tk::N_("Mexico")},
    {"AR", tk::N_("Argentina")},
    {"CO", tk::N_("Colombia")},
    {"CL", tk::N_("Chile")},
    {"PE", tk::N_("Peru")},
    {"VE", tk::N_("Venezuela")},
    {"419", tk::N_("Latin America")},
    {"FR", tk::N_("France")},
    {"BE", tk::N_("Belgium")},
    {"CH", tk::N_("Switzerland")},
    {"LU", tk::N_("Luxembourg")},
    {"DE", tk::N_("Germany")},
    {"AT", tk::N_("Austria")},
    {"IT", tk::N_("Italy")},
    {"NL", tk::N_("Netherlands")},
    {"CN", tk::N_("China")},
    {"TW", tk::N_("Taiwan")},
    {"HK", tk::N_("Hong Kong")},
    {"SG", tk::N_("Singapore")},
    {"JP", tk::N_("Japan")},
    {"KR", tk::N_("South Korea")},
    {"RU", tk::N_("Russia")},
    {"UA", tk::N_("Ukraine")},
    {"PL", tk::N_("Poland")},
    {"SE", tk::N_("Sweden")},
    {"NO", tk::N_("Norway")},
    {"DK", tk::N_("Denmark")},
    {"FI", tk::N_("Finland")},
    {"TR", tk::N_("Turkey")},
    {"GR", tk::N_("Greece")},
    {"IL", tk::N_("Israel")},
    {"EG", tk::N_("Egypt")},
    {"SA", tk::N_("Saudi Arabia")},
    {"AE", tk::N_("United Arab Emirates")},
    {"PH", tk::N_("Philippines")},
    {"ID", tk::N_("Indonesia")},
    {"MY", tk::N_("Malaysia")},
    {"TH", tk::N_("Thailand")},
    {"VN", tk::N_("Vietnam")},
};

inline constexpr Bcp47Language kBcp47Scripts[] = {
    {"Hans", tk::N_("Simplified")}, {"Hant", tk::N_("Traditional")},   {"Latn", tk::N_("Latin")},
    {"Cyrl", tk::N_("Cyrillic")},   {"Arab", tk::N_("Arabic script")},
};

namespace detail
{
inline bool ieq(std::string_view a, std::string_view b)
{
    if (a.size() != b.size())
    {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
        {
            return false;
        }
    }
    return true;
}

template <std::size_t N>
inline const Bcp47Language* find_ci(const Bcp47Language (&table)[N], std::string_view code)
{
    for (const auto& e : table)
    {
        if (ieq(e.code, code))
        {
            return &e;
        }
    }
    return nullptr;
}
} // namespace detail

// Human-readable, translated name for a full BCP-47 tag, e.g. "en-US" ->
// "English (United States)", "zh-Hans-CN" -> "Chinese (Simplified, China)".
// Matching is case-insensitive. A tag whose primary language is not in the
// table is returned unchanged; unknown script/region subtags are shown as
// written (uppercased for regions) and other subtags are ignored.
inline std::string bcp47_display_name(std::string_view tag)
{
    std::vector<std::string_view> parts;
    for (std::size_t pos = 0; pos <= tag.size();)
    {
        std::size_t end = tag.find_first_of("-_", pos);
        if (end == std::string_view::npos)
        {
            end = tag.size();
        }
        parts.push_back(tag.substr(pos, end - pos));
        pos = end + 1;
    }
    const auto* lang = parts.empty() ? nullptr : detail::find_ci(kBcp47Languages, parts[0]);
    if (!lang)
    {
        return std::string(tag);
    }

    std::string qualifiers;
    for (std::size_t i = 1; i < parts.size(); ++i)
    {
        const std::string_view sub = parts[i];
        std::string text;
        const bool alpha4 =
            sub.size() == 4 && std::all_of(sub.begin(), sub.end(),
                                           [](char c)
                                           {
                                               return std::isalpha(static_cast<unsigned char>(c));
                                           });
        const bool region =
            sub.size() == 3 && std::all_of(sub.begin(), sub.end(),
                                           [](char c)
                                           {
                                               return std::isdigit(static_cast<unsigned char>(c));
                                           });
        const bool alpha2 =
            sub.size() == 2 && std::all_of(sub.begin(), sub.end(),
                                           [](char c)
                                           {
                                               return std::isalpha(static_cast<unsigned char>(c));
                                           });
        if (alpha4)
        {
            if (const auto* sc = detail::find_ci(kBcp47Scripts, sub))
            {
                text = tk::tr(sc->name.data());
            }
            else
            {
                text = std::string(sub);
            }
        }
        else if (alpha2 || region)
        {
            if (const auto* rg = detail::find_ci(kBcp47Regions, sub))
            {
                text = tk::tr(rg->name.data());
            }
            else
            {
                text = std::string(sub);
                for (char& c : text)
                {
                    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                }
            }
        }
        else
        {
            continue;
        }
        if (!qualifiers.empty())
        {
            qualifiers += ", ";
        }
        qualifiers += text;
    }

    const std::string name = tk::tr(lang->name.data());
    if (qualifiers.empty())
    {
        return name;
    }
    return tk::trf(tk::tr("{0} ({1})"), {name, qualifiers});
}

} // namespace tesseract::views
