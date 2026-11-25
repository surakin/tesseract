#include "views/world/WorldView.h"

#include "tk/host.h"
#include "tk/i18n.h"
#include "tk/theme.h"
#include "views/media_utils.h"
#include "views/shortcut_registry.h"

#include <algorithm>
#include <cmath>
#include <chrono>
#include <optional>
#include <utility>

namespace tesseract::views::world
{

WorldView::~WorldView()
{
    scene_.stop();
}

void WorldView::set_active(bool on)
{
    if (active_ == on)
    {
        return;
    }
    active_ = on;
    if (!on)
    {
        scene_.stop();
        fallback_image_.reset();
        fallback_key_ = nullptr;
        size_w_ = size_h_ = 0;
        forwarded_.clear();
        avatar_screens_.clear();
        // Hidden: nothing runs, so nothing is sent or heard.
        on_keys_released(); // also gives the mouse and keyboard back
        look_dx_ = look_dy_ = 0;
        participants_key_.clear();
        outgoing_.clear();
    }
}

std::string WorldView::waiting_text() const
{
    if (!awaiting_options_)
    {
        return {};
    }
    return question_pending_ ? tk::tr("Waiting for your answer in the main window")
                             : tk::tr("Loading the world…");
}

void WorldView::set_world_options(EngineScene::Options options)
{
    world_options_ = std::move(options);
    awaiting_options_ = false;
    question_pending_ = false;
    if (scene_.state() != EngineScene::State::Stopped)
    {
        // The next paint starts the scene again with the new world.
        scene_.stop();
        fallback_image_.reset();
        fallback_key_ = nullptr;
        size_w_ = size_h_ = 0;
        forwarded_.clear();
        avatar_screens_.clear();
        participants_key_.clear();
        outgoing_.clear();
    }
}

void WorldView::set_participants(std::vector<tesseract::RtcParticipantInfo> participants,
                                 std::string local_participant_id)
{
    participants_ = std::move(participants);
    local_id_ = std::move(local_participant_id);
    // A switched-off camera is the flat dark screen, not its last picture.
    for (const auto& p : participants_)
    {
        if (p.is_video_muted && videos_.erase(p.participant_id) != 0)
        {
            if (forwarded_.erase(p.participant_id) != 0)
            {
                scene_.clear_robot_screen(p.participant_id);
            }
        }
    }
    // Forget the pictures of people who left.
    for (auto it = videos_.begin(); it != videos_.end();)
    {
        const std::string& id = it->first;
        const bool present = std::any_of(participants_.begin(), participants_.end(),
                                         [&](const tesseract::RtcParticipantInfo& p)
                                         {
                                             return p.participant_id == id;
                                         });
        if (present)
        {
            ++it;
        }
        else
        {
            forwarded_.erase(id);
            it = videos_.erase(it);
        }
    }
}

void WorldView::on_video_frame(const std::string& participant_id, std::uint32_t w, std::uint32_t h,
                               std::shared_ptr<std::vector<std::uint8_t>> bgra)
{
    if (bgra && (w == 0 || h == 0 || bgra->size() < std::size_t{w} * h * 4))
    {
        bgra.reset();
    }
    if (!bgra)
    {
        if (videos_.erase(participant_id) != 0 && forwarded_.erase(participant_id) != 0)
        {
            scene_.clear_robot_screen(participant_id);
        }
        return;
    }
    const bool muted =
        std::any_of(participants_.begin(), participants_.end(),
                    [&](const tesseract::RtcParticipantInfo& p)
                    {
                        return p.participant_id == participant_id && p.is_video_muted;
                    });
    if (muted)
    {
        return; // a late frame of a camera that is now off
    }
    videos_[participant_id] = Video{std::move(bgra), w, h};
}

void WorldView::on_world_frame(const std::string& sender, const std::vector<std::uint8_t>& bytes)
{
    if (active_)
    {
        scene_.post_event("frame", frame_event(sender, bytes.data(), bytes.size()));
    }
}

std::vector<std::vector<std::uint8_t>> WorldView::take_outgoing_frames()
{
    return std::exchange(outgoing_, {});
}

void WorldView::track_key_(const tk::KeyEvent& e, bool down)
{
    if (down)
    {
        held_.insert(e.physical);
    }
    else
    {
        held_.erase(e.physical); // idempotent
    }
}

namespace
{
// Esc and Left Ctrl give the mouse and keyboard back.
bool is_release_key(const tk::KeyEvent& e)
{
    return e.key == tk::Key::Escape || e.physical == tk::PhysicalKey::ControlLeft;
}
} // namespace

bool WorldView::on_key_down(const tk::KeyEvent& e)
{
    if (bar_ && !bar_hidden_ && active_ && bounds_.w > 0.0f && bounds_.h > 0.0f && !e.ctrl &&
        !e.alt && !e.meta && matches(ShortcutId::WorldBar, e))
    {
        // The mouse is needed for the buttons: let go of it, then show or hide them.
        release_capture_();
        const bool was_open = bar_->expanded();
        bar_->toggle(/*focus_first=*/true);
        if (was_open && host())
        {
            host()->request_focus(this);
        }
        return true;
    }
    if (captured_)
    {
        // The whole keyboard is ours while captured, except the button bar's own shortcut above.
        if (is_release_key(e))
        {
            release_capture_();
        }
        else if (e.physical != tk::PhysicalKey::Unknown)
        {
            track_key_(e, true);
        }
        return true;
    }
    if (!controlling_() || e.physical == tk::PhysicalKey::Unknown)
    {
        return false;
    }
    // Ctrl/Alt/Meta+W and friends are other shortcuts (select all, close ...).
    if (e.ctrl || e.alt || e.meta)
    {
        return false;
    }
    track_key_(e, true);
    return true;
}

bool WorldView::on_key_up(const tk::KeyEvent& e)
{
    // Idempotent: a key-up can arrive for a press this widget never saw.
    if (e.physical == tk::PhysicalKey::Unknown)
    {
        return captured_;
    }
    track_key_(e, false);
    return controlling_();
}

void WorldView::on_keys_released()
{
    held_.clear();
    dragging_ = false;
    release_capture_(); // the window lost the keyboard
}

void WorldView::on_focus_lost()
{
    on_keys_released();
}

void WorldView::release_capture_()
{
    if (!captured_)
    {
        return;
    }
    captured_ = false;
    held_.clear();
    unlock_pointer();
}

void WorldView::on_pointer_lock_lost()
{
    captured_ = false;
    held_.clear();
    dragging_ = false;
}

bool WorldView::on_pointer_down(tk::Point local)
{
    if (captured_)
    {
        return true; // the mouse looks without a button
    }
    if (!lock_unsupported_)
    {
        if (lock_pointer())
        {
            captured_ = true;
            return true;
        }
        lock_unsupported_ = true; // this platform (or host) cannot lock: the old controls
    }
    dragging_ = true;
    drag_last_ = local;
    return true;
}

void WorldView::on_pointer_motion(float dx, float dy)
{
    constexpr float kRadiansPerPixel = 0.005f;
    look_dx_ += dx * kRadiansPerPixel;
    look_dy_ += dy * kRadiansPerPixel;
}

void WorldView::on_pointer_drag(tk::Point local)
{
    if (!dragging_)
    {
        return;
    }
    constexpr float kRadiansPerPixel = 0.005f;
    look_dx_ += (local.x - drag_last_.x) * kRadiansPerPixel;
    look_dy_ += (local.y - drag_last_.y) * kRadiansPerPixel;
    drag_last_ = local;
}

void WorldView::on_pointer_up(tk::Point, bool)
{
    dragging_ = false;
}

MoveInput WorldView::take_input_()
{
    const auto has = [&](tk::PhysicalKey k)
    {
        return held_.count(k) != 0;
    };
    MoveInput in;
    using K = tk::PhysicalKey;
    in.forward = has(K::W) || has(K::Up);
    in.back = has(K::S) || has(K::Down);
    in.left = has(K::Left);
    in.right = has(K::Right);
    in.strafe_left = has(K::A);
    in.strafe_right = has(K::D);
    in.jump = has(K::Space);
    in.look_dx = look_dx_;
    in.look_dy = look_dy_;
    look_dx_ = look_dy_ = 0;
    return in;
}

void WorldView::update_avatar_screens_(tk::PaintCtx& ctx)
{
    // The screen's own 16:9 picture: the call tile's avatar disc on the dark of an idle screen.
    constexpr float kW = 320.0f, kH = 180.0f;
    std::set<std::string> wanted;
    for (const tesseract::RtcParticipantInfo& p : participants_)
    {
        if (p.is_local || p.participant_id == local_id_ || videos_.count(p.participant_id) != 0)
        {
            continue;
        }
        wanted.insert(p.participant_id);
        const tk::Image* image = avatar_provider_ ? avatar_provider_(p.user_id) : nullptr;
        std::string name =
            display_name_provider_ ? display_name_provider_(p.user_id) : std::string();
        if (name.empty())
        {
            name = p.user_id;
        }
        AvatarScreen& screen = avatar_screens_[p.participant_id];
        if (screen.bgra && screen.image == image && screen.name == name)
        {
            continue;
        }
        screen.image = image;
        screen.name = name;
        screen.bgra.reset();
        auto surface = ctx.factory.create_offscreen({kW, kH}, 1.0f);
        if (!surface)
        {
            continue;
        }
        tk::Canvas& canvas = surface->canvas();
        canvas.fill_rect({0, 0, kW, kH}, tk::Color{20, 20, 24, 255});
        views::draw_avatar(canvas, image, {kW * 0.5f, kH * 0.5f},
                           std::min(kW, kH) * views::kCallAvatarFrac, name, views::kCallAvatarBg,
                           views::kCallAvatarFg);
        auto pixels = std::make_shared<std::vector<std::uint8_t>>();
        if (surface->finish_bgra(*pixels, screen.w, screen.h))
        {
            screen.bgra = std::move(pixels);
        }
    }
    for (auto it = avatar_screens_.begin(); it != avatar_screens_.end();)
    {
        it = wanted.count(it->first) != 0 ? std::next(it) : avatar_screens_.erase(it);
    }
}

void WorldView::forward_scene_state_()
{
    std::vector<std::string> robots;
    for (const tesseract::RtcParticipantInfo& p : participants_)
    {
        if (!p.is_local && p.participant_id != local_id_)
        {
            robots.push_back(p.participant_id);
        }
    }
    // Who is in the call: the world's script places everyone (and starts the local player).
    std::string key = local_id_;
    for (const std::string& id : robots)
    {
        key += '\n' + id;
    }
    if (!local_id_.empty() && key != participants_key_)
    {
        participants_key_ = std::move(key);
        scene_.post_event("participants", participants_event(local_id_, robots));
    }
    // What the script wants sent to the others.
    for (ScriptEvent& e : scene_.take_events())
    {
        if (e.name == "send" && e.value.kind == EventValue::Kind::Bytes)
        {
            outgoing_.emplace_back(e.value.text.begin(), e.value.text.end());
        }
    }
    scene_.set_robots(std::move(robots)); // before the pictures: unknown ids are ignored
    for (const auto& [id, video] : videos_)
    {
        auto it = forwarded_.find(id);
        if (it != forwarded_.end() && it->second == video.bgra.get())
        {
            continue;
        }
        forwarded_[id] = video.bgra.get();
        scene_.set_robot_screen_bgra(id, video.bgra, video.w, video.h);
    }
    for (const auto& [id, screen] : avatar_screens_)
    {
        auto it = forwarded_.find(id);
        if (!screen.bgra || (it != forwarded_.end() && it->second == screen.bgra.get()))
        {
            continue;
        }
        forwarded_[id] = screen.bgra.get();
        scene_.set_robot_screen_bgra(id, screen.bgra, screen.w, screen.h);
    }
}

std::string WorldView::failure_text(EngineScene::Failure f)
{
    switch (f)
    {
    case EngineScene::Failure::None:
        return {};
    case EngineScene::Failure::LoadFailed:
        return tk::tr("The 3D engine could not be loaded.");
    case EngineScene::Failure::NoOffscreen:
        return tk::tr("This 3D engine cannot draw off-screen.");
    case EngineScene::Failure::NoDevice:
        return tk::tr("No graphics device is available for the 3D view.");
    case EngineScene::Failure::ImportFailed:
        return tk::tr("The 3D world could not be loaded.");
    case EngineScene::Failure::SceneFailed:
        return tk::tr("The 3D scene could not be built.");
    }
    return {};
}

tk::Size WorldView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void WorldView::draw_centred_text_(tk::PaintCtx& ctx, const std::string& text)
{
    tk::TextStyle style{};
    style.wrap = true;
    style.halign = tk::TextHAlign::Center;
    style.max_width = bounds_.w;
    auto layout = ctx.factory.build_text(text, style);
    if (!layout)
    {
        return;
    }
    const tk::Size sz = layout->measure();
    ctx.canvas.draw_text(*layout, {bounds_.x, bounds_.y + (bounds_.h - sz.h) * 0.5f},
                         ctx.theme.palette.text_secondary);
}

// A pill at the bottom telling how to take the mouse and keyboard.
void WorldView::draw_capture_hint_(tk::PaintCtx& ctx)
{
    tk::TextStyle style{};
    style.wrap = true;
    style.halign = tk::TextHAlign::Center;
    constexpr float kHintPad = 8.0f;
    style.max_width = std::max(1.0f, bounds_.w - 4 * kHintPad);
    auto layout = ctx.factory.build_text(
        tk::tr("Click to look around and walk. Esc or Left Ctrl gives the mouse back."), style);
    if (!layout)
    {
        return;
    }
    const tk::Size sz = layout->measure();
    const tk::Rect pill{bounds_.x + (bounds_.w - sz.w) * 0.5f - kHintPad,
                        bounds_.y + bounds_.h - sz.h - 3 * kHintPad, sz.w + 2 * kHintPad,
                        sz.h + 2 * kHintPad};
    ctx.canvas.fill_rounded_rect(pill, kHintPad, tk::Color{0, 0, 0, 150});
    ctx.canvas.draw_text(*layout, {pill.x + kHintPad, pill.y + kHintPad},
                         tk::Color{255, 255, 255, 255});
}

void WorldView::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    if (bar_)
    {
        // In the top-left corner; the overlay's banner hides the bar while it shows.
        constexpr float kInset = 8.0f;
        const tk::Size s = bar_->measure(ctx, {bounds.w, bounds.h});
        bar_->arrange(ctx, bounds.h > 0.0f
                               ? tk::Rect{bounds.x + kInset, bounds.y + kInset, s.w, s.h}
                               : tk::Rect{});
    }
}

void WorldView::set_actions(std::vector<WorldButtonBar::Entry> entries,
                            std::function<void(const std::string& id)> on_activate)
{
    if (bar_)
    {
        remove_child(bar_);
        bar_ = nullptr;
    }
    bar_ = add_child(tk::create_widget<WorldButtonBar>(this, std::move(entries)));
    bar_->set_visible(!bar_hidden_);
    bar_->on_activate = std::move(on_activate);
    bar_->on_closed_by_key = [this]
    {
        if (host())
        {
            host()->request_focus(this); // the keyboard goes back to walking
        }
    };
    bar_->on_layout_changed = [this]
    {
        if (host())
        {
            host()->request_relayout();
        }
    };
}

void WorldView::set_bar_hidden(bool hidden)
{
    if (hidden == bar_hidden_)
    {
        return;
    }
    bar_hidden_ = hidden;
    if (!bar_)
    {
        return;
    }
    if (hidden)
    {
        bar_->set_expanded(false);
        // The keyboard must not stay on a widget that is no longer shown.
        if (host())
        {
            for (const tk::Widget* w = host()->focused_widget(); w; w = w->parent())
            {
                if (w == bar_)
                {
                    host()->request_focus(this);
                    break;
                }
            }
        }
    }
    bar_->set_visible(!hidden);
    if (host())
    {
        host()->request_relayout();
    }
}

void WorldView::paint(tk::PaintCtx& ctx)
{
    paint_world_(ctx);
    if (active_ && bar_ && !bar_hidden_ && bounds_.h > 0.0f)
    {
        bar_->paint(ctx);
    }
}

void WorldView::paint_world_(tk::PaintCtx& ctx)
{
    if (!active_)
    {
        return;
    }
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);

    // Device pixels for the surface, logical bounds for drawing.
    const float scale = ctx.canvas.scale_factor();
    const auto dev_w = static_cast<std::uint32_t>(std::lround(bounds_.w * scale));
    const auto dev_h = static_cast<std::uint32_t>(std::lround(bounds_.h * scale));
    if (dev_w == 0 || dev_h == 0)
    {
        return;
    }

    if (awaiting_options_)
    {
        draw_centred_text_(ctx, waiting_text());
        return;
    }

    if (scene_.state() == EngineScene::State::Stopped)
    {
        EngineScene::Options options = world_options_ ? *world_options_ : room_options();
        options.width = dev_w;
        options.height = dev_h;
        scene_.start(std::move(options));
        participants_key_.clear();
        size_w_ = dev_w;
        size_h_ = dev_h;
        forwarded_.clear(); // start() drops the pending screens
    }
    else if (dev_w != size_w_ || dev_h != size_h_)
    {
        scene_.resize(dev_w, dev_h);
        size_w_ = dev_w;
        size_h_ = dev_h;
    }

    // Input and poses, once per paint.
    scene_.post_event("input", input_event(take_input_()));
    update_avatar_screens_(ctx);
    forward_scene_state_();

    const Picture pic = scene_.latest_picture();
    if (pic.rgba && pic.width > 0 && pic.height > 0)
    {
        if (!ctx.canvas.draw_rgba_pixels(pic.rgba->data(), pic.width, pic.height, bounds_))
        {
            if (fallback_key_ != pic.rgba.get())
            {
                fallback_image_ = ctx.factory.create_image_rgba(
                    pic.rgba->data(), static_cast<int>(pic.width), static_cast<int>(pic.height));
                fallback_key_ = pic.rgba.get();
            }
            if (fallback_image_)
            {
                ctx.canvas.draw_image(*fallback_image_, bounds_);
            }
        }
    }
    if (scene_.state() == EngineScene::State::Failed)
    {
        draw_centred_text_(ctx, failure_text(scene_.failure()));
    }
    else if (pic.rgba && !controlling_())
    {
        draw_capture_hint_(ctx);
    }
    else if (!pic.rgba)
    {
        draw_centred_text_(ctx, tk::tr("Starting the 3D view…"));
    }

    if (ctx.host)
    {
        ctx.host->request_repaint();
    }
}

} // namespace tesseract::views::world
