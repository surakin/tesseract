// Smoke-mode `features` phase: checks the events the seeded features room
// delivers against a manifest. Compiled only with TESSERACT_ENABLE_SMOKE_MODE.

#include "app/ShellBase.h"
#include "app/SmokeFeatures.h"

#include <chrono>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace tesseract
{

namespace
{

const RoomInfo* find_room(const std::vector<RoomInfo>& rooms, const std::string& room_id)
{
    for (const auto& r : rooms)
    {
        if (r.id == room_id)
        {
            return &r;
        }
    }
    return nullptr;
}

} // namespace

// Polls the room-list entry (the room is never opened) or, for an `invite`
// check, the invites cache. Previews arrive by background backfill, so the
// timeout is long.
void ShellBase::add_smoke_room_step_(std::vector<smoke::Step>& steps,
                                     const smoke::FeatureCheck& check)
{
    const std::string room_id = check.args.value("room_id", std::string{});
    const bool invite = check.args.value("invite", false);
    const nlohmann::json expect =
        check.args.contains("expect") ? check.args["expect"] : nlohmann::json::object();

    auto invited = [this, room_id]
    {
        for (const auto& i : invites_)
        {
            if (i.room_id == room_id)
            {
                return true;
            }
        }
        return false;
    };
    if (invite)
    {
        steps.push_back({"room:" + check.name, invited, 90000, []
                         {
                             return std::string("room not in the invites list");
                         }});
        return;
    }
    // A room with no known timestamp sorts last and only gets its preview
    // backfilled once it scrolls into view, which a small CI window never
    // shows. While the check is unmet, ask for that backfill the way a scroll
    // would (idempotent: the SDK ignores the request while a pass is running).
    auto last_kick = std::make_shared<std::chrono::steady_clock::time_point>();
    steps.push_back({"room:" + check.name,
                     [this, room_id, expect, last_kick]
                     {
                         const auto* r = find_room(rooms_, room_id);
                         if (r && smoke::room_mismatch(*r, expect).empty())
                         {
                             return true;
                         }
                         const auto now = std::chrono::steady_clock::now();
                         if (now - *last_kick >= std::chrono::seconds(3))
                         {
                             *last_kick = now;
                             auto sess = active_account_;
                             run_async_(
                                 [sess, room_id]
                                 {
                                     if (sess && sess->client)
                                     {
                                         sess->client->start_background_backfill({room_id});
                                     }
                                 });
                         }
                         return false;
                     },
                     90000,
                     [this, room_id, expect]
                     {
                         const auto* r = find_room(rooms_, room_id);
                         return r ? smoke::room_mismatch(*r, expect)
                                  : std::string("room not in the room list");
                     }});
}

// Each id must appear in `rooms_` before the next one.
void ShellBase::add_smoke_order_step_(std::vector<smoke::Step>& steps,
                                      const smoke::FeatureCheck& check)
{
    std::vector<std::string> ids;
    if (check.args.contains("room_ids") && check.args["room_ids"].is_array())
    {
        for (const auto& id : check.args["room_ids"])
        {
            if (id.is_string())
            {
                ids.push_back(id.get<std::string>());
            }
        }
    }
    auto verdict = [this, ids]
    {
        long prev = -1;
        for (const auto& id : ids)
        {
            long index = -1;
            for (std::size_t i = 0; i < rooms_.size(); ++i)
            {
                if (rooms_[i].id == id)
                {
                    index = static_cast<long>(i);
                    break;
                }
            }
            if (index < 0)
            {
                return "room " + id + " is not in the room list";
            }
            if (index < prev)
            {
                return "room " + id + " is at index " + std::to_string(index) +
                       ", before an earlier-listed room (index " + std::to_string(prev) + ")";
            }
            prev = index;
        }
        return std::string{};
    };
    steps.push_back({"order:" + check.name,
                     [verdict]
                     {
                         return verdict().empty();
                     },
                     90000, verdict});
}

// One action of the manifest, done through the same handlers a click reaches
// (the compose bar, RoomView's on_send_edit / on_delete_requested /
// on_reaction_toggled / on_send_reply / on_pin_requested / on_unpin_requested). Targets are the app's own settled
// events found by body. Each poll first opens the room, then fires the action
// once, then waits for its effect to show in the recorded timeline.
void ShellBase::add_smoke_action_step_(std::vector<smoke::Step>& steps,
                                       const smoke::FeatureCheck& check)
{
    struct State
    {
        bool fired = false;
        std::string target_id;
        std::string waiting_for;
    };
    auto st = std::make_shared<State>();
    const std::string op = check.args.value("op", std::string{});
    const std::string room_id = check.args.value("room_id", std::string{});
    auto arg = [&check](const char* key)
    {
        return check.args.value(key, std::string{});
    };
    const std::string text = arg("text");
    const std::string match = arg("match");
    const std::string target_body = arg("target_body");
    const std::string new_body = arg("new_body");
    const std::string key = arg("key");
    const std::string body = arg("body");
    const std::string path = arg("path");
    const std::string filename = arg("filename");
    const std::string mime = arg("mime");
    const std::string after_body = arg("after_body");
    const std::string room_name = arg("room_name");
    const std::string room_topic = arg("topic");
    const std::string dm_user = arg("user");

    auto own_settled = [this](const std::string& wanted_body, bool contains) -> const Event*
    {
        for (const auto& [id, ev] : smoke_events_)
        {
            const bool body_ok =
                contains ? ev.body.find(wanted_body) != std::string::npos : ev.body == wanted_body;
            if (ev.sender == my_user_id_ && body_ok && !ev.event_id.empty() &&
                ev.pending_state.empty() && ev.type != EventType::Redacted)
            {
                return &ev;
            }
        }
        return nullptr;
    };

    auto step = [this, st, op, room_id, text, match, target_body, new_body, key, body, path,
                 filename, mime, after_body, room_name, room_topic, dm_user, own_settled]() -> bool
    {
        if (op == "create_room" && st->fired)
        {
            st->waiting_for = "the new room '" + room_name + "' in the room list";
            for (const auto& r : rooms_)
            {
                if (r.name == room_name && r.topic == room_topic)
                {
                    return true;
                }
            }
            return false;
        }
        if (op == "dm" && st->fired)
        {
            // The /dm navigates away from the features room: do not reopen it.
            st->waiting_for = "the new direct chat to open";
            // Only the DM with `dm_user` counts, opened by the /dm itself: any
            // other room that happens to be current must not end the step.
            const std::string dm = find_existing_dm_(dm_user);
            return !dm.empty() && current_room_id_ == dm;
        }
        if (current_room_id_ != room_id)
        {
            tab_select_room(room_id);
        }
        if (current_room_id_ != room_id || !room_view_)
        {
            st->waiting_for = "the room to open";
            return false;
        }

        if (op == "send" || op == "dm")
        {
            if (!st->fired)
            {
                auto* cb = room_view_->compose_bar();
                if (!cb)
                {
                    st->waiting_for = "the compose bar";
                    return false;
                }
                cb->set_current_text(text);
                cb->trigger_send();
                st->fired = true;
            }
            if (op == "dm")
            {
                st->waiting_for = "the new direct chat to open";
                return false; // completes on the next poll, from the branch above
            }
            st->waiting_for = "'" + match + "' to echo back from the server";
            return own_settled(match, true) != nullptr;
        }

        if (op == "create_room")
        {
            // The Create tab's button calls create_room_command_ with these options.
            RoomCreateOptions options;
            options.name = room_name;
            options.topic = room_topic;
            create_room_command_(options);
            st->fired = true;
            st->waiting_for = "the new room '" + room_name + "' in the room list";
            return false;
        }

        if (op == "read_receipt")
        {
            // Nothing is driven: opening the room above lets the timeline's own
            // viewport tracking call maybe_send_read_receipt_. Done once the receipt
            // the shell last sent points at an event at or after `after_body`.
            st->waiting_for = "a read receipt at or after '" + after_body + "'";
            const Event* anchor = own_settled(after_body, false);
            const auto sent = last_sent_receipt_.find(room_id);
            if (!anchor || sent == last_sent_receipt_.end())
            {
                return false;
            }
            const auto sep = sent->second.find('\x1f');
            const std::string who =
                sep == std::string::npos ? std::string{} : sent->second.substr(0, sep);
            const std::string event_id =
                sep == std::string::npos ? std::string{} : sent->second.substr(sep + 1);
            const auto it = smoke_events_.find(event_id);
            return who == my_user_id_ && it != smoke_events_.end() &&
                   it->second.timestamp >= anchor->timestamp;
        }

        if (op == "upload")
        {
            if (!st->fired)
            {
                auto* cb = room_view_->compose_bar();
                if (!cb || !cb->on_file_paste)
                {
                    st->waiting_for = "the compose bar's file paste handler";
                    return false;
                }
                std::ifstream file(path, std::ios::binary);
                tk::FileDropPayload payload;
                payload.bytes.assign(std::istreambuf_iterator<char>(file),
                                     std::istreambuf_iterator<char>());
                if (!file || payload.bytes.empty())
                {
                    st->waiting_for = "the upload file '" + path + "' to be readable";
                    return false;
                }
                payload.mime = mime;
                payload.filename = filename;
                std::vector<tk::FileDropPayload> files;
                files.push_back(std::move(payload));
                // The paste handler queues the attachment; Send uploads it.
                cb->on_file_paste(std::move(files));
                cb->set_current_text(std::string{});
                cb->trigger_send();
                st->fired = true;
            }
            st->waiting_for = "the image '" + filename + "' to echo back from the server";
            for (const auto& [id, ev] : smoke_events_)
            {
                if (ev.sender == my_user_id_ && ev.type == EventType::Image &&
                    ev.body.find(filename) != std::string::npos && !ev.event_id.empty() &&
                    ev.pending_state.empty())
                {
                    return true;
                }
            }
            return false;
        }

        if (!st->fired)
        {
            const Event* target = own_settled(target_body, false);
            if (!target)
            {
                st->waiting_for = "the target '" + target_body + "' to settle in the timeline";
                return false;
            }
            st->target_id = target->event_id;
            if (op == "edit" && room_view_->on_send_edit)
            {
                room_view_->on_send_edit(st->target_id, new_body, false);
            }
            else if (op == "delete" && room_view_->on_delete_requested)
            {
                room_view_->on_delete_requested(st->target_id);
            }
            else if (op == "react" && room_view_->on_reaction_toggled)
            {
                room_view_->on_reaction_toggled(st->target_id, key, std::string{});
            }
            else if (op == "pin" && room_view_->on_pin_requested)
            {
                room_view_->on_pin_requested(st->target_id);
            }
            else if (op == "unpin" && room_view_->on_unpin_requested)
            {
                room_view_->on_unpin_requested(st->target_id);
            }
            else if (op == "reply" && room_view_->on_send_reply)
            {
                room_view_->on_send_reply(st->target_id, body);
            }
            else
            {
                st->waiting_for = "a handler for op '" + op + "'";
                return false;
            }
            st->fired = true;
        }

        const auto it = smoke_events_.find(st->target_id);
        if (op == "edit")
        {
            st->waiting_for = "the target to show body '" + new_body + "' as edited";
            return it != smoke_events_.end() && it->second.is_edited && it->second.body == new_body;
        }
        if (op == "delete")
        {
            st->waiting_for = "the target to become redacted";
            return it != smoke_events_.end() && it->second.type == EventType::Redacted;
        }
        if (op == "react")
        {
            st->waiting_for = "the reaction '" + key + "' to show as mine";
            if (it == smoke_events_.end())
            {
                return false;
            }
            for (const auto& r : it->second.reactions)
            {
                if (r.key == key && r.reacted_by_me)
                {
                    return true;
                }
            }
            return false;
        }
        if (op == "pin" || op == "unpin")
        {
            // The room's resolved pin list (what the pinned banner shows).
            const bool want_pinned = op == "pin";
            st->waiting_for = want_pinned ? "the target to appear in the room's pinned list"
                                          : "the target to leave the room's pinned list";
            const auto* r = find_room(rooms_, room_id);
            if (!r)
            {
                return false;
            }
            bool pinned = false;
            for (const auto& p : r->pinned_events)
            {
                pinned = pinned || p.event_id == st->target_id;
            }
            return pinned == want_pinned;
        }
        // reply
        st->waiting_for = "the reply '" + body + "' to echo back";
        for (const auto& [id, ev] : smoke_events_)
        {
            if (ev.sender == my_user_id_ && ev.body.find(body) != std::string::npos &&
                ev.in_reply_to_id == st->target_id && !ev.event_id.empty() &&
                ev.pending_state.empty())
            {
                return true;
            }
        }
        return false;
    };

    steps.push_back({"action:" + check.name, step, 30000, [st, op]
                     {
                         return "op '" + op + "' waited for " +
                                (st->waiting_for.empty() ? std::string("its first poll")
                                                         : st->waiting_for);
                     }});
}

void ShellBase::add_smoke_feature_steps_(std::vector<smoke::Step>& steps,
                                         const std::string& manifest_path)
{
    std::ifstream in(manifest_path, std::ios::binary);
    std::stringstream buf;
    buf << in.rdbuf();
    std::string error;
    std::vector<smoke::FeatureCheck> checks;
    if (!in)
    {
        error = "cannot read '" + manifest_path + "'";
    }
    else
    {
        checks = smoke::parse_feature_manifest(buf.str(), error);
    }
    if (!error.empty())
    {
        steps.push_back({"features-manifest",
                         []
                         {
                             return false;
                         },
                         0,
                         [error]
                         {
                             return error;
                         }});
        return;
    }

    for (auto& check : checks)
    {
        if (check.kind == "room")
        {
            add_smoke_room_step_(steps, check);
            continue;
        }
        if (check.kind == "order")
        {
            add_smoke_order_step_(steps, check);
            continue;
        }
        if (check.kind == "action")
        {
            add_smoke_action_step_(steps, check);
            continue;
        }
        if (check.kind != "event")
        {
            continue;
        }
        const std::string room_id = check.args.value("room_id", std::string{});
        const std::string event_id = check.args.value("event_id", std::string{});
        const nlohmann::json expect =
            check.args.contains("expect") ? check.args["expect"] : nlohmann::json::object();
        const bool absent = expect.is_object() && expect.value("absent", false);

        auto opened = std::make_shared<bool>(false);
        auto open_room = [this, room_id, opened]
        {
            if (!*opened && !room_id.empty())
            {
                *opened = true;
                tab_select_room(room_id);
            }
        };

        if (absent)
        {
            // Passes after ~5 s of polls with the id still unrecorded; a
            // recording never passes, so the step times out.
            auto quiet = std::make_shared<std::pair<int, bool>>(0, false); // polls, recorded
            steps.push_back({"event:" + check.name,
                             [this, event_id, open_room, quiet]
                             {
                                 open_room();
                                 if (smoke_events_.count(event_id))
                                 {
                                     quiet->second = true;
                                 }
                                 return !quiet->second && ++quiet->first >= 20;
                             },
                             15000,
                             [this, event_id]
                             {
                                 return smoke::absent_mismatch(smoke_events_.count(event_id) != 0);
                             }});
            continue;
        }

        steps.push_back({"event:" + check.name,
                         [this, event_id, expect, open_room]
                         {
                             open_room();
                             const auto it = smoke_events_.find(event_id);
                             return it != smoke_events_.end() &&
                                    smoke::event_mismatch(it->second, expect).empty();
                         },
                         30000,
                         [this, event_id, expect]
                         {
                             const auto it = smoke_events_.find(event_id);
                             if (it == smoke_events_.end())
                             {
                                 return std::string("never reached the timeline");
                             }
                             return smoke::event_mismatch(it->second, expect);
                         }});
    }
}

} // namespace tesseract
