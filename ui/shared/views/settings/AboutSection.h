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
    void set_local_cache_size(uint64_t bytes);
    void set_sdk_store_size(uint64_t bytes);

    // Fired when the user presses "Clear all caches". SettingsView wires this
    // to its shared ConfirmDialog before forwarding to the shell.
    std::function<void()> on_clear_caches;

private:
    class CacheSizeRow;
    CacheSizeRow* local_row_ = nullptr; // borrowed
    CacheSizeRow* sdk_row_   = nullptr; // borrowed
};

} // namespace tesseract::views
