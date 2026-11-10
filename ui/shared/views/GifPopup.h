#pragma once
#include "views/ImageStripPopup.h"

#include <tesseract/types.h> // tesseract::GifResult

#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

/// Inline GIF result strip shown above the composer while the user types a
/// `/gif <query>` command. A horizontal row of animated thumbnails driven by a
/// host-supplied image provider (which decodes the preview URL via the shell's
/// animation cache). Keyboard nav (←/→/Enter/Esc) is driven by the
/// GifController; the shared strip handles pointer hover/click.
class GifPopup : public ImageStripPopup
{
public:
    static constexpr float kCellW = 110.0f;
    static constexpr float kCellH = 90.0f;
    static constexpr float kAttribH = 16.0f;
    static constexpr int kMaxCells = 16;

    GifPopup() : ImageStripPopup(kCellW, kCellH, kAttribH, kMaxCells)
    {
    }

    /// Resolve a result to an image frame (animated or static), or null while
    /// loading. The shell fetches `result.preview_url` (static JPEG, fast) for
    /// an immediate placeholder and `result.image_url` (animated WebP/GIF) for
    /// the animated version, storing each in the appropriate cache.
    using ImageProvider = std::function<const tk::Image*(const tesseract::GifResult&)>;

    void set_results(std::vector<tesseract::GifResult> results);
    // Show a one-line status message instead of results (e.g. "No GIFs found",
    // "No GIF API key configured", "Send failed"). Clears any current results.
    void set_status(std::string message);
    void set_image_provider(ImageProvider p)
    {
        image_provider_ = std::move(p);
    }
    const std::vector<tesseract::GifResult>& results() const
    {
        return results_;
    }
    const tesseract::GifResult* selected() const
    {
        return (selected_index() >= 0 && selected_index() < visible_count())
                   ? &results_[std::size_t(selected_index())]
                   : nullptr;
    }

    std::function<void(const tesseract::GifResult&)> on_accepted;

protected:
    void paint_item(tk::PaintCtx& ctx, int i, tk::Rect cell) override;
    void accept_item(int i) override;
    void paint_footer(tk::PaintCtx& ctx) override;

private:
    ImageProvider image_provider_;
    std::vector<tesseract::GifResult> results_;
};

} // namespace tesseract::views
