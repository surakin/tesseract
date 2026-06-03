#pragma once
#include <gtk/gtk.h>
#include "app/RoomWindowBase.h"
#include "tk/host_gtk.h"
#include "views/MentionController.h"
#include "views/MentionPopup.h"
#include <memory>

namespace tesseract::views
{
class EmojiPicker;
class StickerPicker;
} // namespace tesseract::views

namespace gtk4
{
class MainWindow;
}

namespace gtk4
{

// A secondary (pop-out) room window for the GTK4 shell.
class RoomWindow : public tesseract::RoomWindowBase
{
public:
    RoomWindow(MainWindow* parent_shell, const std::string& room_id);
    ~RoomWindow() override;

    void bring_to_front() override;
    void close_window() override;
    void request_relayout() override;
    void update_window_title_(const std::string& name) override;
    void apply_theme(const tk::Theme& t) override;
    void repaint_anim_frame() override;

protected:
    void surface_repaint_() override;
    tk::NativeTextArea* compose_text_area_() override
    {
        return room_text_area_.get();
    }
    tk::EncodedImage encode_for_send_(const std::uint8_t* data,
                                      std::size_t size, bool compress) override
    {
        return surface_ ? surface_->host().encode_for_send(data, size, compress)
                        : tk::EncodedImage{};
    }

private:
    void show_mention_popup_(tk::Rect cursor_local, int rows);

    // Pop-out-local emoji / sticker pickers (GtkPopover parented to surface_).
    // The emoji picker doubles as the reaction picker via the pending id.
    void build_emoji_popover_();
    void build_sticker_popover_();
    void popup_emoji_at_rect_(tk::Rect anchor);
    void popup_sticker_at_rect_(tk::Rect anchor);

    static void on_destroy_(GtkWidget* widget, gpointer self);
    static gboolean on_key_pressed_(GtkEventControllerKey*, guint keyval,
                                    guint, GdkModifierType, gpointer self);
    // Global-scope Ctrl+K shortcut — routes to the main window's quick
    // switcher, bringing the main window forward (the switcher lives there).
    static gboolean on_quick_switch_shortcut_(GtkWidget*, GVariant*,
                                              gpointer self);

    static void on_copy_action_(GSimpleAction*, GVariant*, gpointer self);

    MainWindow* parent_shell_;
    GtkWindow* window_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> surface_;
    GtkWidget* copy_ctx_menu_ = nullptr;
    GSimpleActionGroup* copy_ctx_actions_ = nullptr;

    std::unique_ptr<tk::NativeTextArea> room_text_area_;
    GtkWidget* mention_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> mention_popup_surface_;
    tesseract::views::MentionPopup* mention_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::MentionController> mention_controller_;

    // Emoji / sticker pickers + their search overlays.
    GtkWidget* emoji_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> emoji_picker_surface_;
    tesseract::views::EmojiPicker* emoji_picker_shared_ = nullptr;
    std::unique_ptr<tk::NativeTextField> emoji_picker_search_field_;
    GtkWidget* sticker_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> sticker_picker_surface_;
    tesseract::views::StickerPicker* sticker_picker_shared_ = nullptr;
    std::unique_ptr<tk::NativeTextField> sticker_picker_search_field_;
    // Hover tooltip (reaction / read-receipt info).
    GtkWidget* topic_tooltip_popover_ = nullptr;
    GtkWidget* topic_tooltip_label_ = nullptr;
    // Reaction picker target: set by on_add_reaction_requested.
    std::string pending_reaction_event_id_;
};

} // namespace gtk4
