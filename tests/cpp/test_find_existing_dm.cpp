#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"
#include <tesseract/types.h>

#include <string>
#include <vector>

using tesseract::RoomInfo;
using tesseract::ShellBase;

namespace
{
RoomInfo dm_with(const std::string& id, const std::string& counterpart)
{
    RoomInfo r{};
    r.id                     = id;
    r.is_direct              = true;
    r.dm_counterpart_user_id = counterpart;
    return r;
}

RoomInfo group(const std::string& id)
{
    RoomInfo r{};
    r.id        = id;
    r.is_direct = false;
    return r;
}
} // namespace

TEST_CASE("find_existing_dm: returns matching direct room id",
          "[shell][find_existing_dm]")
{
    std::vector<RoomInfo> rooms = {
        group("!group:example.org"),
        dm_with("!dm-bob:example.org", "@bob:example.org"),
        dm_with("!dm-alice:example.org", "@alice:example.org"),
    };
    CHECK(ShellBase::find_existing_dm(rooms, "@alice:example.org") ==
          "!dm-alice:example.org");
    CHECK(ShellBase::find_existing_dm(rooms, "@bob:example.org") ==
          "!dm-bob:example.org");
}

TEST_CASE("find_existing_dm: no DM with that user → empty",
          "[shell][find_existing_dm]")
{
    std::vector<RoomInfo> rooms = {
        group("!group:example.org"),
        dm_with("!dm-bob:example.org", "@bob:example.org"),
    };
    CHECK(ShellBase::find_existing_dm(rooms, "@carol:example.org").empty());
}

TEST_CASE("find_existing_dm: empty user id never matches",
          "[shell][find_existing_dm]")
{
    // A room whose counterpart could not be identified has an empty
    // dm_counterpart_user_id; an empty query must not match it.
    std::vector<RoomInfo> rooms = {dm_with("!dm:example.org", "")};
    CHECK(ShellBase::find_existing_dm(rooms, "").empty());
}

TEST_CASE("find_existing_dm: non-direct room with same counterpart is skipped",
          "[shell][find_existing_dm]")
{
    // dm_counterpart_user_id is populated even for rooms not marked direct;
    // the fast path must match get_or_create_dm, which only reuses is_direct
    // rooms, so a non-direct room must fall through to async creation.
    RoomInfo not_direct{};
    not_direct.id                     = "!room:example.org";
    not_direct.is_direct              = false;
    not_direct.dm_counterpart_user_id = "@alice:example.org";
    std::vector<RoomInfo> rooms = {not_direct};
    CHECK(ShellBase::find_existing_dm(rooms, "@alice:example.org").empty());
}

TEST_CASE("find_existing_dm: empty room list → empty",
          "[shell][find_existing_dm]")
{
    CHECK(ShellBase::find_existing_dm({}, "@alice:example.org").empty());
}
