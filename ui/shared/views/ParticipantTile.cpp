#ifdef TESSERACT_CALLS_ENABLED
#include "ParticipantTile.h"

#include "icons.h"
#include "media_utils.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr float kCellPad    = 8.0f;
constexpr float kNameH      = 20.0f;
constexpr float kBadgeSz    = 18.0f;  // mic-off / video-off badge circle diameter
constexpr float kBadgeIcon  = 12.0f;  // icon drawn inside each badge
constexpr float kPinBtnSz   = 24.0f;
constexpr float kPinInset   = 4.0f;   // inset from the video-rect corner
constexpr float kAvatarFrac = 0.55f;  // avatar diameter as fraction of media short side

// Badge colours.
constexpr tk::Color kMutedRed    {220,  60,  60, 255};
constexpr tk::Color kBadgeBg     {  0,   0,   0, 180};
constexpr tk::Color kWhite       {255, 255, 255, 255};
constexpr tk::Color kWhiteSoft   {255, 255, 255, 200};
constexpr tk::Color kAvatarBg    { 60,  60,  60, 255};
constexpr tk::Color kAvatarFg    {200, 200, 200, 255};
constexpr tk::Color kTileBg      {  0,   0,   0, 200};
constexpr tk::Color kPinAccent   {255, 200,  50, 255};
constexpr tk::Color kPinBg       {  0,   0,   0, 140};
} // namespace

// ── Construction ──────────────────────────────────────────────────────────────

ParticipantTile::ParticipantTile() = default;

// ── State / configuration ─────────────────────────────────────────────────────

void ParticipantTile::set_state(State s)
{
    state_ = std::move(s);
    if (repaint_requester_)
        repaint_requester_();
}

void ParticipantTile::push_video_frame(std::uint32_t w, std::uint32_t h,
                                        std::shared_ptr<std::vector<std::uint8_t>> bgra)
{
    // Store the shared_ptr directly — no second memcpy on the UI thread.
    // The buffer was pre-converted to premultiplied BGRA by EventHandlerBase
    // on the worker thread.
    state_.pending_bgra = std::move(bgra);
    state_.pending_w    = w;
    state_.pending_h    = h;
    if (repaint_requester_) repaint_requester_();
}

void ParticipantTile::set_pinned(bool pinned)
{
    state_.pinned = pinned;
}

void ParticipantTile::set_avatar_provider(
    std::function<const tk::Image*(const std::string&)> fn)
{
    avatar_provider_ = std::move(fn);
}

void ParticipantTile::set_repaint_requester(std::function<void()> fn)
{
    repaint_requester_ = std::move(fn);
}

// ── Layout ────────────────────────────────────────────────────────────────────

tk::Size ParticipantTile::measure(tk::LayoutCtx&, tk::Size constraints)
{
    // Fill whatever space the parent grid allocates.
    return constraints;
}

void ParticipantTile::arrange(tk::LayoutCtx& /*ctx*/, tk::Rect bounds)
{
    bounds_ = bounds;
    // video_rect_ and pin_rect_ are computed in paint() once the letterbox is known.
}

// ── Paint ─────────────────────────────────────────────────────────────────────

void ParticipantTile::paint(tk::PaintCtx& ctx)
{
    if (bounds_.w <= 0.0f || bounds_.h <= 0.0f)
        return;

    // Dark tile background.
    ctx.canvas.fill_rect(bounds_, kTileBg);

    // The media area is the tile height minus the name row and its padding.
    const float media_h = bounds_.h - kNameH - kCellPad * 2.0f;
    const float vw      = bounds_.w - kCellPad * 2.0f;
    const float vh      = std::max(0.0f, media_h);

    // ── Video frame or avatar ─────────────────────────────────────────────────
    // video_rect: the drawn video image rect (or the full media area as fallback).
    // Used below to anchor the pin button in the correct corner.
    tk::Rect video_rect{bounds_.x + kCellPad, bounds_.y + kCellPad, vw, vh};

    const bool has_pending = (state_.pending_bgra != nullptr);
    const bool show_video  = !state_.video_muted && has_pending;

    if (show_video)
    {
        // Letterbox: scale frame into the media area preserving aspect ratio.
        const float img_w = static_cast<float>(state_.pending_w);
        const float img_h = static_cast<float>(state_.pending_h);
        float draw_w = vw, draw_h = vh;
        if (img_w > 0.0f && img_h > 0.0f && vh > 0.0f)
        {
            const float aspect = img_w / img_h;
            if (vw / vh > aspect) { draw_h = vh; draw_w = vh * aspect; }
            else                  { draw_w = vw; draw_h = vw / aspect; }
        }
        const float draw_x = bounds_.x + kCellPad + (vw - draw_w) * 0.5f;
        const float draw_y = bounds_.y + kCellPad + (vh - draw_h) * 0.5f;
        const tk::Rect draw_rect{draw_x, draw_y, draw_w, draw_h};

        // All backends implement draw_bgra_premult_pixels(); the buffer was
        // pre-converted from RGBA to premultiplied BGRA on the worker thread.
        // is_self=true mirrors the local camera feed via a native canvas
        // transform — no per-pixel copy.
        ctx.canvas.draw_bgra_premult_pixels(
            state_.pending_bgra->data(), state_.pending_w,
            state_.pending_h, draw_rect, state_.is_self);
        video_rect = draw_rect;
    }
    else
    {
        // Avatar disc centred in the media area.
        const tk::Image* avatar =
            avatar_provider_ ? avatar_provider_(state_.user_id) : nullptr;
        const float diam = std::min(bounds_.w, media_h) * kAvatarFrac;
        const tk::Point ctr{bounds_.x + bounds_.w * 0.5f,
                            bounds_.y + kCellPad + media_h * 0.5f};
        draw_avatar(ctx.canvas, avatar, ctr, diam,
                    state_.display_name, kAvatarBg, kAvatarFg);
        // Use a centered square as the video rect so the black tile background
        // frames the avatar with a consistent border, and pin/mic badges anchor
        // to its corners the same way they would over a real video stream.
        const float sq   = std::min(vw, vh);
        const float sq_x = bounds_.x + kCellPad + (vw - sq) * 0.5f;
        const float sq_y = bounds_.y + kCellPad + (vh - sq) * 0.5f;
        video_rect = {sq_x, sq_y, sq, sq};
    }

    // ── Name label (bottom row) ───────────────────────────────────────────────
    {
        const float name_y = bounds_.y + bounds_.h - kNameH;
        const tk::Rect name_r{bounds_.x + kCellPad, name_y,
                               bounds_.w - kCellPad * 2.0f, kNameH};
        if (name_r.w > 0.0f && !state_.display_name.empty())
        {
            tk::TextStyle ts{};
            ts.role      = tk::FontRole::Body;
            ts.trim      = tk::TextTrim::Ellipsis;
            ts.max_width = name_r.w;
            auto layout  = ctx.factory.build_text(state_.display_name, ts);
            if (layout)
            {
                const tk::Size sz = layout->measure();
                const float tx = name_r.x + (name_r.w - sz.w) * 0.5f;
                const float ty = name_r.y + (name_r.h - sz.h) * 0.5f;
                ctx.canvas.draw_text(*layout, {tx, ty}, kWhite);
            }
        }
    }

    // ── Mic-off badge (bottom-left of video image) ───────────────────────────
    if (state_.audio_muted)
    {
        const float bx = video_rect.x + kPinInset;
        const float by = video_rect.y + video_rect.h - kBadgeSz - kPinInset;
        const tk::Rect badge{bx, by, kBadgeSz, kBadgeSz};
        ctx.canvas.fill_rounded_rect(badge, kBadgeSz * 0.5f, kBadgeBg);
        mic_off_icon_.draw(ctx.canvas, ctx.factory, kMicOffSvg,
                           badge, kBadgeIcon, kMutedRed);
    }

    // ── Video-off badge (top-right of tile) ──────────────────────────────────
    if (state_.video_muted)
    {
        const float bx = bounds_.x + bounds_.w - kCellPad - kBadgeSz;
        const float by = bounds_.y + kCellPad;
        const tk::Rect badge{bx, by, kBadgeSz, kBadgeSz};
        ctx.canvas.fill_rounded_rect(badge, kBadgeSz * 0.5f, kBadgeBg);
        video_off_icon_.draw(ctx.canvas, ctx.factory, kVideoOffSvg,
                             badge, kBadgeIcon, kMutedRed);
    }

    // ── Pin button — top-left corner of video image (or avatar rect) ─────────
    // video_rect is always valid here: the letterboxed frame rect when video
    // is live, or the avatar bounding square otherwise. Both serve as the
    // hover/hit-test target for pin and mic-off badges.
    video_rect_ = video_rect;
    pin_rect_   = {video_rect.x + kPinInset,
                   video_rect.y + kPinInset,
                   kPinBtnSz, kPinBtnSz};

    if (state_.pinned && !video_hover_)
    {
        ctx.canvas.fill_rounded_rect(pin_rect_, kPinBtnSz * 0.5f, kPinBg);
        pin_icon_.draw(ctx.canvas, ctx.factory, kPinSvg,
                       pin_rect_, kPinBtnSz * 0.7f, kPinAccent);
    }
    else if (video_hover_)
    {
        ctx.canvas.fill_rounded_rect(pin_rect_, kPinBtnSz * 0.5f, kPinBg);
        const tk::Color icon_col = state_.pinned ? kPinAccent : kWhiteSoft;
        pin_icon_.draw(ctx.canvas, ctx.factory, kPinSvg,
                       pin_rect_, kPinBtnSz * 0.7f, icon_col);
    }
}

// ── Pointer events ────────────────────────────────────────────────────────────

bool ParticipantTile::on_pointer_move(tk::Point local)
{
    const tk::Point world{bounds_.x + local.x, bounds_.y + local.y};
    const bool in_video = video_rect_.w > 0.0f
        && world.x >= video_rect_.x && world.x < video_rect_.x + video_rect_.w
        && world.y >= video_rect_.y && world.y < video_rect_.y + video_rect_.h;

    const bool changed = !hover_ || (video_hover_ != in_video);
    hover_       = true;
    video_hover_ = in_video;
    if (changed && repaint_requester_)
        repaint_requester_();
    return changed;
}

void ParticipantTile::on_pointer_leave()
{
    hover_       = false;
    video_hover_ = false;
    pin_pressed_ = false;
    if (repaint_requester_)
        repaint_requester_();
}

bool ParticipantTile::on_pointer_down(tk::Point local)
{
    const tk::Point world{bounds_.x + local.x, bounds_.y + local.y};
    if (pin_rect_.w > 0.0f &&
        world.x >= pin_rect_.x && world.x < pin_rect_.x + pin_rect_.w &&
        world.y >= pin_rect_.y && world.y < pin_rect_.y + pin_rect_.h)
    {
        pin_pressed_ = true;
        return true;
    }
    return false;
}

void ParticipantTile::on_pointer_up(tk::Point local, bool inside)
{
    if (pin_pressed_)
    {
        pin_pressed_ = false;
        if (inside)
        {
            const tk::Point world{bounds_.x + local.x, bounds_.y + local.y};
            if (world.x >= pin_rect_.x && world.x < pin_rect_.x + pin_rect_.w &&
                world.y >= pin_rect_.y && world.y < pin_rect_.y + pin_rect_.h)
            {
                if (on_pin_toggled) on_pin_toggled(state_.participant_id);
            }
        }
    }
}

} // namespace tesseract::views
#endif // TESSERACT_CALLS_ENABLED
