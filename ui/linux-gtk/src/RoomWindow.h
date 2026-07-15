#pragma once
#include <gtk/gtk.h>
#include "app/RoomWindowBase.h"
#include "tk/host_gtk.h"
#include "views/ConfirmDialog.h"
#include "views/ForwardRoomPicker.h"
#include "views/GifController.h"
#include "views/RoomMediaView.h"
#include "views/GifPopup.h"
#include "views/MentionController.h"
#include "views/MentionPopup.h"
#include "views/ShortcodeController.h"
#include "views/ShortcodePopup.h"
#include "views/SlashCommandController.h"
#include "views/SlashCommandPopup.h"
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
    // Fan-in for async GIF search results (forwarded by ShellBase to every
    // pop-out; only the controller that issued the search matches).
    void on_gif_results(std::uint64_t request_id,
                        std::vector<tesseract::GifResult> results) override;
    void on_gif_search_failed(std::uint64_t request_id,
                              const std::string& message) override;
    // compose_text_area_() uses RoomWindowBase's default (self-owned via
    // room_view_->compose_bar()->text_area()) — no override needed.
    tesseract::views::ForwardRoomPicker* forward_picker_() override
    {
        return forward_picker_widget_;
    }
    tesseract::views::RoomMediaView* room_media_view_() override
    {
        return room_media_view_widget_;
    }
    void focus_forward_picker_field_() override
    {
        if (!forward_picker_widget_)
            return;
        if (auto* f = forward_picker_widget_->search_field())
        {
            f->set_text("");
            f->set_focused(true);
        }
    }
    void hide_forward_picker_field_() override
    {
        if (forward_picker_widget_)
            if (auto* f = forward_picker_widget_->search_field())
                f->set_visible(false);
    }
    tk::EncodedImage encode_for_send_(const std::uint8_t* data,
                                      std::size_t size, bool compress) override
    {
        return surface_ ? surface_->host().encode_for_send(data, size, compress)
                        : tk::EncodedImage{};
    }
    bool
    put_image_on_clipboard_(std::span<const std::uint8_t> bytes) override
    {
        return surface_ && surface_->host().set_clipboard_image(bytes);
    }
    void post_delayed_(int ms, std::function<void()> fn) override
    {
        if (surface_)
            surface_->host().post_delayed(ms, std::move(fn));
    }

private:
    void show_mention_popup_(tk::Rect cursor_local, int rows);
    void show_slash_popup_(tk::Rect cursor_local, int rows);
    void show_shortcode_popup_(tk::Rect cursor_local, int rows);
    void show_gif_popup_();
    void hide_gif_popup_();

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

    // Borrowed from room_view_->compose_bar()->text_area() — see
    // compose_text_area_(). Search fields are self-owned too — see
    // RoomSearchBar::search_field() / ForwardRoomPicker::search_field().
    tk::TextArea* room_text_area_ = nullptr;
    tesseract::views::ForwardRoomPicker* forward_picker_widget_ = nullptr; // borrowed
    tesseract::views::RoomMediaView* room_media_view_widget_ = nullptr; // borrowed
    tesseract::views::ConfirmDialog* confirm_dialog_widget_ = nullptr; // borrowed
    GtkWidget* mention_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> mention_popup_surface_;
    tesseract::views::MentionPopup* mention_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::MentionController> mention_controller_;

    GtkWidget* slash_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> slash_popup_surface_;
    tesseract::views::SlashCommandPopup* slash_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::SlashCommandController> slash_controller_;

    GtkWidget* shortcode_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> shortcode_popup_surface_;
    tesseract::views::ShortcodePopup* shortcode_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::ShortcodeController> shortcode_controller_;

    GtkWidget* gif_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> gif_popup_surface_;
    tesseract::views::GifPopup* gif_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::GifController> gif_controller_;

    // Emoji / sticker pickers. Each picker's search field is self-owned
    // (see tesseract::views::TabbedGridPicker::search_field()).
    GtkWidget* emoji_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> emoji_picker_surface_;
    tesseract::views::EmojiPicker* emoji_picker_shared_ = nullptr;
    GtkWidget* sticker_popover_ = nullptr;
    std::unique_ptr<tk::gtk4::Surface> sticker_picker_surface_;
    tesseract::views::StickerPicker* sticker_picker_shared_ = nullptr;
    // Reaction picker target: set by on_add_reaction_requested.
    std::string pending_reaction_event_id_;
};

} // namespace gtk4
