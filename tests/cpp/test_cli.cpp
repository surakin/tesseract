#include <catch2/catch_test_macros.hpp>
#include <tesseract/cli.h>

#include <string>
#include <vector>

using tesseract::cli::Arity;
using tesseract::cli::Diagnostic;
using tesseract::cli::ForeignOption;
using tesseract::cli::OptionSpec;

namespace
{

std::vector<OptionSpec> specs()
{
    return {
        {"help", "help", 'h', Arity::Flag, "", "Show help", false, false, {}},
        {"verbose", "verbose", 'v', Arity::Flag, "", "Verbose", false, false, {}},
        {"profile", "profile", 'p', Arity::Required, "NAME", "Profile", false,
         false, {}},
        {"include", "include", 'I', Arity::Required, "DIR", "Include", true,
         false, {}},
        {"hidden", "hidden", '\0', Arity::Flag, "", "Start hidden", false,
         false, {"minimized"}},
        {"secret", "secret", '\0', Arity::Required, "DIR", "Secret", false,
         true, {}},
    };
}

tesseract::cli::ParseResult run(std::vector<std::string> args,
                                std::vector<ForeignOption> foreign = {})
{
    const auto s = specs();
    return tesseract::cli::parse(s, args, foreign);
}

} // namespace

TEST_CASE("cli: empty argv")
{
    auto r = run({});
    CHECK(r.occurrences().empty());
    CHECK(r.positionals().empty());
    CHECK(r.diagnostics().empty());
}

TEST_CASE("cli: long flag and long value forms")
{
    auto r = run({"--verbose", "--profile=work"});
    CHECK(r.has("verbose"));
    CHECK(r.value("profile") == "work");
    CHECK(r.diagnostics().empty());

    auto r2 = run({"--profile", "home"});
    CHECK(r2.value("profile") == "home");
    CHECK(r2.positionals().empty());
}

TEST_CASE("cli: empty inline value is kept")
{
    auto r = run({"--profile="});
    REQUIRE(r.value("profile").has_value());
    CHECK(r.value("profile")->empty());
}

TEST_CASE("cli: short flag, attached and separate short values")
{
    auto r = run({"-v", "-pwork"});
    CHECK(r.has("verbose"));
    CHECK(r.value("profile") == "work");

    auto r2 = run({"-p", "home"});
    CHECK(r2.value("profile") == "home");

    auto r3 = run({"-p=eq"});
    CHECK(r3.value("profile") == "eq");
}

TEST_CASE("cli: short cluster, value option ends the cluster")
{
    auto r = run({"-hvpwork"});
    CHECK(r.has("help"));
    CHECK(r.has("verbose"));
    CHECK(r.value("profile") == "work");

    auto r2 = run({"-vp", "x"});
    CHECK(r2.has("verbose"));
    CHECK(r2.value("profile") == "x");
}

TEST_CASE("cli: double dash ends options, lone dash is positional")
{
    auto r = run({"a", "-", "--", "--verbose", "-h"});
    CHECK_FALSE(r.has("verbose"));
    CHECK_FALSE(r.has("help"));
    CHECK(r.positionals() == std::vector<std::string>{"a", "-", "--verbose", "-h"});
}

TEST_CASE("cli: aliases resolve to the canonical id")
{
    auto r = run({"--minimized"});
    CHECK(r.has("hidden"));
}

TEST_CASE("cli: unknown options are reported and parsing continues")
{
    auto r = run({"--bogus=1", "-x", "--verbose", "matrix:u/a:b"});
    CHECK(r.has("verbose"));
    CHECK(r.positionals() == std::vector<std::string>{"matrix:u/a:b"});
    REQUIRE(r.diagnostics().size() == 2);
    CHECK(r.diagnostics()[0].kind == Diagnostic::Kind::Unknown);
    CHECK(r.diagnostics()[0].arg == "--bogus");
    CHECK(r.diagnostics()[1].arg == "-x");
}

TEST_CASE("cli: missing and unexpected values")
{
    auto r = run({"--verbose=yes", "--profile"});
    CHECK_FALSE(r.has("verbose"));
    CHECK_FALSE(r.has("profile"));
    REQUIRE(r.diagnostics().size() == 2);
    CHECK(r.diagnostics()[0].kind == Diagnostic::Kind::UnexpectedValue);
    CHECK(r.diagnostics()[1].kind == Diagnostic::Kind::MissingValue);

    auto r2 = run({"-p"});
    REQUIRE(r2.diagnostics().size() == 1);
    CHECK(r2.diagnostics()[0].kind == Diagnostic::Kind::MissingValue);
}

TEST_CASE("cli: duplicates warn, last wins; repeatable collects")
{
    auto r = run({"--profile=a", "-pb"});
    CHECK(r.value("profile") == "b");
    REQUIRE(r.diagnostics().size() == 1);
    CHECK(r.diagnostics()[0].kind == Diagnostic::Kind::Duplicate);

    auto r2 = run({"-Ia", "--include=b", "-I", "c"});
    CHECK(r2.values("include") == std::vector<std::string>{"a", "b", "c"});
    CHECK(r2.count("include") == 3);
    CHECK(r2.diagnostics().empty());
}

TEST_CASE("cli: foreign options are skipped silently")
{
    std::vector<ForeignOption> foreign = {
        {"-psn_", false}, {"-NS", true}, {"-platform", true}};
    auto r = run({"-psn_0_1234", "-NSDocumentRevisionsDebugMode", "YES",
                  "-platform", "wayland", "-platform=xcb", "--verbose"},
                 foreign);
    CHECK(r.has("verbose"));
    CHECK(r.positionals().empty());
    CHECK(r.diagnostics().empty());
    CHECK_FALSE(r.has("profile"));
}

TEST_CASE("cli: occurrences preserve argv order")
{
    auto r = run({"-v", "--profile=x", "--minimized"});
    REQUIRE(r.occurrences().size() == 3);
    CHECK(r.occurrences()[0].first == "verbose");
    CHECK(r.occurrences()[1].first == "profile");
    CHECK(r.occurrences()[1].second == "x");
    CHECK(r.occurrences()[2].first == "hidden");
}

TEST_CASE("cli: format_usage aligns columns and omits hidden options")
{
    const auto s = specs();
    const std::string usage = tesseract::cli::format_usage(
        s, "tesseract", "[OPTIONS] [URI]", "Usage:", "Options:");
    const std::string expected =
        "Usage: tesseract [OPTIONS] [URI]\n"
        "\n"
        "Options:\n"
        "  -h, --help           Show help\n"
        "  -v, --verbose        Verbose\n"
        "  -p, --profile=NAME   Profile\n"
        "  -I, --include=DIR    Include\n"
        "      --hidden         Start hidden\n";
    CHECK(usage == expected);
}

TEST_CASE("cli: format_diagnostic mentions the argument")
{
    Diagnostic d{Diagnostic::Kind::Unknown, "--bogus", {}};
    CHECK(tesseract::cli::format_diagnostic(d).find("--bogus") != std::string::npos);
    Diagnostic inv{Diagnostic::Kind::InvalidValue, "--profile", "bad name"};
    CHECK(tesseract::cli::format_diagnostic(inv).find("bad name") != std::string::npos);
}
