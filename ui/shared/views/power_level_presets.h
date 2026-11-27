#pragma once

// The three named Matrix power-level presets (Default / Moderator / Admin),
// shared by the room-permissions settings combos and the per-member
// PowerLevelDialog so both offer the same labels and values.

#include "tk/combobox.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace tesseract::views
{

inline constexpr std::int64_t kPowerLevelDefault = 0;
inline constexpr std::int64_t kPowerLevelModerator = 50;
inline constexpr std::int64_t kPowerLevelAdmin = 100;

// Default / Moderator / Admin as combo options (values "0" / "50" / "100"),
// leaving out any preset above `max_level`.
inline std::vector<tk::ComboBox::Option>
power_level_preset_options(std::int64_t max_level = std::numeric_limits<std::int64_t>::max())
{
    std::vector<tk::ComboBox::Option> out;
    if (kPowerLevelDefault <= max_level)
    {
        out.push_back({tk::tr("Default"), std::to_string(kPowerLevelDefault)});
    }
    if (kPowerLevelModerator <= max_level)
    {
        out.push_back({tk::tr("Moderator"), std::to_string(kPowerLevelModerator)});
    }
    if (kPowerLevelAdmin <= max_level)
    {
        out.push_back({tk::tr("Admin"), std::to_string(kPowerLevelAdmin)});
    }
    return out;
}

inline bool is_power_level_preset(std::int64_t level)
{
    return level == kPowerLevelDefault || level == kPowerLevelModerator ||
           level == kPowerLevelAdmin;
}

// A member's role as the member list labels it. None = the room's default
// level (no label). Owner = a room-v12 privileged creator, whose effective
// level arrives as INT64_MAX.
enum class MemberRole
{
    None,
    Owner,
    Admin,
    Moderator,
    Custom,
};

inline MemberRole member_role(std::int64_t level, std::int64_t users_default)
{
    if (level == std::numeric_limits<std::int64_t>::max())
    {
        return MemberRole::Owner;
    }
    if (level == users_default)
    {
        return MemberRole::None;
    }
    if (level == kPowerLevelAdmin)
    {
        return MemberRole::Admin;
    }
    if (level == kPowerLevelModerator)
    {
        return MemberRole::Moderator;
    }
    return MemberRole::Custom;
}

// "Owner" / "Admin" / "Moderator" / "Custom (N)"; empty for MemberRole::None.
inline std::string member_role_label(MemberRole role, std::int64_t level)
{
    switch (role)
    {
    case MemberRole::None:
        return {};
    case MemberRole::Owner:
        return tk::tr("Owner");
    case MemberRole::Admin:
        return tk::tr("Admin");
    case MemberRole::Moderator:
        return tk::tr("Moderator");
    case MemberRole::Custom:
        return tk::trf(tk::tr("Custom ({0})"), {std::to_string(level)});
    }
    return {};
}

inline tk::Color member_role_color(MemberRole role, const tk::Palette& pal)
{
    switch (role)
    {
    case MemberRole::Owner:
        return pal.role_owner;
    case MemberRole::Admin:
        return pal.role_admin;
    case MemberRole::Moderator:
        return pal.role_moderator;
    case MemberRole::None:
    case MemberRole::Custom:
        break;
    }
    return pal.text_muted;
}

} // namespace tesseract::views
