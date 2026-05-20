#pragma once

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/widget.h"

#include <tesseract/types.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace tesseract::views
{

class RoomInfoPanel : public tk::Widget
{
public:
    static constexpr float kPanelW = 280.0f;

    RoomInfoPanel();
    ~RoomInfoPanel() override = default;

    void open(const tesseract::RoomInfo& info);
    void refresh_info(const tesseract::RoomInfo& info); // update without resetting state
    void close();
    bool is_open() const { return open_; }

    void set_members(std::vector<tesseract::RoomMember> members);

    using ImageProvider = std::function<const tk::Image*(const std::string& mxc)>;
    void set_avatar_provider(ImageProvider p);

    // NativeTextArea overlay for topic editing (follows RecoveryBanner pattern).
    // Returns the text-area rect when editing, empty otherwise.
    tk::Rect    topic_edit_rect() const;
    bool        topic_edit_visible() const { return editing_topic_; }
    void        set_topic_edit_text(std::string t);
    std::string topic_edit_initial_text() const { return topic_edit_text_; }

    // Fired when editing_topic_ flips so the shell can relayout the surface
    // and show/hide the NativeTextArea overlay.
    std::function<void()> on_layout_changed;

    // Shell callbacks
    std::function<void(std::string room_id)>                on_fetch_members;
    std::function<void(std::string room_id, std::string t)> on_save_topic;
    std::function<void(std::string room_id)>                on_leave_room;
    std::function<void(std::string user_id,
                       std::string display_name,
                       std::string avatar_url)>             on_member_clicked;
    std::function<void()>                                   on_close;

    // tk::Widget overrides
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void     arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void     paint(tk::PaintCtx&) override;
    bool     on_pointer_down(tk::Point local) override;
    void     on_pointer_up(tk::Point local, bool inside_self) override;
    bool     on_pointer_move(tk::Point local) override;
    void     on_pointer_leave() override;
    bool     on_wheel(tk::Point local, float dx, float dy) override;

private:
    bool open_ = false;

    // Room data
    std::string room_id_;
    std::string display_name_;
    std::string avatar_url_;
    std::string topic_;
    std::string topic_html_;
    bool        is_encrypted_      = false;
    std::string history_visibility_;

    // Members
    std::vector<tesseract::RoomMember> members_;
    bool members_expanded_ = false;

    // Topic edit state
    bool        editing_topic_  = false;
    std::string topic_edit_text_;
    tk::Rect    topic_edit_rect_{}; // world-space rect for NativeTextArea

    // Child buttons (borrowed pointers from add_child)
    tk::Button* close_btn_      = nullptr;
    tk::Button* edit_topic_btn_ = nullptr;
    tk::Button* save_btn_       = nullptr;
    tk::Button* cancel_btn_     = nullptr;
    tk::Button* expand_btn_     = nullptr;
    tk::Button* leave_btn_      = nullptr;

    // Layout rects (world-space, updated each arrange)
    tk::Rect panel_rect_{};
    tk::Rect backdrop_rect_{};
    tk::Rect avatar_rect_{};
    tk::Rect topic_rect_{};
    // Member row rects: up to 5 (or all when expanded), 44px each
    std::vector<tk::Rect> member_rects_;

    // Cached text layouts
    std::unique_ptr<tk::TextLayout> name_layout_;
    std::unique_ptr<tk::TextLayout> badge_enc_layout_;
    std::unique_ptr<tk::TextLayout> badge_hist_layout_;
    std::unique_ptr<tk::TextLayout> topic_layout_;
    struct MemberLayout {
        std::unique_ptr<tk::TextLayout> name;
        std::unique_ptr<tk::TextLayout> uid;
    };
    std::vector<MemberLayout> member_layouts_;

    bool  press_backdrop_  = false;
    int   hover_member_    = -1;
    int   press_member_    = -1;
    float scroll_offset_   = 0.0f; // pixels scrolled from top of scrollable content
    float content_height_  = 0.0f; // total scrollable content height, updated each arrange

    ImageProvider image_provider_;

    static constexpr float kAvatarD     = 72.0f;
    static constexpr float kAvatarSmall = 32.0f;
    static constexpr float kPadX        = 16.0f;
    static constexpr float kPadY        = 12.0f;
    static constexpr float kHeaderH     = 48.0f;
    static constexpr float kButtonH     = 36.0f;
    static constexpr float kMemberRowH  = 44.0f;
    static constexpr float kSmallEditH  = 28.0f;
};

} // namespace tesseract::views
