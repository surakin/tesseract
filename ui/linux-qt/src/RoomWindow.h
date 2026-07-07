#pragma once
#include <QWidget>
#include <memory>
#include "app/RoomWindowBase.h"
#include "tk/host_qt.h"
class QMoveEvent;
#include "views/GifController.h"
#include "views/GifPopup.h"
#include "views/MentionController.h"
#include "views/MentionPopup.h"
#include "views/ShortcodeController.h"
#include "views/ShortcodePopup.h"
#include "views/SlashCommandController.h"
#include "views/SlashCommandPopup.h"

class EmojiPicker;
class StickerPicker;

namespace qt6
{
class MainWindow;
}

namespace qt6
{

// A secondary (pop-out) room window for the Qt6 shell.
class RoomWindow : public QWidget, public tesseract::RoomWindowBase
{
    Q_OBJECT
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
    void resizeEvent(QResizeEvent* ev) override;
    void moveEvent(QMoveEvent* ev) override;
    void closeEvent(QCloseEvent* ev) override;
    void keyPressEvent(QKeyEvent* ev) override;

    void surface_repaint_() override;
    // Fan-in for async GIF search results (forwarded by ShellBase to every
    // pop-out; only the controller that issued the search matches).
    void on_gif_results(std::uint64_t request_id,
                        std::vector<tesseract::GifResult> results) override;
    void on_gif_search_failed(std::uint64_t request_id,
                              const std::string& message) override;
    tk::NativeTextArea* compose_text_area_() override
    {
        return roomTextArea_.get();
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
    // Generic anchored-popup positioning shared by the slash + shortcode popups
    // (and structurally identical to show_mention_popup_): place `frame` of size
    // w×h just above/below the caret, clamped to this window's bounds.
    void show_anchored_popup_(QWidget* frame, tk::qt6::Surface* surface,
                              tk::Rect cursor_local, int w, int h);
    void show_slash_popup_(tk::Rect cursor_local, int rows);
    void show_shortcode_popup_(tk::Rect cursor_local, int rows);
    void show_gif_popup_();
    void hide_gif_popup_();

    MainWindow* parent_shell_;
    tk::qt6::Surface* surface_ = nullptr; // owned by Qt (child widget)
    std::unique_ptr<tk::NativeTextArea> roomTextArea_;
    std::unique_ptr<tk::NativeTextField> roomSearchField_;

    // Pop-out-local emoji/sticker pickers (parented to this QWidget). The emoji
    // picker doubles as the reaction picker via pendingReactionEventId_.
    ::EmojiPicker* emojiPicker_ = nullptr;
    ::StickerPicker* stickerPicker_ = nullptr;
    std::string pendingReactionEventId_;

    QWidget* mention_popup_frame_ = nullptr;
    std::unique_ptr<tk::qt6::Surface> mention_popup_surface_;
    tesseract::views::MentionPopup* mention_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::MentionController> mention_controller_;

    QWidget* slash_popup_frame_ = nullptr;
    std::unique_ptr<tk::qt6::Surface> slash_popup_surface_;
    tesseract::views::SlashCommandPopup* slash_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::SlashCommandController> slash_controller_;

    QWidget* shortcode_popup_frame_ = nullptr;
    std::unique_ptr<tk::qt6::Surface> shortcode_popup_surface_;
    tesseract::views::ShortcodePopup* shortcode_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::ShortcodeController> shortcode_controller_;

    QWidget* gif_popup_frame_ = nullptr;
    std::unique_ptr<tk::qt6::Surface> gif_popup_surface_;
    tesseract::views::GifPopup* gif_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::GifController> gif_controller_;
};

} // namespace qt6
