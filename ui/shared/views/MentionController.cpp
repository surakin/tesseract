#include "views/MentionController.h"

#include <tesseract/client.h>

#include <algorithm>
#include <utility>

namespace tesseract::views
{

MentionController::MentionController(tk::NativeTextArea* text_area,
                                    tesseract::Client* client,
                                    MentionPopup* popup, Hooks hooks)
    : text_area_(text_area), client_(client), popup_(popup),
      hooks_(std::move(hooks))
{
    if (popup_)
    {
        popup_->on_accepted = [this](MentionCandidate c) { accept(c); };
        popup_->on_dismissed = [this] { hide(); };
    }
}

MentionController::~MentionController()
{
    *alive_ = false;
}

bool MentionController::on_text_changed(const std::string& text, int cursor)
{
    if (!text_area_ || !popup_)
    {
        return false;
    }
    auto m = engine_.find_prefix(text, cursor);
    if (!m)
    {
        if (visible_)
        {
            hide();
        }
        return false;
    }

    std::string rid = hooks_.room_id ? hooks_.room_id() : std::string{};

    // Member list must be fetched off the UI thread (get_room_members blocks).
    // When the cache is stale, fetch async and re-run when results arrive.
    if (cached_members_room_ != rid)
    {
        if (fetching_room_ != rid && client_ && hooks_.run_async &&
            hooks_.post_to_ui)
        {
            fetching_room_ = rid;
            auto* c = client_;
            auto alive = alive_;
            hooks_.run_async(
                [this, c, rid, alive]
                {
                    auto members = c->get_room_members(rid);
                    hooks_.post_to_ui(
                        [this, rid, alive, members = std::move(members)]() mutable
                        {
                            if (!*alive)
                            {
                                return;
                            }
                            cached_members_ = std::move(members);
                            cached_members_room_ = rid;
                            fetching_room_.clear();
                            on_text_changed(text_area_->text(),
                                            text_area_->cursor_byte_pos());
                        });
                });
        }
        return true; // handled (fetch pending) — popup appears on next tick
    }

    candidates_ = engine_.lookup(m->prefix, cached_members_, 8, true);
    if (candidates_.empty())
    {
        if (visible_)
        {
            hide();
        }
        return false;
    }
    active_match_ = *m;
    popup_->set_candidates(candidates_);
    if (hooks_.show)
    {
        hooks_.show(text_area_->cursor_rect(), popup_->visible_rows());
    }
    visible_ = true;
    return true;
}

bool MentionController::on_nav(tk::NativeTextArea::NavKey nk)
{
    if (!visible_ || !popup_)
    {
        return false;
    }
    int cur = popup_->selected_index();
    int n = popup_->visible_rows();
    if (n <= 0)
    {
        return true;
    }
    int next = cur;
    switch (nk)
    {
    case tk::NativeTextArea::NavKey::Up:
        next = std::max(0, cur - 1);
        break;
    case tk::NativeTextArea::NavKey::Down:
        next = std::min(n - 1, cur + 1);
        break;
    case tk::NativeTextArea::NavKey::Tab:
    {
        int sel = popup_->selected_index();
        if (sel >= 0 && sel < (int)candidates_.size())
        {
            accept(candidates_[sel]);
        }
        else
        {
            hide();
        }
        return true;
    }
    case tk::NativeTextArea::NavKey::ShiftTab:
        return false;
    case tk::NativeTextArea::NavKey::Escape:
        hide();
        return true;
    }
    popup_->set_selected_index(next);
    if (hooks_.repaint)
    {
        hooks_.repaint();
    }
    return true;
}

bool MentionController::on_submit()
{
    if (!visible_)
    {
        return false;
    }
    int sel = popup_ ? popup_->selected_index() : -1;
    if (sel >= 0 && sel < (int)candidates_.size())
    {
        accept(candidates_[sel]);
        return true;
    }
    hide();
    return false;
}

void MentionController::accept(const MentionCandidate& c)
{
    if (text_area_)
    {
        text_area_->insert_mention(active_match_.start, active_match_.end,
                                   c.user_id, c.display_name, c.is_room);
    }
    hide();
}

void MentionController::hide()
{
    if (!visible_)
    {
        return;
    }
    visible_ = false;
    if (hooks_.hide)
    {
        hooks_.hide();
    }
}

} // namespace tesseract::views
