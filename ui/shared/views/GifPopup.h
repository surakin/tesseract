#pragma once
#include "tk/widget.h"

#include <tesseract/types.h> // tesseract::GifResult
#include <tesseract/visual.h>

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
    static constexpr float kCardRadius = tesseract::visual::kRadiusMD; // matches the compose attachment band
    static constexpr int kMaxCells = 16;
    // Status-row metrics (single-line message mode).
    static constexpr float kStatusH = 22.0f;
    static constexpr float kStatusMinW = 160.0f;

    /// Resolve a result to an image frame (animated or static), or null while
    /// loading. The shell fetches `result.preview_url` (static JPEG, fast) for
    /// an immediate placeholder and `result.image_url` (animated WebP/GIF) for
    /// the animated version, storing each in the appropriate cache.
    using ImageProvider =
        std::function<const tk::Image*(const tesseract::GifResult&)>;

    void set_results(std::vector<tesseract::GifResult> results);
    // Show a one-line status message instead of results (e.g. "No GIFs found",
    // "No GIF API key configured", "Send failed"). Clears any current results.
    void set_status(std::string message);
    bool has_status() const
    {
        return !status_.empty() && results_.empty();
    }
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

    /// Logical size the popup wants given the host's available width. Returns
    /// {0,0} when there is nothing to show (no results and no status). The strip
    /// width is clamped to `max_width` (the overflow is reachable by scrolling
    /// the selection); a status row gets a compact width clamped the same way.
    tk::Size content_size(float max_width) const;

    std::function<void(const tesseract::GifResult&)> on_accepted;
    std::function<void()> on_dismissed;

    tk::Size measure(tk::LayoutCtx& ctx, tk::Size available) override;
    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override;
    void paint(tk::PaintCtx& ctx) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;
    bool on_wheel(tk::Point local, float dx, float dy, bool is_touchpad = false) override;

private:
    int cell_at(tk::Point local) const;
    tk::Rect cell_rect(int i) const;
    // Width the result row would occupy with all `visible_count()` cells laid
    // out edge to edge (excludes the outer padding's right side beyond cells).
    float content_width_() const;
    // Shift scroll_x_ so the selected cell sits fully inside the viewport
    // (`bounds_.w`), then clamp to the scrollable range.
    void ensure_selected_visible_();

    ImageProvider image_provider_;
    std::vector<tesseract::GifResult> results_;
    std::string status_;
    int selected_index_ = -1;
    int hovered_index_ = -1;
    int pressed_index_ = -1;
    float scroll_x_ = 0.0f;     // horizontal scroll offset of the cell row
    float wheel_carry_ = 0.0f;  // sub-notch accumulator for smooth-scroll devices
};

} // namespace tesseract::views
