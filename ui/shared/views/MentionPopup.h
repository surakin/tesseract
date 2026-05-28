#pragma once
#include "tk/widget.h"
#include "views/MentionEngine.h"
#include <algorithm>
#include <functional>
#include <vector>

namespace tesseract::views
{

// Autocomplete popup shown while typing an `@mention` in the composer.
// Mirrors ShortcodePopup's interaction model (keyboard nav driven by the
// shell, click-to-accept) but renders member rows: an initials avatar, the
// display name, and the muted user id.
class MentionPopup : public tk::Widget
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
    void set_selected_index(int index);
    int selected_index() const
    {
        return selected_index_;
    }

    int visible_rows() const
    {
        return std::min((int)candidates_.size(), kMaxRows);
    }
    const std::vector<MentionCandidate>& candidates() const
    {
        return candidates_;
    }

    std::function<void(MentionCandidate)> on_accepted;
    std::function<void()> on_dismissed;

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx& ctx, tk::Size available) override;
    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override;
    void paint(tk::PaintCtx& ctx) override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    bool on_pointer_move(tk::Point local) override;
    void on_pointer_leave() override;
    bool on_wheel(tk::Point local, float dx, float dy) override;

private:
    ImageProvider image_provider_;
    std::vector<MentionCandidate> candidates_;
    int selected_index_ = -1;
    int hovered_index_ = -1;
    int pressed_index_ = -1;

    int row_at(float y) const
    {
        int r = (int)(y / kRowHeight);
        return (r >= 0 && r < visible_rows()) ? r : -1;
    }
};

} // namespace tesseract::views
