#include "AccountManager.h"
#include <algorithm>

namespace tesseract {

AccountManager::AccountManager()  = default;
AccountManager::~AccountManager() = default;

void AccountManager::add_account(std::shared_ptr<AccountSession> session)
{
    accounts_.push_back(std::move(session));
}

void AccountManager::remove_account(std::string_view user_id)
{
    accounts_.erase(
        std::remove_if(accounts_.begin(), accounts_.end(),
                       [&](const auto& s) { return s->user_id == user_id; }),
        accounts_.end());
}

std::shared_ptr<AccountSession> AccountManager::find(std::string_view user_id) const
{
    for (const auto& s : accounts_)
        if (s->user_id == user_id)
            return s;
    return nullptr;
}

std::span<std::shared_ptr<AccountSession> const> AccountManager::accounts() const
{
    return accounts_;
}

void AccountManager::register_window(ShellBase* w)
{
    all_windows_.push_back(w);
}

void AccountManager::unregister_window(ShellBase* w)
{
    all_windows_.erase(
        std::remove(all_windows_.begin(), all_windows_.end(), w),
        all_windows_.end());
}

void AccountManager::set_dedicated(std::string_view user_id, ShellBase* w)
{
    dedicated_windows_[std::string(user_id)] = w;
}

void AccountManager::clear_dedicated(std::string_view user_id)
{
    dedicated_windows_.erase(std::string(user_id));
}

ShellBase* AccountManager::dedicated_window(std::string_view user_id) const
{
    auto it = dedicated_windows_.find(std::string(user_id));
    return it != dedicated_windows_.end() ? it->second : nullptr;
}

int AccountManager::window_count() const
{
    return static_cast<int>(all_windows_.size());
}

std::span<ShellBase* const> AccountManager::all_windows() const
{
    return all_windows_;
}

} // namespace tesseract
