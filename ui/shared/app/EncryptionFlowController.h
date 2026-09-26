#pragma once

// State + decisions behind the single encryption dialog
// (views::EncryptionSetupOverlay) and its reminder strip
// (views::EncryptionReminderBanner). Pure — no client, no widgets — so the
// routing rules are unit-testable; ShellBase owns an instance and applies
// the effects (see ShellBase's "Encryption flow" section).

#include <cstdint>
#include <string>

namespace tesseract
{

class EncryptionFlowController
{
public:
    // ── The interactive (SAS) verification in progress, if any ─────────────
    struct Flow
    {
        std::string id;
        std::string user_id;   // the other party (our own id for self-verification)
        std::string device_id; // the other device, when known
        bool incoming = false; // they asked us
        bool own_user = true;  // one of our own devices, not another user
        // Whether this device still needed confirming when the flow began:
        // decides what the Done step says (unlocked vs. other device confirmed).
        bool this_device_unverified = false;
    };

    bool        has_flow() const { return !flow_.id.empty(); }
    const Flow& flow() const { return flow_; }
    bool        is_flow(const std::string& id) const { return has_flow() && flow_.id == id; }
    void        begin(Flow f) { flow_ = std::move(f); awaiting_outgoing_ = false; }
    void        clear() { flow_ = {}; awaiting_outgoing_ = false; }

    // request_self_verification() doesn't return a flow id; the id arrives
    // later via on_verification_request(incoming=false). While this is true a
    // Ready for a flow we started belongs to the dialog; once the user has
    // cancelled, a late Ready must be cancelled instead of adopted.
    bool awaiting_outgoing() const { return awaiting_outgoing_; }
    void set_awaiting_outgoing(bool v) { awaiting_outgoing_ = v; }

    // ── Incoming requests ───────────────────────────────────────────────────
    enum class IncomingAction
    {
        Show,       // open / switch the dialog to "Was this you?"
        RefuseBusy, // the dialog is mid-setup (e.g. showing the new recovery key)
    };
    static IncomingAction on_incoming(bool dialog_visible, bool dialog_busy)
    {
        return dialog_visible && dialog_busy ? IncomingAction::RefuseBusy
                                             : IncomingAction::Show;
    }

    // ── Reminder strip ──────────────────────────────────────────────────────
    enum class Reminder
    {
        None,
        SetupNeeded, // no recovery for the account yet
        Locked,      // this device can't read encrypted messages yet
    };
    // `recovery_state`: 0=Unknown, 1=Disabled, 2=Enabled, 3=Incomplete.
    static Reminder reminder_for(std::uint8_t recovery_state, bool device_verified,
                                 bool foreign_identity);

    static constexpr std::int64_t kSnoozeSeconds = 3 * 24 * 60 * 60;
    static bool snoozed(std::int64_t snoozed_until_s, std::int64_t now_s)
    {
        return now_s < snoozed_until_s;
    }

private:
    Flow flow_;
    bool awaiting_outgoing_ = false;
};

} // namespace tesseract
