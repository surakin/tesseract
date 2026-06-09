#pragma once
#include "views/ListPopupBase.h"
#include "views/MentionEngine.h"
#include <functional>
#include <vector>

namespace tesseract::views
{

// Autocomplete popup shown while typing an `@mention` in the composer.
// Mirrors ShortcodePopup's interaction model (keyboard nav driven by the
// shell, click-to-accept) but renders member rows: an initials avatar, the
// display name, and the muted user id. All the list scaffolding lives in
// ListPopupBase; this class only owns the candidate model and per-row paint.
class MentionPopup : public ListPopupBase
{
public:
    static constexpr float kRowHeight = 40.0f;
    static constexpr float kWidth = 320.0f;
    static constexpr int kMaxRows = 8;

    using ImageProvider =
        std::function<const tk::Image*(const std::string& url)>;
    void set_image_provider(ImageProvider p)
    {
        image_provider_ = std::move(p);
    }

    void set_candidates(std::vector<MentionCandidate> candidates);
    const std::vector<MentionCandidate>& candidates() const
    {
        return candidates_;
    }

    std::function<void(MentionCandidate)> on_accepted;
    std::function<void()> on_dismissed;

protected:
    size_t row_count() const override
    {
        return candidates_.size();
    }
    void paint_row(tk::PaintCtx& ctx, const tk::Rect& row, size_t index,
                   bool selected, bool hovered) override;
    void on_row_activated(size_t index) override
    {
        if (on_accepted)
            on_accepted(candidates_[index]);
    }
    float row_height() const override { return kRowHeight; }
    float width() const override { return kWidth; }
    int max_visible_rows() const override { return kMaxRows; }

private:
    ImageProvider image_provider_;
    std::vector<MentionCandidate> candidates_;
};

} // namespace tesseract::views
