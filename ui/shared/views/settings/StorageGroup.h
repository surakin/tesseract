#pragma once

// Settings group: "Storage" — local-cache and SDK-store sizes (with hit/miss
// stats in a tooltip) plus a destructive "Clear all caches" button. Hosted by
// GeneralSection, which wraps it in an HBox so it keeps its natural width
// instead of stretching to fill the page.

#include "SettingsGroup.h"

#include <cstdint>
#include <functional>

namespace tesseract::views
{

class StorageGroup : public SettingsGroup
{
public:
    StorageGroup();
    ~StorageGroup() override = default;

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

private:
    class CacheSizeRow;
    CacheSizeRow* memory_row_ = nullptr; // borrowed
    CacheSizeRow* local_row_ = nullptr;  // borrowed
    CacheSizeRow* sdk_row_ = nullptr;    // borrowed
};

} // namespace tesseract::views
