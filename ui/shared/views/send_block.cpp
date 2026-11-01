#include "send_block.h"

#include "tk/i18n.h"

namespace tesseract::views::send_block
{

namespace
{

std::string label_for(const std::string& user_id, const NameOf& name_of)
{
    if (name_of)
    {
        std::string n = name_of(user_id);
        if (!n.empty())
        {
            return n;
        }
    }
    return user_id;
}

// "Alice", "Alice and Bob", "Alice, Bob and 2 others".
std::string names(const std::vector<std::string>& users, const NameOf& name_of)
{
    if (users.empty())
    {
        return tk::tr("someone in this room");
    }
    if (users.size() == 1)
    {
        return label_for(users[0], name_of);
    }
    if (users.size() == 2)
    {
        return tk::trf(tk::tr("{0} and {1}"),
                       {label_for(users[0], name_of), label_for(users[1], name_of)});
    }
    return tk::trf(tk::trn("{0}, {1} and {2} other", "{0}, {1} and {2} others",
                           static_cast<long>(users.size() - 2)),
                   {label_for(users[0], name_of), label_for(users[1], name_of),
                    std::to_string(users.size() - 2)});
}

long count(const std::vector<std::string>& users)
{
    // No named user reads as the singular "someone".
    return users.empty() ? 1 : static_cast<long>(users.size());
}

} // namespace

bool is_block_code(std::string_view code)
{
    return code == "identity_violation" || code == "insecure_devices" ||
           code == "own_verification_required";
}

bool can_remove_verification(std::string_view code)
{
    return code == "identity_violation";
}

RetryAction retry_action(std::string_view code, bool setting_on, bool applied)
{
    // Only these two come from the setting (the third is about this device).
    if (code != "identity_violation" && code != "insecure_devices")
    {
        return RetryAction::Explain;
    }
    if (!applied)
    {
        return RetryAction::RetryDirectly;
    }
    return setting_on ? RetryAction::Explain : RetryAction::NeedsRestart;
}

std::string reason(std::string_view code, const std::vector<std::string>& users,
                   const NameOf& name_of)
{
    if (code == "identity_violation")
    {
        return tk::trf(tk::trn("{0}'s digital identity was reset",
                               "The digital identities of {0} were reset", count(users)),
                       {names(users, name_of)});
    }
    if (code == "insecure_devices")
    {
        return tk::trf(tk::trn("{0} has a device that isn't confirmed by its owner",
                               "{0} have devices that aren't confirmed by their owners",
                               count(users)),
                       {names(users, name_of)});
    }
    if (code == "own_verification_required")
    {
        return tk::tr("This device must be confirmed before it can send messages");
    }
    return {};
}

std::string explanation(std::string_view code, const std::vector<std::string>& users,
                        const NameOf& name_of)
{
    std::string r = reason(code, users, name_of);
    if (r.empty())
    {
        return r;
    }
    if (can_remove_verification(code))
    {
        return tk::trf(tk::tr("{0}, so the message wasn't sent. You can withdraw your "
                              "verification, which lets it go through; you can verify them "
                              "again later from their profile."),
                       {r});
    }
    if (code == "insecure_devices")
    {
        return tk::trf(tk::tr("{0}, so the message wasn't sent. It can be sent once they confirm "
                              "or sign out that device. You can also discard the message, or turn "
                              "off this check in Settings."),
                       {r});
    }
    return tk::trf(tk::tr("{0}, so the message wasn't sent."), {r});
}

} // namespace tesseract::views::send_block
