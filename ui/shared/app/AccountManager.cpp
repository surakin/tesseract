#include "AccountManager.h"
#include <algorithm>

namespace tesseract {

AccountManager::AccountManager()  = default;
AccountManager::~AccountManager() = default;

void AccountManager::add_account(std::shared_ptr<AccountSession> session)
{
    // Guard against double-adds: a secondary window must reuse the existing
    // live session, never append a second entry for the same user (which would
    // duplicate it in every account picker and start its sync twice). Keep the
    // already-registered session.
    if (session && find(session->user_id))
        return;
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
    if (!primary_window_)
        primary_window_ = w;
}

void AccountManager::unregister_window(ShellBase* w)
{
    all_windows_.erase(
        std::remove(all_windows_.begin(), all_windows_.end(), w),
        all_windows_.end());
    // Defensive: under hide-to-tray the startup window never unregisters, but if
    // the primary ever goes away, fall back to the oldest surviving window.
    if (primary_window_ == w)
        primary_window_ = all_windows_.empty() ? nullptr : all_windows_.front();
    if (tray_owner_ == w)
        tray_owner_ = nullptr;
}

ShellBase* AccountManager::primary_window() const
{
    return primary_window_;
}

bool AccountManager::claim_tray_owner(ShellBase* w)
{
    if (!tray_owner_)
        tray_owner_ = w;
    return tray_owner_ == w;
}

void AccountManager::release_tray_owner(ShellBase* w)
{
    if (tray_owner_ == w)
        tray_owner_ = nullptr;
}

bool AccountManager::is_tray_owner(ShellBase* w) const
{
    return tray_owner_ == w;
}

ShellBase* AccountManager::tray_owner() const
{
    return tray_owner_;
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
