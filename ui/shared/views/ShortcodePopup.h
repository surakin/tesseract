#pragma once
#include "views/ListPopupBase.h"
#include "views/ShortcodeEngine.h"
#include <functional>
#include <string>
#include <vector>

namespace tesseract::views
{

// Autocomplete popup shown while typing a `:shortcode:` in the composer.
// All list scaffolding lives in ListPopupBase; this class owns the suggestion
// model and the per-row glyph/image + label paint.
class ShortcodePopup : public ListPopupBase
{
public:
    static constexpr float kRowHeight = 36.0f;
    static constexpr float kWidth = 280.0f;
    static constexpr int kMaxRows = 8;

    using ImageProvider =
        std::function<const tk::Image*(const std::string& url)>;

    void set_suggestions(std::vector<ShortcodeSuggestion> suggestions);
    void set_image_provider(ImageProvider p)
    {
        image_provider_ = std::move(p);
    }

    std::function<void(ShortcodeSuggestion)> on_accepted;
    std::function<void()> on_dismissed;

protected:
    size_t row_count() const override
    {
        return suggestions_.size();
    }
    void paint_row(tk::PaintCtx& ctx, const tk::Rect& row, size_t index,
                   bool selected, bool hovered) override;
    void on_row_activated(size_t index) override
    {
        if (on_accepted)
            on_accepted(suggestions_[index]);
    }
    float row_height() const override { return kRowHeight; }
    float width() const override { return kWidth; }
    int max_visible_rows() const override { return kMaxRows; }

private:
    ImageProvider image_provider_;
    std::vector<ShortcodeSuggestion> suggestions_;
};

} // namespace tesseract::views
