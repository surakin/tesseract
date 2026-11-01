#pragma once

// Settings panel section: "About" tab pinned to the bottom of the sidebar.
// Renders the standard brand splash (app icon, name, version) as the upper
// fill, with a "Storage" group below showing local-cache and SDK-store sizes
// plus a destructive "Clear all caches" button.

#include "SettingsPage.h"

#include <cstdint>
#include <functional>

namespace tesseract::views
{

class AboutSection : public SettingsPage
{
public:
    AboutSection();
    ~AboutSection() override = default;

    // Update displayed size values. Call on the UI thread after the shell's
    // async size-computation callback fires.
    void set_memory_cache_size(uint64_t bytes);
    void set_local_cache_size(uint64_t bytes);
    void set_sdk_store_size(uint64_t bytes);

    // Update hit/miss stats for the tooltip shown when hovering each row.
    // SDK store has no trackable stats so there is no set_sdk_store_stats().
    void set_memory_cache_stats(uint64_t hits, uint64_t misses);
    void set_local_cache_stats(uint64_t hits, uint64_t misses);

    // Fired when the user presses "Clear all caches". SettingsView wires this
    // to its shared ConfirmDialog before forwarding to the shell.
    std::function<void()> on_clear_caches;

    // Fired when the user presses "Advanced". SettingsView wires this to
    // reveal and navigate to the hidden Advanced tab.
    std::function<void()> on_advanced_clicked;

    // Hide the storage group (cache sizes + "Clear all caches") and the
    // "Advanced" button — both act on a signed-in account. Used while
    // Settings is open from the login screen.
    void set_account_actions_visible(bool visible);

private:
    class CacheSizeRow;
    tk::Widget* storage_row_ = nullptr;  // borrowed
    tk::Widget* advanced_row_ = nullptr; // borrowed
    CacheSizeRow* memory_row_ = nullptr; // borrowed
    CacheSizeRow* local_row_ = nullptr;  // borrowed
    CacheSizeRow* sdk_row_ = nullptr;    // borrowed
};

} // namespace tesseract::views
