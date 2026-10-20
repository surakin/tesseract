// Smoke-mode run (CI): drives the real shell through sign-in, room open,
// receive and send against a homeserver. Compiled only with
// TESSERACT_ENABLE_SMOKE_MODE.

#include "app/ShellBase.h"
#include "views/MainAppWidget.h"

#include <chrono>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

namespace tesseract
{

namespace
{

std::string smoke_env(const char* name)
{
#ifdef _WIN32
    char* v = nullptr;
    size_t n = 0;
    if (_dupenv_s(&v, &n, name) != 0 || !v)
    {
        return {};
    }
    std::string out(v);
    std::free(v);
    return out;
#else
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string{};
#endif
}

struct SmokeEnv
{
    // "fresh" (default): sign in with a password, then the plain and encrypted
    // room checks. "restore": relaunch on the same profile, expect to be
    // signed in without credentials, then sign out. "signed-out": relaunch
    // after that, expect the login view and no session.
    std::string phase = []
    {
        auto v = smoke_env("TESSERACT_SMOKE_PHASE");
        return v.empty() ? std::string("fresh") : v;
    }();
    std::string homeserver = smoke_env("TESSERACT_SMOKE_HOMESERVER");
    std::string user = smoke_env("TESSERACT_SMOKE_USER");
    std::string password = smoke_env("TESSERACT_SMOKE_PASSWORD");
    std::string room_id = smoke_env("TESSERACT_SMOKE_ROOM_ID");
    std::string ping = smoke_env("TESSERACT_SMOKE_PING");
    std::string pong = smoke_env("TESSERACT_SMOKE_PONG");
    std::string e2ee_room_id = smoke_env("TESSERACT_SMOKE_E2EE_ROOM_ID");
    std::string e2ee_pong = smoke_env("TESSERACT_SMOKE_E2EE_PONG");

    bool known_phase() const
    {
        return phase == "fresh" || phase == "restore" || phase == "signed-out";
    }

    std::string missing() const
    {
        std::string out;
        auto check = [&out](const std::string& v, const char* name)
        {
            if (!v.empty())
            {
                return;
            }
            if (!out.empty())
            {
                out += ", ";
            }
            out += name;
        };
        if (phase == "fresh")
        {
            check(homeserver, "TESSERACT_SMOKE_HOMESERVER");
            check(user, "TESSERACT_SMOKE_USER");
            check(password, "TESSERACT_SMOKE_PASSWORD");
        }
        if (phase == "fresh" || phase == "restore")
        {
            check(room_id, "TESSERACT_SMOKE_ROOM_ID");
            check(ping, "TESSERACT_SMOKE_PING");
            check(pong, "TESSERACT_SMOKE_PONG");
        }
        if (phase == "fresh")
        {
            check(e2ee_room_id, "TESSERACT_SMOKE_E2EE_ROOM_ID");
            check(e2ee_pong, "TESSERACT_SMOKE_E2EE_PONG");
        }
        return out;
    }
};

struct SmokeFlags
{
    using Clock = std::chrono::steady_clock;

    // The LoginView each action was applied to: ensure_login_view_ can retire
    // and recreate it, so an action on a stale instance is redone on the new one.
    const views::LoginView* entered_on = nullptr;
    Clock::time_point entered_at;
    const views::LoginView* signed_in_on = nullptr;
    Clock::time_point signed_in_at;
    int sign_in_retries = 0;
    bool sign_out_started = false;
    bool sign_out_confirmed = false;
};

} // namespace

void ShellBase::smoke_observe_(const std::string& room_id, const Event& ev)
{
    if (!smoke_director_ || ev.body.empty())
    {
        return;
    }
    for (const auto& needle : smoke_needles_)
    {
        if (ev.body != needle)
        {
            continue;
        }
        smoke_seen_.push_back({room_id, ev.sender, ev.body, ev.event_id, ev.pending_state});
        return;
    }
}

void ShellBase::start_smoke_director_(smoke::SmokeHost& host, std::string result_path)
{
    auto login_view = [this]() -> views::LoginView*
    {
        return login_view_;
    };
    auto env = std::make_shared<SmokeEnv>();
    auto flags = std::make_shared<SmokeFlags>();

    std::vector<smoke::Step> steps;

    auto fail_step = [&steps](const char* name, std::string message)
    {
        steps.push_back({name,
                         []
                         {
                             return false;
                         },
                         0,
                         [message = std::move(message)]
                         {
                             return message;
                         }});
    };

    if (!env->known_phase())
    {
        fail_step("env", "unknown TESSERACT_SMOKE_PHASE '" + env->phase +
                             "' (want fresh, restore or signed-out)");
    }
    else if (const auto missing = env->missing(); !missing.empty())
    {
        fail_step("env", "missing " + missing);
    }
    else
    {
        // The encryption dialog (and the sync gate behind it) can open on a
        // fresh account; closing it is what "skip" does for a user.
        auto dismiss_encryption_dialog = [this]
        {
            auto* ov = main_app_ ? main_app_->encryption_setup() : nullptr;
            if (ov && ov->visible() && ov->on_close)
            {
                ov->on_close();
            }
        };

        auto find_seen = [this](const std::string& room, const std::string& body,
                                auto&& accept) -> const SmokeSeen*
        {
            for (const auto& s : smoke_seen_)
            {
                if (s.room_id == room && s.body == body && accept(s))
                {
                    return &s;
                }
            }
            return nullptr;
        };

        // room, open, [receive], send, echo for one room. `prefix` namespaces
        // the step names; an empty `ping` skips the receive step (the bot
        // cannot write into an encrypted room).
        auto add_room_steps = [this, &steps, dismiss_encryption_dialog,
                               find_seen](const std::string& prefix, const std::string& room_id,
                                          const std::string& ping, const std::string& pong)
        {
            steps.push_back({prefix + "room",
                             [this, room_id, dismiss_encryption_dialog]
                             {
                                 dismiss_encryption_dialog();
                                 for (const auto& r : rooms_)
                                 {
                                     if (r.id == room_id)
                                     {
                                         return true;
                                     }
                                 }
                                 return false;
                             },
                             90000,
                             [this, room_id]
                             {
                                 return std::to_string(rooms_.size()) + " rooms synced, " +
                                        room_id + " not among them";
                             }});

            steps.push_back({prefix + "open",
                             [this, room_id, dismiss_encryption_dialog]
                             {
                                 dismiss_encryption_dialog();
                                 if (current_room_id_ != room_id)
                                 {
                                     tab_select_room(room_id);
                                 }
                                 return current_room_id_ == room_id && room_view_ != nullptr;
                             },
                             20000,
                             [this]
                             {
                                 return "current room '" + current_room_id_ + "'";
                             }});

            if (!ping.empty())
            {
                steps.push_back({prefix + "receive",
                                 [this, room_id, ping, find_seen]
                                 {
                                     return find_seen(room_id, ping,
                                                      [this](const SmokeSeen& s)
                                                      {
                                                          return s.sender != my_user_id_;
                                                      }) != nullptr;
                                 },
                                 30000,
                                 [ping]
                                 {
                                     return "'" + ping + "' never reached the timeline";
                                 }});
            }

            steps.push_back({prefix + "send",
                             [this, pong]
                             {
                                 auto* cb = room_view_ ? room_view_->compose_bar() : nullptr;
                                 if (!cb)
                                 {
                                     return false;
                                 }
                                 cb->set_current_text(pong);
                                 cb->trigger_send();
                                 return true;
                             },
                             15000,
                             []
                             {
                                 return std::string("no compose bar");
                             }});

            steps.push_back(
                {prefix + "echo",
                 [this, room_id, pong, find_seen]
                 {
                     return find_seen(room_id, pong,
                                      [this](const SmokeSeen& s)
                                      {
                                          return s.sender == my_user_id_ &&
                                                 s.pending_state.empty() && !s.event_id.empty();
                                      }) != nullptr;
                 },
                 30000,
                 [this, room_id, pong]
                 {
                     for (auto it = smoke_seen_.rbegin(); it != smoke_seen_.rend(); ++it)
                     {
                         if (it->room_id == room_id && it->body == pong)
                         {
                             return "last state '" + it->pending_state + "'";
                         }
                     }
                     return std::string("send never appeared in the timeline");
                 }});
        };

        if (env->phase == "fresh")
        {
            smoke_needles_ = {env->ping, env->pong, env->e2ee_pong};

            steps.push_back(
                {"discover",
                 [login_view, env, flags]
                 {
                     auto* lv = login_view();
                     if (!lv)
                     {
                         return false;
                     }
                     // Enter the homeserver again on a rebuilt view, after a
                     // failed or never-started discovery (2 s), or when it is
                     // stuck in Discovering (10 s; hs_changed_ sets that state
                     // before bailing out without a client). Re-entering bumps
                     // the discovery generation, so the stale attempt is dropped.
                     using DS = views::LoginView::DiscoveryState;
                     const auto now = SmokeFlags::Clock::now();
                     const auto since = now - flags->entered_at;
                     const DS ds = lv->discovery_state();
                     const bool stalled =
                         ((ds == DS::Idle || ds == DS::Failed) &&
                          since >= std::chrono::seconds(2)) ||
                         (ds == DS::Discovering && since >= std::chrono::seconds(10));
                     if (flags->entered_on != lv || stalled)
                     {
                         flags->entered_on = lv;
                         flags->entered_at = now;
                         lv->smoke_enter_homeserver(env->homeserver);
                         return false;
                     }
                     return lv->discovery_state() == views::LoginView::DiscoveryState::Resolved &&
                            lv->homeserver_submit_action() ==
                                views::LoginView::SubmitAction::Password;
                 },
                 30000,
                 [login_view]
                 {
                     auto* lv = login_view();
                     if (!lv)
                     {
                         return std::string("login view never appeared");
                     }
                     return "discovery state " +
                            std::to_string(static_cast<int>(lv->discovery_state())) +
                            ", submit action " +
                            std::to_string(static_cast<int>(lv->homeserver_submit_action())) +
                            ", status '" + lv->smoke_status_text() + "'";
                 }});

            steps.push_back({"login",
                             [this, login_view, env, flags]
                             {
                                 if (!my_user_id_.empty())
                                 {
                                     return true;
                                 }
                                 auto* lv = login_view();
                                 if (!lv)
                                 {
                                     return false;
                                 }
                                 // Submit once per LoginView instance. A sign-in may
                                 // be in flight (a duplicate would create a second
                                 // device), so the only same-instance resubmit is
                                 // after it came back to the form with an error
                                 // status (State::Form; success stays Waiting).
                                 const auto now = SmokeFlags::Clock::now();
                                 const bool failed_back_to_form =
                                     flags->signed_in_on == lv &&
                                     lv->state() == views::LoginView::State::Form &&
                                     !lv->smoke_status_text().empty() &&
                                     now - flags->signed_in_at >= std::chrono::seconds(2) &&
                                     flags->sign_in_retries < 3;
                                 if (flags->signed_in_on != lv || failed_back_to_form)
                                 {
                                     if (failed_back_to_form)
                                     {
                                         ++flags->sign_in_retries;
                                     }
                                     flags->signed_in_on = lv;
                                     flags->signed_in_at = now;
                                     lv->smoke_sign_in(env->user, env->password);
                                 }
                                 return false;
                             },
                             60000,
                             [login_view]
                             {
                                 auto* lv = login_view();
                                 return lv ? "status '" + lv->smoke_status_text() + "'"
                                           : std::string("login view gone, no session");
                             }});

            add_room_steps("", env->room_id, env->ping, env->pong);
            add_room_steps("e2ee-", env->e2ee_room_id, {}, env->e2ee_pong);
        }
        else if (env->phase == "restore")
        {
            smoke_needles_ = {env->ping, env->pong};

            // No credentials in this phase: the stored session must be enough.
            steps.push_back({"restored",
                             [this]
                             {
                                 return !my_user_id_.empty();
                             },
                             60000,
                             [login_view]
                             {
                                 return std::string(login_view() ? "login view is showing"
                                                                 : "no login view") +
                                        ", no signed-in user";
                             }});

            add_room_steps("", env->room_id, env->ping, env->pong);

            steps.push_back(
                {"sign-out",
                 [this, flags]
                 {
                     if (!flags->sign_out_started)
                     {
                         flags->sign_out_started = true;
                         logout_active_account_ui_();
                     }
                     // An unsaved recovery key opens the save-key dialog instead;
                     // take the path of a user choosing "sign out anyway".
                     if (!flags->sign_out_confirmed && pending_sign_out_)
                     {
                         flags->sign_out_confirmed = true;
                         proceed_sign_out_();
                     }
                     return my_user_id_.empty() && !active_account_ && login_view_ != nullptr;
                 },
                 30000,
                 [this]
                 {
                     return "signed-in user '" + my_user_id_ + "', " +
                            (login_view_ ? "login view is showing" : "no login view") +
                            (pending_sign_out_ ? ", unsaved-key prompt still open" : "");
                 }});
        }
        else
        {
            steps.push_back({"no-session",
                             [this]
                             {
                                 return login_view_ != nullptr && my_user_id_.empty() &&
                                        !active_account_;
                             },
                             30000,
                             [this]
                             {
                                 return "signed-in user '" + my_user_id_ + "', " +
                                        (login_view_ ? "login view is showing" : "no login view");
                             }});

            // Passes after ~5 s of polls with still no session; a session
            // appearing meanwhile never passes, so the step times out.
            auto quiet = std::make_shared<std::pair<int, bool>>(0, false); // polls, saw a session
            steps.push_back({"stays-signed-out",
                             [this, quiet]
                             {
                                 if (!my_user_id_.empty() || active_account_)
                                 {
                                     quiet->second = true;
                                 }
                                 return !quiet->second && ++quiet->first >= 20;
                             },
                             15000,
                             [this]
                             {
                                 return "a session appeared: signed-in user '" + my_user_id_ + "'";
                             }});
        }
    }

    smoke_director_ = std::make_unique<smoke::SmokeDirector>(host, std::move(steps),
                                                             std::move(result_path), 250, 300000);
    smoke_director_->start();
}

} // namespace tesseract
