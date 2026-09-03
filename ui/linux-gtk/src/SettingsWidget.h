#pragma once
#include <gtk/gtk.h>
#include <tesseract/settings.h>
#include <cstdint>
#include <memory>
#include <string>
#include "app/SettingsController.h"
#include "tk/host.h"
#include "tk/host_gtk.h"
#include "views/settings/AccountSection.h"
#include "views/SettingsView.h"

namespace gtk4
{

class SettingsWidget
{
public:
    SettingsWidget();
    ~SettingsWidget() = default;

    SettingsWidget(const SettingsWidget&) = delete;
    SettingsWidget& operator=(const SettingsWidget&) = delete;

    // Returns the GtkWidget* to add to GtkStack (the surface widget).
    GtkWidget* widget() const;

    // Apply a new theme to the surface.
    void set_theme(const tk::Theme& t);

    // Push current account info and settings into the view before showing.
    void populate(std::string display_name, std::string user_id,
                  std::string avatar_mxc,
                  tesseract::views::AccountSection::ImageProvider provider);

    // The hosted shared SettingsView (borrowed). Used by the shell to push
    // search-index stats.
    tesseract::views::SettingsView* settings_view() const { return settings_view_; }

    // Forward server capability info into the shared SettingsView.
    void set_server_info(const tesseract::ServerInfo& info);

    // Push the fetched extended profile into the Account section.
    void set_extended_profile(const tesseract::ExtendedProfile& profile);

    // Mark a profile field as busy (in-flight write) or idle.
    void set_profile_field_busy(const std::string& key, bool busy);

    // Show an inline error for the given profile field. Pass "" to clear.
    void set_profile_field_error(const std::string& key, std::string error);

    // Update the Storage size labels and hit/miss stats in the About section.
    void set_cache_sizes(uint64_t local_bytes, uint64_t sdk_bytes,
                         uint64_t memory_bytes,
                         uint64_t mem_hits   = 0, uint64_t mem_misses  = 0,
                         uint64_t disk_hits  = 0, uint64_t disk_misses = 0);

    // Wire the SettingsController.
    void set_controller(tesseract::SettingsController* ctrl);

    // Invalidate this widget's own Surface. Passed by the shell as the
    // `relayout` callback to ShellBase::wire_settings_controller_common_.
    void relayout();

    // Repaint the surface on every ~60Hz animation tick while this widget is
    // visible, so animated stickers in the Emojis & Stickers tab advance
    // without needing mouse movement — mirrors
    // sticker_picker_surface_->relayout()'s brute-force-repaint idiom (GTK4's
    // partial-region AnimDamageSink path isn't wired up for this view; see
    // ImagePackTileGridBase, which doesn't call ctx.anim_damage->note_image()).
    void update_anim_regions();

    // Repaint the settings surface (no relayout) so newly-decoded static
    // images (e.g. Emojis & Stickers pack thumbnails) become visible
    // without waiting for an unrelated relayout/resize — see
    // MainWindow::on_media_bytes_ready_.
    void request_repaint();

private:
    std::unique_ptr<tk::gtk4::Surface> surface_;
    tesseract::views::SettingsView* settings_view_ = nullptr; // borrowed
    tesseract::SettingsController* controller_ = nullptr;
    // The name/pronouns/timezone/bio fields are self-owned by AccountSection
    // — see AccountSection::name_field()/pronouns_editor()/tz_field()/
    // bio_field() — so no member is needed for them here.
};

} // namespace gtk4
