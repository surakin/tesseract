#include "MediaViewerOverlay.h"
#include "AudioViewerPage.h"
#include "FileViewerPage.h"
#include "ImageViewerPage.h"
#include "VideoViewerPage.h"
#include "icons.h"
#include "shortcut_registry.h"

#include "tk/i18n.h"

#include <algorithm>
#include <string>

namespace tesseract::views
{

namespace
{
constexpr float kNavBtnSize = 44.0f;  // prev / next circle diameter
constexpr float kNavBtnInset = 16.0f; // distance from the left / right edge
// Horizontal room kept free on each side of the page content while a sequence
// is shown windowed, so the image / video never runs under the nav buttons.
constexpr float kNavGutter = kNavBtnSize + kNavBtnInset + 4.0f;
constexpr float kNavIconPx = 24.0f;
constexpr float kCounterH = 36.0f;
constexpr float kCounterPadX = 12.0f;
constexpr tk::Color kNavPillRest = tk::Color::rgba(0, 0, 0, 160);
constexpr tk::Color kNavPillHover = tk::Color::rgba(60, 60, 60, 185);
constexpr tk::Color kNavPillPressed = tk::Color::rgba(95, 95, 95, 200);
} // namespace

// ── construction ─────────────────────────────────────────────────────────

MediaViewerOverlay::MediaViewerOverlay()
{
    // Pages add their own buttons to this overlay (after the base chrome
    // buttons, so hit-test order matches the old per-kind overlays).
    MediaViewerPageHost& host = *this; // private base: convert here, not inside make_unique
    image_page_ = std::make_unique<ImageViewerPage>(host);
    video_page_ = std::make_unique<VideoViewerPage>(host);
    audio_page_ = std::make_unique<AudioViewerPage>(host);
    file_page_ = std::make_unique<FileViewerPage>(host, [this] { fire_save_(); });

    auto prev = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                              tk::Button::Variant::Icon);
    prev->set_accessible_name(tk::tr("Previous"));
    prev_btn_ = add_child(std::move(prev));
    prev_btn_->set_on_click([this] { step_(-1); });
    prev_btn_->set_fill_override(
        tk::Button::FillOverride{kNavPillRest, kNavPillHover, kNavPillPressed});
    prev_btn_->set_visible(false);

    auto next = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                              tk::Button::Variant::Icon);
    next->set_accessible_name(tk::tr("Next"));
    next_btn_ = add_child(std::move(next));
    next_btn_->set_on_click([this] { step_(1); });
    next_btn_->set_fill_override(
        tk::Button::FillOverride{kNavPillRest, kNavPillHover, kNavPillPressed});
    next_btn_->set_visible(false);
}

MediaViewerOverlay::~MediaViewerOverlay() = default;

// ── opening and state ────────────────────────────────────────────────────

void MediaViewerOverlay::open(MediaViewerItem item)
{
    std::vector<MediaViewerItem> items;
    items.push_back(std::move(item));
    open_sequence(std::move(items), 0);
}

void MediaViewerOverlay::open_sequence(std::vector<MediaViewerItem> items,
                                       std::size_t index)
{
    if (items.empty())
    {
        return;
    }
    items_ = std::move(items);
    index_ = index < items_.size() ? index : 0;
    // Only open()/open_sequence() reset full-screen; stepping never does.
    // Re-opening while full-screen (e.g. the avatar-click path) must tell the
    // shell too, or its window stays full-screen.
    leave_fullscreen_();
    show_index_(index_);
}

void MediaViewerOverlay::close()
{
    dismiss_();
}

void MediaViewerOverlay::dismiss_()
{
    if (active_)
    {
        active_->deactivate();
        active_ = nullptr;
    }
    ++load_token_;
    MediaOverlayBase::dismiss_();
}

void MediaViewerOverlay::step_(int delta)
{
    const std::size_t n = items_.size();
    if (n < 2 || !is_open_)
    {
        return;
    }
    const long long next =
        (static_cast<long long>(index_) + delta + static_cast<long long>(n)) %
        static_cast<long long>(n);
    show_index_(static_cast<std::size_t>(next));
}

const MediaViewerItem& MediaViewerOverlay::current_item() const
{
    static const MediaViewerItem kEmpty{};
    return index_ < items_.size() ? items_[index_] : kEmpty;
}

MediaViewerPage* MediaViewerOverlay::page_for_(MediaViewerItem::Kind kind) const
{
    switch (kind)
    {
        case MediaViewerItem::Kind::Image:
        {
            return image_page_.get();
        }
        case MediaViewerItem::Kind::Video:
        {
            return video_page_.get();
        }
        case MediaViewerItem::Kind::Audio:
        {
            return audio_page_.get();
        }
        case MediaViewerItem::Kind::File:
        {
            return file_page_.get();
        }
    }
    return nullptr;
}

void MediaViewerOverlay::show_index_(std::size_t i)
{
    if (active_)
    {
        active_->deactivate();
        active_ = nullptr;
    }
    ++load_token_;
    index_ = i;
    MediaViewerPage* page = page_for_(items_[i].kind);
    if (!page)
    {
        is_open_ = false;
        return;
    }
    active_ = page;
    is_open_ = true;
    active_->activate(items_[i]);
    note_activity_();
    if (on_item_shown)
    {
        on_item_shown(items_[i], load_token_);
    }
    request_repaint_();
}

tk::Rect MediaViewerOverlay::image_rect() const
{
    return image_page_->image_rect();
}

tk::Rect MediaViewerOverlay::video_rect() const
{
    return video_page_->video_rect();
}

bool MediaViewerOverlay::is_loading() const
{
    return video_page_->is_loading();
}

// ── video delivery ───────────────────────────────────────────────────────

void MediaViewerOverlay::load_bytes(std::uint64_t token, const std::uint8_t* data,
                                    std::size_t size)
{
    if (token != load_token_ || active_ != video_page_.get())
    {
        return;
    }
    video_page_->load_bytes(data, size);
}

void MediaViewerOverlay::begin_stream_or_buffer(std::uint64_t token)
{
    if (token != load_token_ || active_ != video_page_.get())
    {
        return;
    }
    video_page_->begin_stream_or_buffer();
}

void MediaViewerOverlay::feed_stream_chunk(std::uint64_t token,
                                           const std::uint8_t* data,
                                           std::size_t size)
{
    if (token != load_token_ || active_ != video_page_.get())
    {
        return;
    }
    video_page_->feed_stream_chunk(data, size);
}

void MediaViewerOverlay::set_stream_length(std::uint64_t token,
                                           std::uint64_t total_size)
{
    if (token != load_token_ || active_ != video_page_.get())
    {
        return;
    }
    video_page_->set_stream_length(total_size);
}

void MediaViewerOverlay::end_stream(std::uint64_t token)
{
    if (token != load_token_ || active_ != video_page_.get())
    {
        return;
    }
    video_page_->end_stream();
}

void MediaViewerOverlay::fail_stream(std::uint64_t token)
{
    if (token != load_token_ || active_ != video_page_.get())
    {
        return;
    }
    video_page_->fail_stream();
}

void MediaViewerOverlay::load_audio_bytes(std::uint64_t token,
                                          std::vector<std::uint8_t> bytes)
{
    if (token != load_token_ || active_ != audio_page_.get())
    {
        return;
    }
    audio_page_->load_bytes(bytes.data(), bytes.size());
}

// ── players and providers ────────────────────────────────────────────────

void MediaViewerOverlay::set_video_player(std::unique_ptr<tk::VideoPlayer> player)
{
    video_page_->set_video_player(std::move(player));
}

void MediaViewerOverlay::set_audio_player(std::unique_ptr<tk::AudioPlayer> player)
{
    audio_page_->set_audio_player(std::move(player));
}

bool MediaViewerOverlay::has_audio_player() const
{
    return audio_page_->has_player();
}

std::size_t MediaViewerOverlay::memory_bytes() const
{
    return video_page_->memory_bytes() + audio_page_->memory_bytes();
}

void MediaViewerOverlay::set_image_provider(
    std::function<const tk::Image*(const std::string&)> fn)
{
    image_provider_ = std::move(fn);
}

tk::Button* MediaViewerOverlay::play_btn_for_test() const
{
    return video_page_->play_btn();
}

tk::Button* MediaViewerOverlay::speed_btn_for_test() const
{
    return video_page_->speed_btn();
}

tk::Button* MediaViewerOverlay::audio_play_btn_for_test() const
{
    return audio_page_->play_btn();
}

tk::Button* MediaViewerOverlay::audio_speed_btn_for_test() const
{
    return audio_page_->speed_btn();
}

tk::Button* MediaViewerOverlay::file_save_btn_for_test() const
{
    return file_page_->save_btn();
}

tk::AudioPlayer* MediaViewerOverlay::audio_player_for_test() const
{
    return audio_page_->player();
}

bool MediaViewerOverlay::audio_loading_for_test() const
{
    return audio_page_->is_loading();
}

bool MediaViewerOverlay::audio_error_for_test() const
{
    return audio_page_->has_error();
}

// ── page host ────────────────────────────────────────────────────────────

float MediaViewerOverlay::content_inset_x_() const
{
    return (items_.size() > 1 && !fullscreen_) ? kNavGutter : 0.0f;
}

tk::Rect MediaViewerOverlay::host_bounds() const
{
    const tk::Rect b = bounds();
    const float g = content_inset_x_();
    return {b.x + g, b.y, std::max(1.0f, b.w - 2.0f * g), b.h};
}

bool MediaViewerOverlay::host_fullscreen() const
{
    return fullscreen_;
}

bool MediaViewerOverlay::host_chrome_shown() const
{
    return chrome_shown_();
}

void MediaViewerOverlay::host_refresh_chrome_autohide()
{
    refresh_chrome_autohide_();
}

void MediaViewerOverlay::host_request_repaint()
{
    request_repaint_();
}

const tk::Image* MediaViewerOverlay::host_image(const std::string& key) const
{
    return image_provider_ ? image_provider_(key) : nullptr;
}

tk::Button* MediaViewerOverlay::host_add_icon_button()
{
    auto btn = tk::create_widget<tk::Button>(this, "", std::function<void()>{},
                                             tk::Button::Variant::Icon);
    return add_child(std::move(btn));
}

tk::Button* MediaViewerOverlay::host_add_label_button(std::string label)
{
    auto btn = tk::create_widget<tk::Button>(this, std::move(label),
                                             std::function<void()>{},
                                             tk::Button::Variant::Primary);
    return add_child(std::move(btn));
}

void MediaViewerOverlay::host_playback_started()
{
    if (on_playback_started)
    {
        on_playback_started();
    }
}

// ── layout ───────────────────────────────────────────────────────────────

tk::Size MediaViewerOverlay::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints; // fills the entire surface
}

void MediaViewerOverlay::arrange(tk::LayoutCtx& lc, tk::Rect b)
{
    tk::Widget::arrange(lc, b);
    if (active_)
    {
        active_->arrange(lc, host_bounds());
    }
    layout_chrome_(lc, b);
    layout_nav_(lc, b);
}

bool MediaViewerOverlay::nav_shown_() const
{
    return is_open_ && items_.size() > 1 && chrome_shown_();
}

void MediaViewerOverlay::layout_nav_(tk::LayoutCtx& lc, tk::Rect b)
{
    const float y = b.y + (b.h - kNavBtnSize) * 0.5f;
    prev_btn_->arrange(lc, {b.x + kNavBtnInset, y, kNavBtnSize, kNavBtnSize});
    next_btn_->arrange(
        lc, {b.x + b.w - kNavBtnInset - kNavBtnSize, y, kNavBtnSize, kNavBtnSize});
    const bool show = nav_shown_();
    prev_btn_->set_visible(show);
    next_btn_->set_visible(show);
}

std::string MediaViewerOverlay::counter_text_() const
{
    if (items_.size() < 2)
    {
        return {};
    }
    return tk::trf(tk::tr("{0} / {1}"),
                   {std::to_string(index_ + 1), std::to_string(items_.size())});
}

std::string MediaViewerOverlay::counter_text_for_test() const
{
    return counter_text_();
}

void MediaViewerOverlay::on_icon_scale_changed_()
{
    prev_icon_.reset();
    next_icon_.reset();
    counter_layout_.reset();
}

void MediaViewerOverlay::paint_nav_(tk::PaintCtx& ctx)
{
    if (!nav_shown_())
    {
        return;
    }
    auto& cv = ctx.canvas;
    const tk::Color tint = tk::Color::rgba(255, 255, 255, 220);

    cv.push_clip_rounded_rect(prev_btn_->bounds(), kNavBtnSize * 0.5f);
    prev_btn_->paint(ctx);
    cv.pop_clip();
    draw_icon_(ctx, prev_btn_->bounds(), kNavIconPx, prev_icon_, kChevronLeftSvg, tint);

    cv.push_clip_rounded_rect(next_btn_->bounds(), kNavBtnSize * 0.5f);
    next_btn_->paint(ctx);
    cv.pop_clip();
    draw_icon_(ctx, next_btn_->bounds(), kNavIconPx, next_icon_, kChevronRightSvg, tint);

    // Counter pill, top-left (mirrors the chrome cluster top-right).
    const std::string text = counter_text_();
    // The layout only depends on the text (and the DPI scale, which resets it
    // via on_icon_scale_changed_), so keep it across paints.
    if (!counter_layout_ || counter_layout_text_ != text)
    {
        tk::TextStyle st{};
        st.role = tk::FontRole::Body;
        counter_layout_ = ctx.factory.build_text(text, st);
        counter_layout_text_ = text;
    }
    const auto& lo = counter_layout_;
    if (lo)
    {
        const tk::Size sz = lo->measure();
        const tk::Rect b = bounds();
        const tk::Rect pill{b.x + 8.0f, b.y + 8.0f, sz.w + 2.0f * kCounterPadX,
                            kCounterH};
        cv.fill_rounded_rect(pill, kCounterH * 0.5f, kNavPillRest);
        cv.draw_text(*lo, {pill.x + kCounterPadX, pill.y + (kCounterH - sz.h) * 0.5f},
                     tk::Color::rgba(255, 255, 255, 230));
    }
}

void MediaViewerOverlay::on_fullscreen_changed_(bool fullscreen)
{
    if (active_)
    {
        active_->on_fullscreen_changed(fullscreen);
    }
    request_repaint_();
}

// ── paint ────────────────────────────────────────────────────────────────

void MediaViewerOverlay::paint(tk::PaintCtx& ctx)
{
    if (!is_open_ || !active_)
    {
        return;
    }

    // Recompute geometry here too — zoom/pan/hide-controls may have changed
    // since arrange.
    const tk::Rect b = bounds();
    tk::LayoutCtx lc{ctx.factory, ctx.theme};
    active_->arrange(lc, host_bounds());
    layout_chrome_(lc, b);
    layout_nav_(lc, b);

    // Keep icon_scale_ current for the chrome buttons drawn below.
    sync_icon_scale_(ctx);

    // scrim -> content -> controls -> chrome
    paint_scrim_(ctx);
    active_->paint_content(ctx);
    active_->paint_controls(ctx);
    paint_chrome_buttons_(ctx);
    paint_nav_(ctx);
}

// ── pointer / key events ─────────────────────────────────────────────────

bool MediaViewerOverlay::on_pointer_down(tk::Point local)
{
    return handle_pointer_down_(local);
}

void MediaViewerOverlay::on_pointer_up(tk::Point local, bool inside_self)
{
    handle_pointer_up_(local, inside_self);
}

void MediaViewerOverlay::on_pointer_drag(tk::Point local)
{
    if (active_)
    {
        // Pages convert local->world through host_bounds(), which is inset.
        active_->on_pointer_drag({local.x - content_inset_x_(), local.y});
    }
}

bool MediaViewerOverlay::on_wheel(tk::Point local, float dx, float dy,
                                  bool is_touchpad)
{
    if (!is_open_ || !active_)
    {
        return false;
    }
    return active_->on_wheel({local.x - content_inset_x_(), local.y}, dx, dy,
                             is_touchpad);
}

bool MediaViewerOverlay::on_content_pointer_down_(tk::Point world, tk::Point local)
{
    return active_ && active_->on_content_pointer_down(world, local);
}

bool MediaViewerOverlay::on_content_pointer_up_(tk::Point world, tk::Point local,
                                                bool inside_self)
{
    return active_ && active_->on_content_pointer_up(world, local, inside_self);
}

bool MediaViewerOverlay::on_content_key_(const tk::KeyEvent& e)
{
    // The page gets first refusal: video seeks on Left/Right, a zoomed image
    // pans on the arrows. Whatever it declines steps through the sequence.
    if (active_ && active_->on_key(e))
    {
        return true;
    }
    if (items_.size() > 1)
    {
        if (matches(ShortcutId::MediaPrev, e))
        {
            step_(-1);
            return true;
        }
        if (matches(ShortcutId::MediaNext, e))
        {
            step_(1);
            return true;
        }
    }
    return false;
}

// ── chrome ───────────────────────────────────────────────────────────────

void MediaViewerOverlay::fire_save_()
{
    if (on_save && index_ < items_.size())
    {
        on_save(items_[index_]);
    }
}

void MediaViewerOverlay::fire_copy_()
{
    if (on_copy && index_ < items_.size())
    {
        on_copy(items_[index_]);
    }
}

bool MediaViewerOverlay::wants_copy_button_() const
{
    return active_ && active_->wants_copy_button();
}

bool MediaViewerOverlay::extra_chrome_hovered_() const
{
    return (active_ && active_->controls_hovered()) ||
           (prev_btn_ && prev_btn_->hovered()) || (next_btn_ && next_btn_->hovered());
}

} // namespace tesseract::views
