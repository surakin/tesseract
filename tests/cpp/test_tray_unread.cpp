#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include <tesseract/types.h>

#include <string>
#include <unordered_map>
#include <vector>

using tesseract::RoomInfo;
using tesseract::ShellBase;
using PerAccount = std::unordered_map<std::string, std::vector<RoomInfo>>;

namespace
{
RoomInfo room_with(std::uint64_t notification, std::uint64_t highlight)
{
    RoomInfo r{};
    r.notification_count = notification;
    r.highlight_count    = highlight;
    return r;
}
} // namespace

TEST_CASE("compute_tray_unread: empty map → no unread, no highlight",
          "[shell][tray_unread]")
{
    auto [u, h] = ShellBase::compute_tray_unread({});
    CHECK_FALSE(u);
    CHECK_FALSE(h);
}

TEST_CASE("compute_tray_unread: all zero-count rooms → no indicator",
          "[shell][tray_unread]")
{
    PerAccount by_account;
    by_account["@alice:example.org"] = {room_with(0, 0), room_with(0, 0)};
    by_account["@bob:example.org"]   = {room_with(0, 0)};
    auto [u, h] = ShellBase::compute_tray_unread(by_account);
    CHECK_FALSE(u);
    CHECK_FALSE(h);
}

TEST_CASE("compute_tray_unread: any notification flips has_unread",
          "[shell][tray_unread]")
{
    PerAccount by_account;
    by_account["@alice:example.org"] = {room_with(0, 0), room_with(3, 0)};
    auto [u, h] = ShellBase::compute_tray_unread(by_account);
    CHECK(u);
    CHECK_FALSE(h);
}

TEST_CASE("compute_tray_unread: any highlight flips has_highlight",
          "[shell][tray_unread]")
{
    PerAccount by_account;
    by_account["@alice:example.org"] = {room_with(5, 1)};
    auto [u, h] = ShellBase::compute_tray_unread(by_account);
    CHECK(u);
    CHECK(h);
}

TEST_CASE("compute_tray_unread: aggregates across accounts",
          "[shell][tray_unread]")
{
    PerAccount by_account;
    by_account["@alice:example.org"] = {room_with(0, 0)};
    by_account["@bob:example.org"]   = {room_with(2, 0)};
    by_account["@carol:example.org"] = {room_with(7, 3)};
    auto [u, h] = ShellBase::compute_tray_unread(by_account);
    CHECK(u);
    CHECK(h);
}

TEST_CASE("compute_tray_unread: highlight without notification still surfaces",
          "[shell][tray_unread]")
{
    // matrix-sdk semantics make highlight a subset of notification, but the
    // aggregation must not silently drop a pure-highlight row if one ever
    // appears — it would mean failing to alert the user to a mention.
    PerAccount by_account;
    by_account["@alice:example.org"] = {room_with(0, 1)};
    auto [u, h] = ShellBase::compute_tray_unread(by_account);
    CHECK_FALSE(u);
    CHECK(h);
}
