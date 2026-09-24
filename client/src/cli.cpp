#include "tesseract/cli.h"

#include <algorithm>

namespace tesseract::cli
{

namespace
{

const OptionSpec* find_long(std::span<const OptionSpec> specs,
                            std::string_view name)
{
    for (const auto& s : specs)
    {
        if (!s.long_name.empty() && s.long_name == name)
        {
            return &s;
        }
        if (std::find(s.aliases.begin(), s.aliases.end(), name) != s.aliases.end())
        {
            return &s;
        }
    }
    return nullptr;
}

const OptionSpec* find_short(std::span<const OptionSpec> specs, char c)
{
    for (const auto& s : specs)
    {
        if (s.short_name != '\0' && s.short_name == c)
        {
            return &s;
        }
    }
    return nullptr;
}

const ForeignOption* find_foreign(std::span<const ForeignOption> foreign,
                                  std::string_view arg)
{
    for (const auto& f : foreign)
    {
        if (!f.prefix.empty() && arg.starts_with(f.prefix))
        {
            return &f;
        }
    }
    return nullptr;
}

} // namespace

bool ParseResult::has(std::string_view id) const
{
    return count(id) > 0;
}

std::optional<std::string> ParseResult::value(std::string_view id) const
{
    for (auto it = occurrences_.rbegin(); it != occurrences_.rend(); ++it)
    {
        if (it->first == id)
        {
            return it->second;
        }
    }
    return std::nullopt;
}

std::vector<std::string> ParseResult::values(std::string_view id) const
{
    std::vector<std::string> out;
    for (const auto& [oid, v] : occurrences_)
    {
        if (oid == id)
        {
            out.push_back(v);
        }
    }
    return out;
}

std::size_t ParseResult::count(std::string_view id) const
{
    return static_cast<std::size_t>(
        std::count_if(occurrences_.begin(), occurrences_.end(),
                      [id](const auto& o) { return o.first == id; }));
}

ParseResult parse(std::span<const OptionSpec> specs,
                  std::span<const std::string> args,
                  std::span<const ForeignOption> foreign)
{
    ParseResult r;

    auto record = [&r](const OptionSpec& spec, std::string value,
                       const std::string& as_typed)
    {
        if (!spec.repeatable && r.has(spec.id))
        {
            r.diagnostics_.push_back(
                {Diagnostic::Kind::Duplicate, as_typed, {}});
        }
        r.occurrences_.emplace_back(spec.id, std::move(value));
    };

    bool options_done = false;
    for (std::size_t i = 0; i < args.size(); ++i)
    {
        const std::string& arg = args[i];

        if (options_done || arg.size() < 2 || arg[0] != '-')
        {
            // Plain word, lone "-", or anything after "--".
            r.positionals_.push_back(arg);
            continue;
        }
        if (arg == "--")
        {
            options_done = true;
            continue;
        }

        if (const ForeignOption* f = find_foreign(foreign, arg))
        {
            if (f->takes_value && arg.find('=') == std::string::npos &&
                i + 1 < args.size())
            {
                ++i;
            }
            continue;
        }

        if (arg.starts_with("--"))
        {
            const std::string_view body = std::string_view(arg).substr(2);
            const auto eq = body.find('=');
            const std::string_view name = body.substr(0, eq);
            const std::string as_typed = "--" + std::string(name);

            const OptionSpec* spec = find_long(specs, name);
            if (!spec)
            {
                r.diagnostics_.push_back({Diagnostic::Kind::Unknown, as_typed, {}});
                continue;
            }

            if (spec->arity == Arity::Flag)
            {
                if (eq != std::string_view::npos)
                {
                    r.diagnostics_.push_back(
                        {Diagnostic::Kind::UnexpectedValue, as_typed, {}});
                    continue;
                }
                record(*spec, {}, as_typed);
                continue;
            }

            if (eq != std::string_view::npos)
            {
                record(*spec, std::string(body.substr(eq + 1)), as_typed);
            }
            else if (i + 1 < args.size())
            {
                record(*spec, args[++i], as_typed);
            }
            else
            {
                r.diagnostics_.push_back(
                    {Diagnostic::Kind::MissingValue, as_typed, {}});
            }
            continue;
        }

        // Short option or cluster: "-a", "-abc", "-pNAME", "-p NAME".
        for (std::size_t k = 1; k < arg.size(); ++k)
        {
            const char c = arg[k];
            const std::string as_typed = std::string("-") + c;
            const OptionSpec* spec = find_short(specs, c);
            if (!spec)
            {
                r.diagnostics_.push_back({Diagnostic::Kind::Unknown, as_typed, {}});
                continue;
            }
            if (spec->arity == Arity::Flag)
            {
                record(*spec, {}, as_typed);
                continue;
            }
            // Value-taking short option consumes the rest of the cluster,
            // or the next argument when the cluster ends here.
            if (k + 1 < arg.size())
            {
                std::string_view rest = std::string_view(arg).substr(k + 1);
                if (rest.starts_with('='))
                {
                    rest.remove_prefix(1);
                }
                record(*spec, std::string(rest), as_typed);
            }
            else if (i + 1 < args.size())
            {
                record(*spec, args[++i], as_typed);
            }
            else
            {
                r.diagnostics_.push_back(
                    {Diagnostic::Kind::MissingValue, as_typed, {}});
            }
            break;
        }
    }

    return r;
}

std::string format_usage(std::span<const OptionSpec> specs,
                         std::string_view program,
                         std::string_view synopsis,
                         std::string_view usage_label,
                         std::string_view options_heading)
{
    std::vector<std::pair<std::string, const OptionSpec*>> rows;
    std::size_t width = 0;
    for (const auto& s : specs)
    {
        if (s.hidden)
        {
            continue;
        }
        std::string left = s.short_name != '\0'
                               ? std::string("-") + s.short_name
                               : std::string("    ");
        if (!s.long_name.empty())
        {
            if (s.short_name != '\0')
            {
                left += ", ";
            }
            left += "--";
            left += s.long_name;
            if (s.arity == Arity::Required)
            {
                left += "=";
                left += s.value_name.empty() ? std::string("VALUE") : s.value_name;
            }
        }
        else if (s.arity == Arity::Required)
        {
            left += " ";
            left += s.value_name.empty() ? std::string("VALUE") : s.value_name;
        }
        width = std::max(width, left.size());
        rows.emplace_back(std::move(left), &s);
    }

    std::string out;
    out += usage_label;
    out += " ";
    out += program;
    if (!synopsis.empty())
    {
        out += " ";
        out += synopsis;
    }
    out += "\n\n";
    out += options_heading;
    out += "\n";
    for (const auto& [left, spec] : rows)
    {
        out += "  ";
        out += left;
        out.append(width - left.size() + 3, ' ');
        out += spec->help;
        out += "\n";
    }
    return out;
}

std::string format_diagnostic(const Diagnostic& d)
{
    switch (d.kind)
    {
    case Diagnostic::Kind::Unknown:
        return "unknown option '" + d.arg + "' (ignored)";
    case Diagnostic::Kind::MissingValue:
        return "option '" + d.arg + "' requires a value (ignored)";
    case Diagnostic::Kind::UnexpectedValue:
        return "option '" + d.arg + "' does not take a value (ignored)";
    case Diagnostic::Kind::Duplicate:
        return "option '" + d.arg + "' given more than once";
    case Diagnostic::Kind::InvalidValue:
        return "invalid value for '" + d.arg + "'" +
               (d.detail.empty() ? std::string() : ": " + d.detail) +
               " (ignored)";
    }
    return {};
}

} // namespace tesseract::cli
