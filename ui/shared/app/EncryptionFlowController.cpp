#include "app/EncryptionFlowController.h"

namespace tesseract
{

EncryptionFlowController::Reminder
EncryptionFlowController::reminder_for(std::uint8_t recovery_state, bool device_verified,
                                       bool foreign_identity, bool key_unsaved)
{
    // An identity made elsewhere that this device hasn't been confirmed
    // against: history is unreadable here until the user unlocks it.
    if (foreign_identity && !device_verified)
    {
        return Reminder::Locked;
    }
    switch (recovery_state)
    {
    case 1: // Disabled
        // A confirmed device on an identity made elsewhere can't create
        // the account's recovery from here (it lacks the private keys),
        // and it can read its messages, so there's nothing to nag about.
        return foreign_identity ? Reminder::None : Reminder::SetupNeeded;
    case 3: // Incomplete — recovery exists but this device lacks its secrets.
        // Right after an emoji verification this device is confirmed but
        // the other device's secrets are still in flight; don't call a
        // confirmed device "locked".
        return device_verified ? Reminder::None : Reminder::Locked;
    case 2: // Enabled
        return key_unsaved ? Reminder::SaveKey : Reminder::None;
    default: // Unknown (not decided yet)
        return Reminder::None;
    }
}

std::int64_t EncryptionFlowController::save_key_snooze_seconds(int dismissals)
{
    if (dismissals <= 1)
    {
        return 1 * kDaySeconds;
    }
    if (dismissals == 2)
    {
        return 3 * kDaySeconds;
    }
    if (dismissals == 3)
    {
        return 7 * kDaySeconds;
    }
    return 14 * kDaySeconds;
}

} // namespace tesseract
