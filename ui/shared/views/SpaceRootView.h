#pragma once

// SpaceRootView — full-panel widget shown in the chat area for a Matrix space.
// Mirrors RoomPreviewView's centred summary card, but describes the joined
// space and its child counts rather than offering a join action.

#include "tk/canvas.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <optional>
#include <string>

namespace tesseract::views
{

class SpaceRootView : public tk::Widget
{
public:
    using AvatarProvider =
        std::function<const tk::Image*(const std::string& mxc)>;

    SpaceRootView();
    ~SpaceRootView() override = default;

    void set_space(const tesseract::RoomInfo& space,
                   std::size_t joined_children,
                   std::size_t unjoined_children);
    void clear();
    void set_avatar_provider(AvatarProvider p);

    // Shell should kick an avatar fetch on miss and repaint.
    std::function<void(const std::string& mxc)> on_avatar_needed;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx& ctx) override;

private:
    std::optional<tesseract::RoomInfo> space_;
    std::size_t joined_children_ = 0;
    std::size_t unjoined_children_ = 0;
    AvatarProvider avatar_provider_;

    mutable std::unique_ptr<tk::TextLayout> name_layout_;
    mutable std::unique_ptr<tk::TextLayout> alias_layout_;
    mutable std::unique_ptr<tk::TextLayout> topic_layout_;
    mutable std::unique_ptr<tk::TextLayout> meta_layout_;
    mutable std::unique_ptr<tk::TextLayout> hint_layout_;
    mutable tk::CanvasFactory* factory_seen_ = nullptr;
    mutable float last_bounds_h_ = -1.0f;
    mutable float last_content_w_ = -1.0f;

    static constexpr float kAvatarD = 72.0f;
    static constexpr float kContentW = 340.0f;
    static constexpr float kPadY = 32.0f;
    static constexpr float kGap = 12.0f;

    void reset_layouts_();
    std::string child_count_label_() const;
};

} // namespace tesseract::views
