#include "MessageActions.h"
#include "icons.h"
#include "tk/i18n.h"

namespace tesseract::views
{

std::vector<MessageAction> build_message_actions(const MessageActionInputs& in)
{
    std::vector<MessageAction> out;
    const bool reactable = !in.redacted && !in.utd;

    if (reactable)
        out.push_back({MessageActionId::React, tk::tr("Add reaction"), kEmojiSvg,
                       false, true});
    out.push_back({MessageActionId::Reply, tk::tr("Reply"), kReplySvg, false, true});
    // A plain reply can still become a thread root; only a message already
    // inside a thread can't start a nested one.
    if (!in.redacted && in.thread_button_visible && in.can_thread)
        out.push_back({MessageActionId::ReplyInThread, tk::tr("Reply in thread"),
                       kThreadSvg, false, true});
    // Receivers ignore edits of a forward (MSC4553), so don't offer one.
    if (in.is_own && in.editable_kind && !in.forwarded)
        out.push_back({MessageActionId::Edit, tk::tr("Edit"), kEditSvg, false, true});

    if (!reactable)
        return out;

    // A poll can't be forwarded (it would re-send as an unrelated poll).
    if (!in.poll && !in.pending)
        out.push_back({MessageActionId::Forward, tk::tr("Forward message"),
                       kForwardSvg});
    if (in.is_own || in.can_redact_others)
        out.push_back({MessageActionId::Delete, tk::tr("Delete message"), kRedactSvg,
                       true});
    // Only other people's confirmed messages can be reported (own messages
    // have nothing to report; a local echo has no server event yet).
    if (!in.is_own && !in.pending)
        out.push_back({MessageActionId::Report, tk::tr("Report message"),
                       kShieldAlertSvg, true});
    if (in.can_pin)
        out.push_back({in.is_pinned ? MessageActionId::Unpin : MessageActionId::Pin,
                       in.is_pinned ? tk::tr("Unpin message") : tk::tr("Pin message"),
                       kPinSvg});
    if (in.developer_mode)
        out.push_back({MessageActionId::CopyEventSource,
                       tk::tr("Copy event source"), kCopySvg});
    return out;
}

} // namespace tesseract::views
