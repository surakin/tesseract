#include "StateEventPhrases.h"
#include "MessageListView.h"
#include "tk/i18n.h"

#include <cstdlib>
#include <vector>

namespace tesseract::views
{
namespace
{
using Params = std::vector<std::string>;

struct ValuePhrase
{
    const char* value;
    const char* phrase; // "{0}" is the actor; marked for extraction, translated where shown
};

// Actor shown for a row, never empty.
std::string actor(const MessageRowData& m)
{
    const std::string s = m.member_label();
    return s.empty() ? tk::tr("Someone") : s;
}

const std::string& param(const Params& p, std::size_t i)
{
    static const std::string empty;
    return i < p.size() ? p[i] : empty;
}

// Phrase for `value` from `table`, or `generic` (also "{0} = actor") when the value is
// unknown or empty.
template <std::size_t N>
std::string by_value(const ValuePhrase (&table)[N], const std::string& value, const char* generic,
                     const std::string& who)
{
    for (const auto& e : table)
    {
        if (value == e.value)
        {
            return tk::trf(tk::tr(e.phrase), {who});
        }
    }
    return tk::trf(tk::tr(generic), {who});
}

constexpr ValuePhrase kJoinRules[] = {
    {"public", tk::N_("{0} made the room public")},
    {"invite", tk::N_("{0} made the room invite-only")},
    {"knock", tk::N_("{0} made the room ask-to-join")},
    {"restricted", tk::N_("{0} allowed members of certain spaces to join the room")},
    {"knock_restricted",
     tk::N_("{0} allowed members of certain spaces to join the room, and others to ask to join")},
    {"private", tk::N_("{0} made the room private")},
};

constexpr ValuePhrase kHistoryVisibility[] = {
    {"world_readable", tk::N_("{0} made the room history visible to anyone")},
    {"shared", tk::N_("{0} made the room history visible to all room members")},
    {"invited",
     tk::N_("{0} made the room history visible to members from the point they were invited")},
    {"joined", tk::N_("{0} made the room history visible to members from the point they joined")},
};

constexpr ValuePhrase kGuestAccess[] = {
    {"can_join", tk::N_("{0} allowed guests to join the room")},
    {"forbidden", tk::N_("{0} stopped guests from joining the room")},
};

// Power-level fields, in the wire format's `field|<key>|old|new` entries.
struct FieldLabel
{
    const char* key;
    const char* changed; // "{0}" actor, "{1}" old, "{2}" new
    const char* set;     // "{0}" actor, "{1}" new
};

constexpr FieldLabel kPowerFields[] = {
    {"users_default", tk::N_("{0} changed the default power level from {1} to {2}"),
     tk::N_("{0} set the default power level to {1}")},
    {"events_default",
     tk::N_("{0} changed the power level needed to send messages from {1} to {2}"),
     tk::N_("{0} set the power level needed to send messages to {1}")},
    {"state_default",
     tk::N_("{0} changed the power level needed to change room settings from {1} to {2}"),
     tk::N_("{0} set the power level needed to change room settings to {1}")},
    {"ban", tk::N_("{0} changed the power level needed to ban from {1} to {2}"),
     tk::N_("{0} set the power level needed to ban to {1}")},
    {"kick", tk::N_("{0} changed the power level needed to remove members from {1} to {2}"),
     tk::N_("{0} set the power level needed to remove members to {1}")},
    {"redact", tk::N_("{0} changed the power level needed to remove messages from {1} to {2}"),
     tk::N_("{0} set the power level needed to remove messages to {1}")},
    {"invite", tk::N_("{0} changed the power level needed to invite from {1} to {2}"),
     tk::N_("{0} set the power level needed to invite to {1}")},
};

// Split "a|b|c|d" into at most four parts (the last keeps any further bars).
Params split_entry(const std::string& s)
{
    Params out;
    std::size_t start = 0;
    while (out.size() < 3)
    {
        const auto bar = s.find('|', start);
        if (bar == std::string::npos)
        {
            break;
        }
        out.push_back(s.substr(start, bar - start));
        start = bar + 1;
    }
    out.push_back(s.substr(start));
    return out;
}

std::string power_levels_phrase(const MessageRowData& m)
{
    const std::string who = actor(m);
    long total = 0;
    std::string single;
    // A room's first power levels event diffs against the spec defaults, so an
    // old value is never empty there; it still reads as "set", not "changed".
    const bool first = m.state_change == "set";
    for (const auto& raw : m.state_params)
    {
        const Params e = split_entry(raw);
        if (e.size() == 4 && e[0] == "more")
        {
            total += std::strtol(e[3].c_str(), nullptr, 10);
            continue;
        }
        if (e.size() != 4)
        {
            continue;
        }
        ++total;
        const std::string& scope = e[0];
        const std::string& key = e[1];
        const std::string& old_v = e[2];
        const std::string& new_v = e[3];
        if (scope == "field")
        {
            for (const auto& f : kPowerFields)
            {
                if (key == f.key)
                {
                    single = (first || old_v.empty())
                                 ? tk::trf(tk::tr(f.set), {who, new_v})
                                 : tk::trf(tk::tr(f.changed), {who, old_v, new_v});
                }
            }
        }
        else if (scope == "user")
        {
            single =
                (first || old_v.empty())
                    ? tk::trf(tk::tr("{0} set the power level of {1} to {2}"), {who, key, new_v})
                    : tk::trf(tk::tr("{0} changed the power level of {1} from {2} to {3}"),
                              {who, key, old_v, new_v});
        }
        else if (scope == "event" && new_v.empty() && !old_v.empty())
        {
            // The override was removed, so the type falls back to the room default.
            single =
                tk::trf(tk::tr("{0} removed the custom power level needed for {1}"), {who, key});
        }
        else if (scope == "event")
        {
            single = (first || old_v.empty())
                         ? tk::trf(tk::tr("{0} set the power level needed for {1} to {2}"),
                                   {who, key, new_v})
                         : tk::trf(tk::tr("{0} changed the power level needed for {1} from {2} to "
                                          "{3}"),
                                   {who, key, old_v, new_v});
        }
    }
    if (total == 1 && !single.empty())
    {
        return single;
    }
    if (total <= 0)
    {
        return tk::trf(tk::tr("{0} changed permissions"), {who});
    }
    return tk::trf(tk::trn("{0} changed {1} permission", "{0} changed {1} permissions", total),
                   {who, std::to_string(total)});
}

std::string canonical_alias_phrase(const MessageRowData& m)
{
    const std::string who = actor(m);
    const std::string& now = param(m.state_params, 0);
    const std::string& before = param(m.state_params, 1);
    if (m.state_change == "removed")
    {
        return tk::trf(tk::tr("{0} removed the room's main address"), {who});
    }
    if (now.empty())
    {
        return tk::trf(tk::tr("{0} changed the room's main address"), {who});
    }
    if (m.state_change == "changed" && !before.empty() && before != now)
    {
        return tk::trf(tk::tr("{0} changed the room's main address from {1} to {2}"),
                       {who, before, now});
    }
    return tk::trf(tk::tr("{0} set the room's main address to {1}"), {who, now});
}

std::string create_phrase(const MessageRowData& m)
{
    const std::string who = actor(m);
    const std::string& version = param(m.state_params, 0);
    const std::string& predecessor = param(m.state_params, 1);
    if (!predecessor.empty())
    {
        return tk::trf(tk::tr("{0} created the room as an upgrade of {1}"), {who, predecessor});
    }
    if (!version.empty())
    {
        return tk::trf(tk::tr("{0} created the room (room version {1})"), {who, version});
    }
    return tk::trf(tk::tr("{0} created the room"), {who});
}

std::string space_phrase(const MessageRowData& m, bool child)
{
    const std::string who = actor(m);
    const std::string& other = m.state_key;
    const bool removed = m.state_change == "removed";
    if (other.empty())
    {
        if (child)
        {
            return removed ? tk::trf(tk::tr("{0} removed a room from the space"), {who})
                           : tk::trf(tk::tr("{0} added a room to the space"), {who});
        }
        return removed ? tk::trf(tk::tr("{0} removed a parent space"), {who})
                       : tk::trf(tk::tr("{0} added a parent space"), {who});
    }
    if (child)
    {
        return removed ? tk::trf(tk::tr("{0} removed {1} from the space"), {who, other})
                       : tk::trf(tk::tr("{0} added {1} to the space"), {who, other});
    }
    return removed ? tk::trf(tk::tr("{0} removed {1} as a parent space"), {who, other})
                   : tk::trf(tk::tr("{0} added {1} as a parent space"), {who, other});
}

std::string policy_phrase(const MessageRowData& m)
{
    const std::string who = actor(m);
    const std::string& entity = param(m.state_params, 0);
    const std::string& reason = param(m.state_params, 2);
    const bool removed = m.state_change == "removed";
    std::string line;
    if (entity.empty())
    {
        line = removed ? tk::trf(tk::tr("{0} removed a moderation policy rule"), {who})
                       : tk::trf(tk::tr("{0} added a moderation policy rule"), {who});
    }
    else
    {
        line =
            removed
                ? tk::trf(tk::tr("{0} removed the moderation policy rule for {1}"), {who, entity})
                : tk::trf(tk::tr("{0} added a moderation policy rule for {1}"), {who, entity});
    }
    if (!removed && !reason.empty())
    {
        line = tk::trf(tk::tr("{0}: {1}"), {line, reason});
    }
    return line;
}

// "Alice changed the server ACL: 3 servers now denied; IP literals no longer allowed".
// The ACL itself can be huge, so the row never lists hostnames: each param is a
// `deny+3` / `allow-1` / `ip-` style count line.
std::string server_acl_phrase(const MessageRowData& m)
{
    std::vector<std::string> parts;
    for (const auto& line : m.state_params)
    {
        const auto cut = line.find_first_of("+-");
        if (cut == std::string::npos)
        {
            continue;
        }
        const std::string tag = line.substr(0, cut);
        const bool added = line[cut] == '+';
        const std::string count_text = line.substr(cut + 1);
        const long n = count_text.empty() ? 0 : std::strtol(count_text.c_str(), nullptr, 10);
        if (tag == "deny")
        {
            parts.push_back(
                added ? tk::trf(tk::trn("{0} server now denied", "{0} servers now denied", n),
                                {count_text})
                      : tk::trf(tk::trn("{0} server no longer denied",
                                        "{0} servers no longer denied", n),
                                {count_text}));
        }
        else if (tag == "allow")
        {
            parts.push_back(
                added ? tk::trf(tk::trn("{0} server now allowed", "{0} servers now allowed", n),
                                {count_text})
                      : tk::trf(tk::trn("{0} server no longer allowed",
                                        "{0} servers no longer allowed", n),
                                {count_text}));
        }
        else if (tag == "ip")
        {
            parts.push_back(added ? tk::tr("IP literals now allowed")
                                  : tk::tr("IP literals no longer allowed"));
        }
    }

    const std::string s = m.member_label();
    std::string line = s.empty() ? tk::tr("The server ACL was changed")
                                 : tk::trf(tk::tr("{0} changed the server ACL"), {s});
    if (!parts.empty())
    {
        std::string detail;
        for (const auto& p : parts)
        {
            detail += detail.empty() ? p : "; " + p;
        }
        line = tk::trf(tk::tr("{0}: {1}"), {line, detail});
    }
    return line;
}

struct KnownStateType
{
    const char* type;
    const char* label; // marked for extraction, translated where shown
};

// State event types Tesseract reads into room state rather than rendering as timeline rows,
// with the label a developer row shows for them. Only these are reported as "handled": the
// row exists solely under the "Show unhandled events" setting. Keep in step with the Rust
// readers (bridge info, service members, room features, emote packs).
constexpr KnownStateType kKnownStateTypes[] = {
    {"m.bridge", tk::N_("Bridge information")},
    {"uk.half-shot.bridge", tk::N_("Bridge information")},
    {"io.element.functional_members", tk::N_("Service members")},
    {"com.beeper.room_features", tk::N_("Bridge room features")},
    {"im.ponies.room_emotes", tk::N_("Custom emoji pack")},
};

std::string fallback_phrase(const MessageRowData& m)
{
    const std::string label = known_state_label(m.state_type);
    return tk::trf(tk::tr("{0} updated the room state: {1}"),
                   {actor(m), label.empty() ? m.state_type : label});
}
} // namespace

std::string known_state_label(const std::string& body)
{
    const std::string type = body.substr(0, body.find(" ["));
    for (const auto& k : kKnownStateTypes)
    {
        if (type == k.type)
        {
            return tk::tr(k.label);
        }
    }
    return {};
}

std::string state_event_phrase(const MessageRowData& m)
{
    const std::string& t = m.state_type;
    const std::string who = actor(m);
    const std::string& now = param(m.state_params, 0);
    if (t == "m.room.join_rules")
    {
        return by_value(kJoinRules, now, tk::N_("{0} changed the join rules"), who);
    }
    if (t == "m.room.history_visibility")
    {
        return by_value(kHistoryVisibility, now, tk::N_("{0} changed the history visibility"), who);
    }
    if (t == "m.room.guest_access")
    {
        return by_value(kGuestAccess, now, tk::N_("{0} changed guest access"), who);
    }
    if (t == "m.room.encryption")
    {
        return tk::trf(tk::tr("{0} turned on end-to-end encryption"), {who});
    }
    if (t == "m.room.avatar")
    {
        if (m.state_change == "removed")
        {
            return tk::trf(tk::tr("{0} removed the room avatar"), {who});
        }
        return m.state_change == "set" ? tk::trf(tk::tr("{0} set the room avatar"), {who})
                                       : tk::trf(tk::tr("{0} changed the room avatar"), {who});
    }
    if (t == "m.room.canonical_alias")
    {
        return canonical_alias_phrase(m);
    }
    if (t == "m.room.create")
    {
        return create_phrase(m);
    }
    if (t == "m.room.power_levels")
    {
        return power_levels_phrase(m);
    }
    if (t == "m.room.third_party_invite")
    {
        return now.empty() ? tk::trf(tk::tr("{0} sent a third-party invite"), {who})
                           : tk::trf(tk::tr("{0} sent a third-party invite to {1}"), {who, now});
    }
    if (t == "m.space.child" || t == "m.space.parent")
    {
        return space_phrase(m, t == "m.space.child");
    }
    if (t == "m.policy.rule.room" || t == "m.policy.rule.server" || t == "m.policy.rule.user")
    {
        return policy_phrase(m);
    }
    if (t == "m.room.server_acl")
    {
        return server_acl_phrase(m);
    }
    return fallback_phrase(m);
}

std::string state_summary_run_phrase(const std::string& sender_label, std::size_t count)
{
    const long n = static_cast<long>(count);
    const std::string num = std::to_string(count);
    if (sender_label.empty())
    {
        return tk::trf(tk::trn("The room settings were updated ({0} change)",
                               "The room settings were updated ({0} changes)", n),
                       {num});
    }
    return tk::trf(tk::trn("{0} updated the room settings ({1} change)",
                           "{0} updated the room settings ({1} changes)", n),
                   {sender_label, num});
}

std::string state_event_run_phrase(const std::string& sender_label, const std::string& state_type,
                                   std::size_t count, const MessageRowData& last)
{
    const std::string& t = state_type;
    const std::string who = sender_label.empty() ? tk::tr("Someone") : sender_label;
    const long n = static_cast<long>(count);
    const std::string num = std::to_string(count);

    if (t == "m.room.server_acl")
    {
        if (sender_label.empty())
        {
            return tk::trf(tk::trn("The server ACL was changed {0} time",
                                   "The server ACL was changed {0} times", n),
                           {num});
        }
        return tk::trf(tk::trn("{0} changed the server ACL {1} time",
                               "{0} changed the server ACL {1} times", n),
                       {who, num});
    }
    if (t == "m.space.child")
    {
        return last.state_change == "removed"
                   ? tk::trf(tk::trn("{0} removed {1} room from the space",
                                     "{0} removed {1} rooms from the space", n),
                             {who, num})
                   : tk::trf(tk::trn("{0} added {1} room to the space",
                                     "{0} added {1} rooms to the space", n),
                             {who, num});
    }
    if (t == "m.space.parent")
    {
        return last.state_change == "removed" ? tk::trf(tk::trn("{0} removed {1} parent space",
                                                                "{0} removed {1} parent spaces", n),
                                                        {who, num})
                                              : tk::trf(tk::trn("{0} added {1} parent space",
                                                                "{0} added {1} parent spaces", n),
                                                        {who, num});
    }

    std::string base;
    if (t == "m.room.join_rules")
    {
        base = tk::trf(tk::tr("{0} changed the join rules"), {who});
    }
    else if (t == "m.room.history_visibility")
    {
        base = tk::trf(tk::tr("{0} changed the history visibility"), {who});
    }
    else if (t == "m.room.guest_access")
    {
        base = tk::trf(tk::tr("{0} changed guest access"), {who});
    }
    else if (t == "m.room.encryption")
    {
        base = tk::trf(tk::tr("{0} turned on end-to-end encryption"), {who});
    }
    else if (t == "m.room.avatar")
    {
        base = tk::trf(tk::tr("{0} changed the room avatar"), {who});
    }
    else if (t == "m.room.canonical_alias")
    {
        base = tk::trf(tk::tr("{0} changed the room's main address"), {who});
    }
    else if (t == "m.room.create")
    {
        base = tk::trf(tk::tr("{0} created the room"), {who});
    }
    else if (t == "m.room.power_levels")
    {
        base = tk::trf(tk::tr("{0} changed permissions"), {who});
    }
    else if (t == "m.room.third_party_invite")
    {
        base = tk::trf(tk::tr("{0} sent third-party invites"), {who});
    }
    else if (t == "m.policy.rule.room" || t == "m.policy.rule.server" || t == "m.policy.rule.user")
    {
        base = tk::trf(tk::tr("{0} updated moderation policy rules"), {who});
    }
    else
    {
        const std::string label = known_state_label(t);
        base = tk::trf(tk::tr("{0} updated the room state: {1}"), {who, label.empty() ? t : label});
    }
    if (count <= 1)
    {
        return base;
    }
    return tk::trf(tk::trn("{0} ({1} time)", "{0} ({1} times)", n), {base, num});
}
} // namespace tesseract::views
