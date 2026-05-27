#include "app/SlashCommands.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("available_commands lists me and shrug", "[slash]")
{
    const auto& cmds = tesseract::available_commands();
    REQUIRE(cmds.size() >= 2);

    auto by_name = [&](const std::string& n) {
        for (const auto& c : cmds) if (c.name == n) return &c;
        return (const tesseract::SlashCommandDescriptor*) nullptr;
    };

    const auto* me = by_name("me");
    REQUIRE(me != nullptr);
    REQUIRE(me->args_hint == "<action>");

    const auto* shrug = by_name("shrug");
    REQUIRE(shrug != nullptr);
    REQUIRE(shrug->args_hint.empty()); // /shrug takes no args
}

