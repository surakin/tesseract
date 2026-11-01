#include "SendPipeline.h"

#include <cstdio>
#include <exception>
#include <utility>

namespace tesseract
{

SendPipeline::SendPipeline(Hooks hooks) : hooks_(std::move(hooks))
{
}

void SendPipeline::submit(const std::string& room_id, Prepare prepare, Send send)
{
    auto& room = rooms_[room_id];

    if (room.outstanding++ == 0)
    {
        // First send of a burst: arm the busy-delay timer for this burst.
        room.epoch = next_epoch_++;
        if (hooks_.post_ui_after)
        {
            const std::uint64_t epoch = room.epoch;
            hooks_.post_ui_after(kBusyDelayMs,
                                 [this, room_id, epoch]
                                 {
                                     auto it = rooms_.find(room_id);
                                     if (it == rooms_.end() || it->second.epoch != epoch ||
                                         it->second.outstanding == 0 || it->second.busy)
                                     {
                                         return;
                                     }
                                     it->second.busy = true;
                                     if (hooks_.on_busy_changed)
                                     {
                                         hooks_.on_busy_changed(room_id, true);
                                     }
                                 });
        }
    }

    Entry entry;
    entry.id = next_id_++;
    entry.send = std::move(send);
    entry.ready = !prepare;
    const std::uint64_t id = entry.id;
    room.queue.push_back(std::move(entry));

    if (!prepare)
    {
        pump_(room_id);
        return;
    }
    // The worker only touches its own copies; the result comes back to the
    // UI thread through post_ui.
    auto post_ui = hooks_.post_ui;
    hooks_.run_prepare(
        [this, post_ui, room_id, id, prepare = std::move(prepare)]
        {
            std::string prepared = prepare();
            post_ui(
                [this, room_id, id, prepared = std::move(prepared)]() mutable
                {
                    on_prepared_(room_id, id, std::move(prepared));
                });
        });
}

void SendPipeline::on_prepared_(const std::string& room_id, std::uint64_t id, std::string prepared)
{
    auto it = rooms_.find(room_id);
    if (it == rooms_.end())
    {
        return;
    }
    for (auto& e : it->second.queue)
    {
        if (e.id == id)
        {
            e.ready = true;
            e.prepared = std::move(prepared);
            break;
        }
    }
    pump_(room_id);
}

void SendPipeline::pump_(const std::string& room_id)
{
    auto it = rooms_.find(room_id);
    if (it == rooms_.end())
    {
        return;
    }
    auto& queue = it->second.queue;
    while (!queue.empty() && queue.front().ready)
    {
        Entry e = std::move(queue.front());
        queue.pop_front();
        auto post_ui = hooks_.post_ui;
        hooks_.run_send(
            [this, post_ui, room_id, send = std::move(e.send), prepared = std::move(e.prepared)]
            {
                // A throwing send still counts as done: the room's queue must not stall
                // with this entry outstanding for good.
                if (send)
                {
                    try
                    {
                        send(prepared);
                    }
                    catch (const std::exception& ex)
                    {
                        std::fprintf(stderr, "[send] send failed: %s\n", ex.what());
                    }
                    catch (...)
                    {
                        std::fprintf(stderr, "[send] send failed\n");
                    }
                }
                post_ui(
                    [this, room_id]
                    {
                        on_sent_(room_id);
                    });
            });
    }
}

void SendPipeline::on_sent_(const std::string& room_id)
{
    auto it = rooms_.find(room_id);
    if (it == rooms_.end() || it->second.outstanding == 0)
    {
        return;
    }
    if (--it->second.outstanding > 0)
    {
        return;
    }
    // Idle: every submitted send has been sent, so the queue is empty too.
    const bool was_busy = it->second.busy;
    rooms_.erase(it);
    if (was_busy && hooks_.on_busy_changed)
    {
        hooks_.on_busy_changed(room_id, false);
    }
}

bool SendPipeline::is_busy(const std::string& room_id) const
{
    auto it = rooms_.find(room_id);
    return it != rooms_.end() && it->second.busy;
}

std::size_t SendPipeline::outstanding(const std::string& room_id) const
{
    auto it = rooms_.find(room_id);
    return it == rooms_.end() ? 0 : it->second.outstanding;
}

} // namespace tesseract
