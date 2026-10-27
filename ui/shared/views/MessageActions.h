#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace tesseract::views
{

// The one description of what can be done to a timeline message. The hover
// action pill, the "more" overflow, the right-click menu and the
// accessibility "Message actions" group all render the list built here, so a
// new action or a changed availability rule lands in every surface at once.
enum class MessageActionId
{
    React,
    Reply,
    ReplyInThread,
    Edit,
    Forward,
    Pin,
    Unpin,
    Delete,
    Report,
    CopyEventSource,
};

struct MessageAction
{
    MessageActionId id;
    std::string label;
    std::span<const std::uint8_t> svg;
    bool destructive = false;
    // Gets its own cell in the hover pill; the rest live behind the pill's
    // "more" cell. The right-click menu lists every action either way.
    bool in_pill = false;
};

// Row/room facts the availability rules read.
struct MessageActionInputs
{
    bool redacted = false;
    bool utd = false;
    bool poll = false;
    bool editable_kind = false;
    bool is_own = false;
    bool pending = false;           // local echo not yet confirmed
    bool can_thread = false;        // is a thread root or not yet in a thread
    bool thread_button_visible = false;
    bool can_redact_others = false;
    bool can_pin = false;
    bool is_pinned = false;
    bool developer_mode = false;
};

// Pill actions first (display order), then overflow actions.
std::vector<MessageAction> build_message_actions(const MessageActionInputs& in);

} // namespace tesseract::views
