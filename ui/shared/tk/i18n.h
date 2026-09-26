#pragma once
#include <cstdint>
#include <ctime>
#include <initializer_list>
#include <string>
#include <string_view>

namespace tk
{

// Load <dir>/tesseract.<lang>.mo, with fallback: lang-REGION -> lang -> identity.
// Must be called (and return) before any other thread calls tr/trn/trf. The global
// catalog is written once at startup and read lock-free thereafter.
// Failure = identity (English).
void set_locale(const std::string& i18n_dir, const std::string& lang);
const std::string& current_locale();

// msgid -> translation string, or source if not found
std::string tr(const char* source);

// Plural: pick form based on n using the catalog's Plural-Forms rule
std::string trn(const char* sing, const char* plur, long n);

// Positional interpolation: replace {0}, {1}, ... in fmt with args (reorderable)
std::string trf(const std::string& fmt, std::initializer_list<std::string> args);

// Marks a literal for extraction without translating it, for static tables
// whose entries go through tr() only when displayed:
//   static constexpr const char* kLabels[] = {tk::N_("Timezone"), ...};
//   label = tk::tr(kLabels[i]);
constexpr const char* N_(const char* source)
{
    return source;
}

// Format `tm` with a translatable strftime-style pattern, so a locale can
// reorder it ("%B %-d, %Y" -> "%-d %B %Y"). Month and weekday names come from
// the catalog, not the C locale. Supported: %A / %a (weekday, full / "Mon"),
// %B / %b (month, full / "Jan"), %d / %-d (day), %m / %-m (month number),
// %Y / %y (year), %%. Anything else is copied through.
//   tk::format_date(tm, tk::tr("%B %-d, %Y"))
std::string format_date(const std::tm& tm, std::string_view pattern);

// Two-letter weekday label for a calendar header ("Su" .. "Sa"), 0 = Sunday.
std::string weekday_initials(int wday);

// Whole-unit file size with a translated unit: "512 B", "3 KB", "12 MB",
// "2 GB" (1024-based).
std::string format_size(std::uint64_t bytes);

} // namespace tk
