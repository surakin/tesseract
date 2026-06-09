#include "app/ThreadPanelController.h"

namespace tesseract
{

ThreadPanelController::ThreadTransition
ThreadPanelController::compute_transition(ThreadPanel cur, ThreadPanel prev,
                                          const std::string& current_root,
                                          ThreadTrigger trigger,
                                          const std::string& trigger_root)
{
    ThreadTransition t;
    t.new_state = cur;
    t.new_prev  = prev;
    t.new_root  = current_root;

    switch (trigger)
    {
    case ThreadTrigger::ToggleList:
        if (cur == ThreadPanel::Closed)
        {
            t.new_state = ThreadPanel::List;
            t.new_prev  = ThreadPanel::Closed;
            t.new_root.clear();
            t.subscribe_room_threads_ = true;
        }
        else if (cur == ThreadPanel::List)
        {
            t.new_state = ThreadPanel::Closed;
            t.new_prev  = ThreadPanel::Closed;
            t.new_root.clear();
            t.unsubscribe_room_threads_ = true;
        }
        else // Open
        {
            // Toggle while a thread is open: close everything (matches
            // the spec — main button is a Closed<->List toggle and any
            // thread sub gets released).
            t.new_state = ThreadPanel::Closed;
            t.new_prev  = ThreadPanel::Closed;
            t.threads_to_unsubscribe.push_back(current_root);
            t.new_root.clear();
            if (prev == ThreadPanel::List)
                t.unsubscribe_room_threads_ = true;
        }
        break;

    case ThreadTrigger::OpenFromList:
        if (cur == ThreadPanel::Open)
            t.threads_to_unsubscribe.push_back(current_root);
        t.new_state = ThreadPanel::Open;
        t.new_prev  = ThreadPanel::List;
        t.new_root  = trigger_root;
        t.threads_to_subscribe.push_back(trigger_root);
        break;

    case ThreadTrigger::OpenFromMain:
        if (cur == ThreadPanel::Open)
            t.threads_to_unsubscribe.push_back(current_root);
        t.new_state = ThreadPanel::Open;
        t.new_prev  = (cur == ThreadPanel::List) ? ThreadPanel::List
                                                 : ThreadPanel::Closed;
        t.new_root  = trigger_root;
        t.threads_to_subscribe.push_back(trigger_root);
        break;

    case ThreadTrigger::CloseThread:
        if (cur != ThreadPanel::Open)
            break; // no-op
        t.threads_to_unsubscribe.push_back(current_root);
        t.new_state = prev; // back to whatever opened us
        t.new_prev  = ThreadPanel::Closed;
        t.new_root.clear();
        if (prev == ThreadPanel::Closed)
            t.unsubscribe_room_threads_ = false; // never subscribed
        break;

    case ThreadTrigger::RoomSwitch:
        if (cur == ThreadPanel::Open)
            t.threads_to_unsubscribe.push_back(current_root);
        // Always release the outgoing room's thread-list subscription. The
        // shell now keeps a background subscription on the active room (for
        // the threads-button visibility check) regardless of panel state, so
        // every room switch must clean it up — even when the panel was closed.
        // unsubscribe_room_threads is a no-op when no handle exists.
        t.unsubscribe_room_threads_ = true;
        t.new_state = ThreadPanel::Closed;
        t.new_prev  = ThreadPanel::Closed;
        t.new_root.clear();
        break;
    }
    return t;
}

bool ThreadPanelController::begin_paginate(bool can_paginate)
{
    if (!can_paginate || reached_start_ || paginating_)
        return false;
    paginating_ = true;
    if (run_paginate_)
        run_paginate_();
    return true;
}

bool ThreadPanelController::on_paginate_result(bool reached, bool want_more)
{
    paginating_ = false;
    if (reached)
    {
        reached_start_ = true;
        return false;
    }
    return want_more;
}

} // namespace tesseract
