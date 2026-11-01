#pragma once
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace tesseract::views::send_block
{

// A failed send can be parked by an encryption check (`pending_error` is then
// one of these codes, see sdk/src/client/timeline_convert.rs). They are
// language-neutral; this turns them into text for the tooltip, the screen
// reader and the Retry dialog.

/// True for the codes the SDK uses for an encryption check (not for a plain
/// error message).
bool is_block_code(std::string_view code);

/// True when removing the named users' verification clears the block. Only an
/// identity change: for an unsigned device the user stays verified by our
/// signature, so only its owner can resolve it.
bool can_remove_verification(std::string_view code);

/// What Retry does for a blocked message. The block setting only applies
/// after a restart, so `applied` (what this run uses) can differ from
/// `setting_on` (what is saved).
enum class RetryAction
{
    Explain,       ///< still blocked: show the explanation dialog
    RetryDirectly, ///< the check is off in this run: just retry
    NeedsRestart,  ///< turned off but not applied yet: say so
};
RetryAction retry_action(std::string_view code, bool setting_on, bool applied);

using NameOf = std::function<std::string(const std::string& user_id)>;

/// One-line reason, e.g. "Alice has a device that isn't confirmed by its
/// owner". `name_of` may be empty (or return an empty string) to fall back to
/// the user id. Empty `code` or an unknown code gives an empty string.
std::string reason(std::string_view code, const std::vector<std::string>& users,
                   const NameOf& name_of);

/// Text for the Retry dialog: the reason plus what removing verification does.
std::string explanation(std::string_view code, const std::vector<std::string>& users,
                        const NameOf& name_of);

} // namespace tesseract::views::send_block
