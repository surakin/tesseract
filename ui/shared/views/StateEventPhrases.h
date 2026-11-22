#pragma once

// English phrases (through tk::tr/trn/trf) for generic state-event timeline rows.
// The Rust side sends only identifiers and parameters (see the wire format in the
// StateEvent doc comment in client/include/tesseract/types.h); this is the one place
// that turns a (state_type, state_change, state_params) triple into a sentence.

#include <cstddef>
#include <string>

namespace tesseract::views
{
struct MessageRowData;

// One event as a standalone sentence, e.g. "Alice made the room invite-only".
// Unknown types or changes read "Alice updated the room state: <type>".
std::string state_event_phrase(const MessageRowData& m);

// A collapsed run of `count` events of `state_type`, e.g. "Alice changed the join rules
// (3 times)". `sender_label` may be empty (several senders); `last` is the newest event.
std::string state_event_run_phrase(const std::string& sender_label, const std::string& state_type,
                                   std::size_t count, const MessageRowData& last);

// One summary line for `count` state events of two or more types in a group, e.g.
// "Alice updated the room settings (5 changes)". `sender_label` may be empty (several senders).
std::string state_summary_run_phrase(const std::string& sender_label, std::size_t count);

// Label for a state type Tesseract reads into room state instead of rendering as a row
// (`body` is "type" or "type [state_key]"), or empty. Used by developer rows.
std::string known_state_label(const std::string& body);
} // namespace tesseract::views
