#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Portable, dependency-free, table-driven command-line parser.
//
// Callers describe their options as a table of OptionSpec and hand the
// already-UTF-8 argument list (argv[1..]) to parse(). The parser itself does
// no OS calls and no translation: help text is plain data supplied by the
// caller (already translated, if it should be), so this stays pure and unit
// testable. Each shell is responsible only for turning its native argv into
// UTF-8 strings.
//
// Grammar:
//   --long            flag
//   --long=value      option with a value
//   --long value      option with a value (Arity::Required only)
//   -s                short flag
//   -s value / -svalue  short option with a value
//   -abc              cluster of short flags (a value-taking short option
//                     ends the cluster and takes the rest as its value)
//   --                ends option parsing; everything after is positional
//   -                 a positional (conventionally "stdin")
//
// Unknown options, missing/unexpected values, and duplicates of
// non-repeatable options are reported as Diagnostics; parsing always
// continues so a GUI app never refuses to start over a stray argument.
namespace tesseract::cli
{

enum class Arity
{
    Flag,     // takes no value
    Required, // takes exactly one value
};

struct OptionSpec
{
    /// Stable key used for lookups via ParseResult (e.g. "profile").
    std::string_view id;
    /// Long name without leading dashes (e.g. "profile"). May be empty when
    /// the option only has a short form.
    std::string_view long_name;
    /// Short name without the dash, or '\0' for none.
    char short_name = '\0';
    Arity arity = Arity::Flag;
    /// Placeholder shown in usage for Required options (e.g. "NAME").
    std::string value_name;
    /// One-line description shown in usage. Already translated by the caller.
    std::string help;
    /// When false, a second occurrence is reported as Diagnostic::Duplicate
    /// (the last value still wins).
    bool repeatable = false;
    /// Hidden options parse normally but are left out of format_usage().
    bool hidden = false;
    /// Additional long names accepted for this option.
    std::vector<std::string_view> aliases;
};

/// Arguments owned by a toolkit or OS layer (Qt's -platform, AppKit's
/// -NSDocumentRevisionsDebugMode, the Carbon -psn_ process serial number, ...)
/// that should be skipped silently rather than reported as unknown. An
/// argument matches when it starts with `prefix`; when `takes_value` is set
/// and the argument has no inline "=value", the following argument is
/// consumed too.
struct ForeignOption
{
    std::string_view prefix;
    bool takes_value = false;
};

struct Diagnostic
{
    enum class Kind
    {
        Unknown,         // option not in the table
        MissingValue,    // Required option at the end of argv
        UnexpectedValue, // --flag=value on a Flag option
        Duplicate,       // non-repeatable option given more than once
        InvalidValue,    // value rejected by a higher-level validator
    };
    Kind kind;
    /// The offending argument as the user typed it (e.g. "--bogus").
    std::string arg;
    /// Optional extra detail (used by InvalidValue).
    std::string detail;
};

class ParseResult
{
public:
    /// True if the option with this id appeared at least once.
    bool has(std::string_view id) const;
    /// The value of the last occurrence, or nullopt if absent (or a Flag).
    std::optional<std::string> value(std::string_view id) const;
    /// Every value in the order given.
    std::vector<std::string> values(std::string_view id) const;
    /// Number of times the option appeared.
    std::size_t count(std::string_view id) const;

    const std::vector<std::string>& positionals() const
    {
        return positionals_;
    }
    const std::vector<Diagnostic>& diagnostics() const
    {
        return diagnostics_;
    }

    /// Every recognised occurrence, in argv order: (id, value). Value is
    /// empty for Flag options. Lets callers apply "first one wins" rules.
    const std::vector<std::pair<std::string_view, std::string>>& occurrences() const
    {
        return occurrences_;
    }

private:
    friend ParseResult parse(std::span<const OptionSpec>,
                             std::span<const std::string>,
                             std::span<const ForeignOption>);

    std::vector<std::pair<std::string_view, std::string>> occurrences_;
    std::vector<std::string> positionals_;
    std::vector<Diagnostic> diagnostics_;
};

/// Parse `args` (argv[1..], i.e. excluding the program name) against `specs`.
ParseResult parse(std::span<const OptionSpec> specs,
                  std::span<const std::string> args,
                  std::span<const ForeignOption> foreign = {});

/// Render a usage/help block:
///
///   <usage_label> <program> <synopsis>
///
///   <options_heading>
///     -h, --help               Show this help and exit
///     -p, --profile=NAME       ...
///
/// Hidden options are omitted. The help column is aligned to the longest
/// visible option. Labels are passed in so the caller can translate them.
std::string format_usage(std::span<const OptionSpec> specs,
                         std::string_view program,
                         std::string_view synopsis,
                         std::string_view usage_label,
                         std::string_view options_heading);

/// One-line English description of a diagnostic, for stderr (developer
/// facing, like the rest of Tesseract's console output).
std::string format_diagnostic(const Diagnostic& d);

} // namespace tesseract::cli
