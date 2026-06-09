#pragma once

// ReadReceiptTracker — read-receipt NOTIFY de-dup state extracted from
// MessageListView. Owns only the guard against re-firing on_receipt_needed for
// an unchanged newest-visible event: last_receipt_event_id_.
//
// The "newest visible real event id" computation stays in MessageListView
// (newest_visible_real_event_id() needs ListView's visible_range() + the
// messages_ vector). MessageListView computes that id, then asks the tracker
// whether it changed since the last fire (should_fire) and, if it fires,
// records it (mark). Behavior is identical to the original inline guard.
//
// NOTE: the receipt-disc PAINTING (hovered_row_geom_.receipt_discs, interleaved
// with the chip/avatar strip in Adapter::paint_row) is NOT part of this — it
// stays entangled in MessageListView. This collaborator owns only the notify
// de-dup state.

#include <string>

namespace tesseract::views
{

class ReadReceiptTracker
{
public:
    // True iff `newest` is non-empty and differs from the last fired id.
    bool should_fire(const std::string& newest) const
    {
        return !newest.empty() && newest != last_event_id_;
    }

    // Record `newest` as the most recently fired receipt target.
    void mark(const std::string& newest) { last_event_id_ = newest; }

private:
    std::string last_event_id_;
};

} // namespace tesseract::views
