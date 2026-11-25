#pragma once

// The 3D call room on the canvas: draws the engine's picture (EngineScene), feeds the keyboard and
// mouse to the player script, and hands the world's scripts what the call tells it. Built only with
// TESSERACT_WITH_ENGINE. Owns at most one EngineScene, started lazily by the first paint with a
// non-zero size.

#include "tesseract/types.h"
#include "tk/canvas.h"
#include "tk/widget.h"
#include "views/world/EngineScene.h"
#include "views/world/WorldButtonBar.h"
#include "views/world/world_room.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace tesseract::views::world
{

class WorldView : public tk::Widget
{
public:
    WorldView() = default;
    ~WorldView() override;

    // Off: the scene stops at once. On: it starts at the next paint with a non-zero size.
    void set_active(bool on);
    bool active() const
    {
        return active_;
    }

    // The world to run: handed over by the loader (the room's bundle or the meadow). Without one
    // the scene starts with the meadow. Replacing it while the scene runs restarts the scene.
    void set_world_options(EngineScene::Options options);
    // True while the loader is still deciding what to run: the scene does not start (it shows a
    // waiting text) until set_world_options.
    void set_awaiting_options(bool on)
    {
        awaiting_options_ = on;
    }
    // While the consent question is open in the main window: the waiting text says so.
    void set_question_pending(bool on)
    {
        question_pending_ = on;
    }
    // Translated: what the view says while it waits for the loader (empty when it does not wait).
    std::string waiting_text() const;
    bool awaiting_options() const
    {
        return awaiting_options_;
    }

    // Everyone in the call, the local participant included (it gets no robot, only the camera).
    void set_participants(std::vector<tesseract::RtcParticipantInfo> participants,
                          std::string local_participant_id);
    // Where the avatar image and the name of a user come from, for the screen of a robot whose
    // camera is off or missing (the same providers the call tiles use).
    void set_avatar_provider(std::function<const tk::Image*(const std::string& user_id)> fn)
    {
        avatar_provider_ = std::move(fn);
    }
    void set_display_name_provider(std::function<std::string(const std::string& user_id)> fn)
    {
        display_name_provider_ = std::move(fn);
    }
    // A remote participant's picture (premultiplied BGRA); null clears it. Shown on their robot.
    void on_video_frame(const std::string& participant_id, std::uint32_t w, std::uint32_t h,
                        std::shared_ptr<std::vector<std::uint8_t>> bgra);

    // Bytes that arrived from `sender` over the call's data stream: the world's script reads them.
    void on_world_frame(const std::string& sender, const std::vector<std::uint8_t>& bytes);
    // The frames the world's script wants sent to everyone in the call since the last call.
    std::vector<std::vector<std::uint8_t>> take_outgoing_frames();

    // The per-paint input alone: what the held keys and the drag since the last call ask of the
    // player (the world's player script does the walking). Paint calls this; tests call it without
    // an engine.
    MoveInput take_input_for_test()
    {
        return take_input_();
    }

    bool has_video_for_test(const std::string& participant_id) const
    {
        return videos_.count(participant_id) != 0;
    }

    // The world's action buttons (hidden at first, a handle opens them): `on_activate` hears the
    // id of the one pressed. Replaces an earlier set.
    void set_actions(std::vector<WorldButtonBar::Entry> entries,
                     std::function<void(const std::string& id)> on_activate);
    WorldButtonBar* button_bar() const
    {
        return bar_;
    }
    // Hides the button bar (closing it) while something else needs the corner, such as the
    // overlay's "the world changed" banner.
    void set_bar_hidden(bool hidden);

    // Translated; empty for None.
    static std::string failure_text(EngineScene::Failure f);

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx& ctx, tk::Rect bounds) override;
    void paint(tk::PaintCtx& ctx) override;

    bool focusable() const override
    {
        return true;
    }
    bool focus_on_click() const override
    {
        return true;
    }
    bool on_key_down(const tk::KeyEvent& e) override;
    bool on_key_up(const tk::KeyEvent& e) override;
    void on_keys_released() override;
    void on_focus_lost() override;
    bool on_pointer_down(tk::Point local) override;
    void on_pointer_drag(tk::Point local) override;
    void on_pointer_up(tk::Point local, bool inside_self) override;
    void on_pointer_motion(float dx, float dy) override;
    void on_pointer_lock_lost() override;

    // Whether the view holds the mouse and keyboard (a click takes them; Esc or Left Ctrl gives
    // them back). Without a platform pointer lock the view works the old way: keys while
    // focused, look by dragging.
    bool captured() const
    {
        return captured_;
    }

private:
    struct Video
    {
        std::shared_ptr<std::vector<std::uint8_t>> bgra;
        std::uint32_t w = 0, h = 0;
    };

    // What a robot without video shows: the participant's avatar (or initials) as a picture.
    struct AvatarScreen
    {
        const tk::Image* image = nullptr; // what it was drawn from, to redraw when it changes
        std::string name;
        std::shared_ptr<std::vector<std::uint8_t>> bgra; // premultiplied
        std::uint32_t w = 0, h = 0;
    };

    MoveInput take_input_();
    void update_avatar_screens_(tk::PaintCtx& ctx);
    void release_capture_();
    // Whether keys and the mouse move the player now: captured, or no lock is possible here.
    bool controlling_() const
    {
        return captured_ || lock_unsupported_;
    }
    void track_key_(const tk::KeyEvent& e, bool down);
    void forward_scene_state_();
    void draw_centred_text_(tk::PaintCtx& ctx, const std::string& text);
    void draw_capture_hint_(tk::PaintCtx& ctx);
    void paint_world_(tk::PaintCtx& ctx);

    EngineScene scene_;
    WorldButtonBar* bar_ = nullptr; // a child, drawn over the picture
    bool bar_hidden_ = false;
    std::optional<EngineScene::Options> world_options_;
    bool awaiting_options_ = false;
    bool question_pending_ = false;
    bool active_ = false;
    std::uint32_t size_w_ = 0, size_h_ = 0; // last device size given to the scene
    std::string participants_key_;          // what the scene was last told about who is in the call
    std::vector<std::vector<std::uint8_t>> outgoing_;
    std::vector<tesseract::RtcParticipantInfo> participants_;
    std::string local_id_;
    std::map<std::string, Video> videos_;
    std::function<const tk::Image*(const std::string&)> avatar_provider_;
    std::function<std::string(const std::string&)> display_name_provider_;
    std::map<std::string, AvatarScreen> avatar_screens_; // by participant, those with no video
    std::map<std::string, const void*> forwarded_; // the buffer last given to the scene, by id
    // Input.
    std::set<tk::PhysicalKey> held_;
    bool captured_ = false;
    bool lock_unsupported_ = false; // the platform refused the lock: fall back to focus + drag
    bool dragging_ = false;
    tk::Point drag_last_{};
    float look_dx_ = 0, look_dy_ = 0; // radians since the last step
    // The fallback image, rebuilt only when the picture buffer changes.
    std::unique_ptr<tk::Image> fallback_image_;
    const void* fallback_key_ = nullptr;
};

} // namespace tesseract::views::world
