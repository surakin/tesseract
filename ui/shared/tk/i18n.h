#pragma once
#include <string>
#include <initializer_list>

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

} // namespace tk
