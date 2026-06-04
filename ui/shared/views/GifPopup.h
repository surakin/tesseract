#pragma once
#include "tk/widget.h"

#include <tesseract/types.h> // tesseract::GifResult

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

/// Inline GIF result strip shown above the composer while the user types a
/// `/gif <query>` command. A horizontal row of animated thumbnails driven by a
/// host-supplied image provider (which decodes the preview URL via the shell's
/// animation cache). Keyboard nav (←/→/Enter/Esc) is driven by the
/// GifController; the popup itself handles pointer hover/click. Parallel to
/// ShortcodePopup but horizontal.
class GifPopup : public tk::Widget
{
public:
    static constexpr float kCellW = 110.0f;
    static constexpr float kCellH = 90.0f;
    static constexpr float kGap = 4.0f;
    static constexpr float kPad = 6.0f;
    static constexpr float kAttribH = 16.0f;
    static constexpr int kMaxCells = 16;

    /// Resolve a result's `preview_url` to a decoded (possibly animated) frame,
    /// or null while it is still loading. The shell kicks off the fetch on a
    /// miss and repaints once the bytes land.
    using ImageProvider =
        std::function<const tk::Image*(const std::string& preview_url)>;

    void set_results(std::vector<tesseract::GifResult> results);
    void set_image_provider(ImageProvider p)
    {
        image_provider_ = std::move(p);
    }
    void set_selected_index(int index);
    int selected_index() const
    {
        return selected_index_;
    }
    int visible_count() const
    {
        return std::min(static_cast<int>(results_.size()), kMaxCells);
    }
    const std::vector<tesseract::GifResult>& results() const
    {
        return results_;
    }
    const tesseract::GifResult* selected() const
    {
        return (selected_index_ >= 0 && selected_index_ < visible_count())
                   ? &results_[std::size_t(selected_index_)]
                   : nullptr;
    }

    /// Shift the selection by `delta` (clamped to [0, visible-1]). Returns true
    /// when there is a list to move within.
    bool move_selection(int delta);

    std::function<void(const tesseract::GifResult&)> on_accepted;
    std::function<void()> on_dismissed;

    tk::Size measure(tk::LayoutCtx& ctx, tk::Size available) override;
    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override;
    void paint(tk::PaintCtx& ctx) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;

private:
    int cell_at(tk::Point local) const;
    tk::Rect cell_rect(int i) const;

    ImageProvider image_provider_;
    std::vector<tesseract::GifResult> results_;
    int selected_index_ = -1;
    int hovered_index_ = -1;
    int pressed_index_ = -1;
    tk::Rect bounds_{};
};

} // namespace tesseract::views
