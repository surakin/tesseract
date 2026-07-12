#include "ComposeBar.h"

#include "icons.h"
#include "format.h"
#include "tk/i18n.h"
#include "tk/svg.h"
#include "tk/theme.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <cstdio>
#include <utility>

namespace tesseract::views
{

namespace
{

constexpr float kComposeBarPadX = 8.0f;
constexpr float kComposeBarPadY = 8.0f;
constexpr float kButtonSide = 40.0f;
constexpr float kSendWidth = 64.0f;
constexpr float kComposeBarGap = 6.0f;
constexpr float kRemoveBtnSide = 24.0f;
constexpr float kRemoveBtnInset = 4.0f;

// Compose-bar background is a tint of the surface — sits below the
// message list and above the bottom edge. Border is a 1px hairline on
// the top edge separating from the timeline.
inline tk::Color bar_bg(const tk::Theme& t)
{
    return t.palette.chrome_bg;
}
inline tk::Color card_bg(const tk::Theme& t)
{
    return t.palette.compose_card_bg;
}

} // namespace

std::string ComposeBar::make_filename(const std::string& mime)
{
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t tt = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf), "clipboard-%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
                  tm.tm_min, tm.tm_sec);
    std::string ext = "bin";
    if (mime == "image/png")
    {
        ext = "png";
    }
    else if (mime == "image/jpeg")
    {
        ext = "jpg";
    }
    else if (mime == "image/webp")
    {
        ext = "webp";
    }
    else if (mime == "image/bmp")
    {
        ext = "bmp";
    }
    else if (mime == "image/gif")
    {
        ext = "gif";
    }
    return std::string(buf) + "." + ext;
}

ComposeBar::ComposeBar()
{
    auto emoji = std::make_unique<tk::Button>(
        // U+1F600 GRINNING FACE. We keep the glyph as the Button's label
        // (even though Icon variant doesn't paint it) so test scaffolding
        // that scans buttons by label can still find it; the glyph itself
        // is painted in ComposeBar::paint() at Title size — see below.
        std::string("\xF0\x9F\x98\x80"), std::function<void()>{},
        tk::Button::Variant::Icon);
    emoji->set_on_click(
        [this]
        {
            if (on_emoji)
            {
                on_emoji(emoji_rect_);
            }
        });
    emoji->set_min_size({kButtonSide, kButtonSide});
    emoji_btn_ = add_child(std::move(emoji));

    // Sticker button. Glyph: U+1F5BC FE0F FRAMED PICTURE — distinct from
    // the emoji face so the two icons are visually unambiguous. Same
    // Icon variant + Title-size glyph painted on top as the emoji button.
    auto sticker = std::make_unique<tk::Button>(
        std::string("\xF0\x9F\x96\xBC\xEF\xB8\x8F"), std::function<void()>{},
        tk::Button::Variant::Icon);
    sticker->set_on_click(
        [this]
        {
            if (on_sticker)
            {
                on_sticker(sticker_rect_);
            }
        });
    sticker->set_min_size({kButtonSide, kButtonSide});
    sticker_btn_ = add_child(std::move(sticker));

    auto send = std::make_unique<tk::Button>(tk::tr("Send"), std::function<void()>{},
                                             tk::Button::Variant::Primary);
    send->set_on_click(
        [this]
        {
            trigger_send();
        });
    send->set_min_size({kSendWidth, kButtonSide});
    send_btn_ = add_child(std::move(send));

    {
        auto b = std::make_unique<tk::Button>(
            std::string("\xF0\x9F\x8E\x99"), std::function<void()>{},
            tk::Button::Variant::Icon);
        b->set_on_click([this] { if (on_mic_clicked) on_mic_clicked(); });
        b->set_min_size({kButtonSide, kButtonSide});
        mic_btn_ = add_child(std::move(b));
    }

    auto remove = std::make_unique<tk::Button>(
        // ✕ small (U+2715)
        std::string("\xE2\x9C\x95"), std::function<void()>{},
        tk::Button::Variant::Icon);
    remove->set_on_click(
        [this]
        {
            clear_pending();
        });
    remove->set_min_size({kRemoveBtnSide, kRemoveBtnSide});
    remove->set_visible(false);
    remove_btn_ = add_child(std::move(remove));

    refresh_send_enabled();
}

void ComposeBar::trigger_send()
{
    if (recording_)
        return;
    if (pending_.has_value())
    {
        std::string reply_id = reply_event_id_;
        if (pending_->kind == PendingAttachment::Kind::Image)
        {
            if (on_send_image)
            {
                // Snapshot scalar fields before moves (evaluation order unspecified).
                std::uint32_t w = pending_->width;
                std::uint32_t h = pending_->height;
                bool anim = pending_->is_animated;
                on_send_image(
                    std::move(pending_->bytes), std::move(pending_->mime),
                    std::move(pending_->filename), current_text_, w, h, anim,
                    std::move(reply_id));
            }
        }
        else if (pending_->kind == PendingAttachment::Kind::Video)
        {
            if (on_send_video)
            {
                std::uint32_t w = pending_->width;
                std::uint32_t h = pending_->height;
                std::uint32_t tw = pending_->thumb_width;
                std::uint32_t th = pending_->thumb_height;
                std::uint64_t dur = pending_->duration_ms;
                on_send_video(
                    std::move(pending_->bytes), std::move(pending_->mime),
                    std::move(pending_->filename), current_text_, w, h,
                    std::move(pending_->thumb_bytes_raw), tw, th, dur,
                    std::move(reply_id));
            }
        }
        else if (pending_->kind == PendingAttachment::Kind::Audio)
        {
            if (on_send_audio)
            {
                std::uint64_t dur = pending_->duration_ms;
                on_send_audio(
                    std::move(pending_->bytes), std::move(pending_->mime),
                    std::move(pending_->filename), current_text_, dur,
                    std::move(reply_id));
            }
        }
        else
        {
            if (on_send_file)
            {
                on_send_file(std::move(pending_->bytes),
                             std::move(pending_->mime),
                             std::move(pending_->filename), current_text_,
                             std::move(reply_id));
            }
        }
        pending_.reset();
        file_name_layout_.reset();
        file_size_layout_.reset();
        file_layout_key_.clear();
        clear_reply();
        recompute_height();
        if (remove_btn_)
        {
            remove_btn_->set_visible(false);
        }
        refresh_send_enabled();
        if (on_size_changed)
        {
            on_size_changed();
        }
    }
    else if (has_editing())
    {
        std::string ev = edit_event_id_;
        std::string text = current_text_;
        clear_editing();
        if (on_send_edit)
        {
            on_send_edit(ev, text);
        }
    }
    else if (has_reply())
    {
        std::string id = reply_event_id_;
        std::string text = current_text_;
        clear_reply();
        if (on_send_reply)
        {
            on_send_reply(id, text);
        }
    }
    else
    {
        if (on_send)
        {
            on_send(current_text_);
        }
    }
}

void ComposeBar::set_text_area_natural_height(float h)
{
    text_area_natural_ = h;
    recompute_height();
}

void ComposeBar::recompute_height()
{
    float text_h =
        std::clamp(text_area_natural_ + kComposeBarPadY * 2, kMinHeight, kMaxHeight);
    float top_h = 0.0f;
    if (has_editing())
    {
        top_h = kEditBandH + kEditBandGap;
    }
    else if (has_reply())
    {
        top_h = kReplyBandH + kReplyBandGap;
    }
    float band_h = 0.0f;
    if (pending_.has_value())
    {
        const auto k = pending_->kind;
        if (k == PendingAttachment::Kind::File ||
            k == PendingAttachment::Kind::Audio ||
            (k == PendingAttachment::Kind::Video &&
             !pending_->preview && pending_->thumb_bytes_raw.empty()))
        {
            // Image/video-with-thumbnail float above the bar (no extra height).
            // Chips (file, audio, loading video) push the bar up instead.
            band_h = kFileBandH + kPreviewBandGap;
        }
    }
    // arrange() insets the first band kComposeBarPadY from the bar's top edge; add
    // that offset once when any band is present so natural_height_ matches
    // the full space that arrange() actually consumes.
    float total_bands = top_h + band_h;
    natural_height_ =
        text_h + total_bands + (total_bands > 0.0f ? kComposeBarPadY : 0.0f);
}

void ComposeBar::set_reply_to(std::string event_id, std::string sender_name,
                              std::string body_preview)
{
    reply_event_id_ = std::move(event_id);
    reply_sender_name_ = std::move(sender_name);
    // Collapse newlines to spaces: the reply band has a fixed height, so a
    // multiline original message must render as a single ellipsized line.
    for (char& c : body_preview)
    {
        if (c == '\n' || c == '\r')
            c = ' ';
    }
    reply_body_preview_ = std::move(body_preview);
    notify_size_changed_();
}

void ComposeBar::clear_reply()
{
    if (reply_event_id_.empty())
    {
        return;
    }
    reply_event_id_.clear();
    reply_sender_name_.clear();
    reply_body_preview_.clear();
    reply_band_rect_ = {};
    reply_cancel_rect_ = {};
    notify_size_changed_();
}

void ComposeBar::set_editing(std::string event_id)
{
    // Edit mode and reply mode are mutually exclusive — silently drop reply.
    reply_event_id_.clear();
    reply_sender_name_.clear();
    reply_body_preview_.clear();
    reply_band_rect_ = {};
    reply_cancel_rect_ = {};

    edit_event_id_ = std::move(event_id);
    notify_size_changed_();
}

void ComposeBar::clear_editing()
{
    if (edit_event_id_.empty())
    {
        return;
    }
    edit_event_id_.clear();
    edit_band_rect_ = {};
    edit_cancel_rect_ = {};
    notify_size_changed_();
}

void ComposeBar::set_current_text(std::string text)
{
    current_text_ = std::move(text);
    refresh_send_enabled();
}

void ComposeBar::on_theme_changed(const tk::Theme& t)
{
    if (auto area = native_text_area_.lock())
    {
        area->set_text_color(t.palette.text_primary);
        area->set_mention_colors(t.palette.accent, t.palette.text_on_accent);
    }
}

void ComposeBar::set_enabled(bool e)
{
    if (e != enabled())
    {
        tk::Widget::set_enabled(e);
        if (emoji_btn_)
        {
            emoji_btn_->set_enabled(e);
        }
        if (sticker_btn_)
        {
            sticker_btn_->set_enabled(e);
        }
        if (remove_btn_)
        {
            remove_btn_->set_enabled(e);
        }
        refresh_send_enabled();
    }
}

void ComposeBar::set_mic_available(bool available)
{
    if (mic_available_ == available)
        return;
    mic_available_ = available;
    if (mic_btn_)
        mic_btn_->set_visible(available);
    recompute_height();
    if (on_size_changed)
        on_size_changed();
}

void ComposeBar::set_recording(bool recording)
{
    if (recording_ == recording)
        return;
    recording_ = recording;
    if (recording)
    {
        waveform_samples_.clear();
        recording_start_ms_ = static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }
    elapsed_layout_.reset();
    // Fire on_size_changed so the host relayouts and updates the NativeTextArea
    // visibility (hidden while recording, restored when done).
    if (on_size_changed)
        on_size_changed();
}

void ComposeBar::push_amplitude(std::uint16_t amplitude)
{
    if (!recording_)
        return;
    if (waveform_samples_.size() >= kMaxWaveformSamples)
        waveform_samples_.erase(waveform_samples_.begin());
    waveform_samples_.push_back(amplitude);
    // Visual-only change: host repaints on next cycle.
}

void ComposeBar::set_pending_image(std::vector<std::uint8_t> bytes,
                                   std::string mime, std::string filename,
                                   bool is_animated)
{
    ++pending_gen_;
    PendingAttachment pa;
    pa.kind = PendingAttachment::Kind::Image;
    pa.bytes = std::move(bytes);
    pa.mime = std::move(mime);
    pa.filename =
        filename.empty() ? make_filename(pa.mime) : std::move(filename);
    pa.is_animated = is_animated;
    pending_ = std::move(pa);
    file_name_layout_.reset();
    file_size_layout_.reset();
    file_layout_key_.clear();
    // Dimensions + preview are filled in lazily on the next arrange()
    // pass (we don't have a CanvasFactory here).
    recompute_height();
    if (remove_btn_)
    {
        remove_btn_->set_visible(true);
    }
    refresh_send_enabled();
    if (on_size_changed)
    {
        on_size_changed();
    }
}

void ComposeBar::set_pending_file(std::vector<std::uint8_t> bytes,
                                  std::string mime, std::string filename)
{
    ++pending_gen_;
    PendingAttachment pa;
    pa.kind = PendingAttachment::Kind::File;
    pa.bytes = std::move(bytes);
    pa.mime = std::move(mime);
    pa.filename = std::move(filename);
    pending_ = std::move(pa);
    file_name_layout_.reset();
    file_size_layout_.reset();
    file_layout_key_.clear();
    recompute_height();
    if (remove_btn_)
    {
        remove_btn_->set_visible(true);
    }
    refresh_send_enabled();
    if (on_size_changed)
    {
        on_size_changed();
    }
}

void ComposeBar::set_pending_video(std::vector<std::uint8_t> bytes,
                                   std::string mime, std::string filename)
{
    ++pending_gen_;
    PendingAttachment pa;
    pa.kind = PendingAttachment::Kind::Video;
    pa.loading = true;
    pa.bytes = std::move(bytes);
    pa.mime = std::move(mime);
    pa.filename = std::move(filename);
    pending_ = std::move(pa);
    file_name_layout_.reset();
    file_size_layout_.reset();
    file_layout_key_.clear();
    video_badge_layout_.reset();
    recompute_height();
    if (remove_btn_)
        remove_btn_->set_visible(true);
    refresh_send_enabled();
    if (on_size_changed)
        on_size_changed();
}

void ComposeBar::set_pending_audio(std::vector<std::uint8_t> bytes,
                                   std::string mime, std::string filename)
{
    ++pending_gen_;
    PendingAttachment pa;
    pa.kind = PendingAttachment::Kind::Audio;
    pa.loading = true;
    pa.bytes = std::move(bytes);
    pa.mime = std::move(mime);
    pa.filename = std::move(filename);
    pending_ = std::move(pa);
    file_name_layout_.reset();
    file_size_layout_.reset();
    file_layout_key_.clear();
    recompute_height();
    if (remove_btn_)
        remove_btn_->set_visible(true);
    refresh_send_enabled();
    if (on_size_changed)
        on_size_changed();
}

void ComposeBar::update_pending_attachment(const MediaInfo& info)
{
    if (!pending_.has_value())
        return;
    if (info.pending_gen != pending_gen_)
        return; // stale result — user replaced or removed the attachment
    switch (pending_->kind)
    {
    case PendingAttachment::Kind::Video:
        pending_->width = info.video_w;
        pending_->height = info.video_h;
        pending_->thumb_bytes_raw = info.thumb_bytes;
        pending_->thumb_width = info.thumb_w;
        pending_->thumb_height = info.thumb_h;
        pending_->duration_ms = info.duration_ms;
        pending_->preview.reset(); // decoded lazily from thumb_bytes_raw in arrange()
        break;
    case PendingAttachment::Kind::Audio:
        pending_->duration_ms = info.duration_ms;
        break;
    case PendingAttachment::Kind::Image:
        pending_->is_animated = info.is_animated;
        break;
    default:
        break;
    }
    pending_->loading = false;
    file_name_layout_.reset(); // force duration text re-layout
    file_size_layout_.reset();
    file_layout_key_.clear();
    notify_size_changed_();
}

void ComposeBar::clear_pending()
{
    if (!pending_.has_value())
    {
        return;
    }
    ++pending_gen_;
    pending_.reset();
    file_name_layout_.reset();
    file_size_layout_.reset();
    file_layout_key_.clear();
    recompute_height();
    if (remove_btn_)
    {
        remove_btn_->set_visible(false);
    }
    refresh_send_enabled();
    if (on_size_changed)
    {
        on_size_changed();
    }
}

void ComposeBar::notify_size_changed_()
{
    recompute_height();
    if (on_size_changed)
        on_size_changed();
}

void ComposeBar::rebuild_chip_layouts_(tk::LayoutCtx& ctx, const std::string& key,
                                        const std::string& secondary_text)
{
    if (file_layout_key_ == key)
        return;
    tk::TextStyle name_style{};
    name_style.role = tk::FontRole::Body;
    file_name_layout_ = ctx.factory.build_text(pending_->filename, name_style);
    tk::TextStyle size_style{};
    size_style.role = tk::FontRole::Small;
    file_size_layout_ = ctx.factory.build_text(secondary_text, size_style);
    file_layout_key_ = key;
}

void ComposeBar::paint_two_line_chip_(tk::PaintCtx& ctx) const
{
    if (!file_name_layout_ || !file_size_layout_)
        return;
    constexpr float kChipPadX = 12.0f;
    float text_x = preview_band_rect_.x + kChipPadX;
    tk::Size name_sz = file_name_layout_->measure();
    tk::Size size_sz = file_size_layout_->measure();
    float total_h = name_sz.h + size_sz.h;
    float ty = preview_band_rect_.y + (preview_band_rect_.h - total_h) * 0.5f;
    ctx.canvas.draw_text(*file_name_layout_, {text_x, ty},
                         ctx.theme.palette.text_primary);
    ctx.canvas.draw_text(*file_size_layout_, {text_x, ty + name_sz.h},
                         ctx.theme.palette.text_secondary);
}

void ComposeBar::refresh_send_enabled()
{
    if (!send_btn_)
    {
        return;
    }
    bool any_text = false;
    for (char c : current_text_)
    {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
        {
            any_text = true;
            break;
        }
    }
    send_btn_->set_enabled(enabled_ && (any_text || pending_.has_value()));
}

tk::Size ComposeBar::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return {constraints.w, natural_height_};
}

void ComposeBar::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    // ── Decode the pending image lazily (now that we have a factory) ──
    if (pending_.has_value() &&
        pending_->kind == PendingAttachment::Kind::Image && !pending_->preview)
    {
        auto img = ctx.factory.decode_image(std::span<const std::uint8_t>(
            pending_->bytes.data(), pending_->bytes.size()));
        if (img)
        {
            pending_->width = static_cast<std::uint32_t>(img->width());
            pending_->height = static_cast<std::uint32_t>(img->height());
            constexpr int kMaxPx = static_cast<int>(kPreviewBandH) * 4;
            if (auto scaled = ctx.factory.scale_image(*img, kMaxPx, kMaxPx))
                pending_->preview = std::move(scaled);
            else
                pending_->preview = std::move(img);
        }
    }

    // ── Decode animated frames once is_animated is confirmed ────────────────
    if (pending_.has_value() &&
        pending_->kind == PendingAttachment::Kind::Image &&
        pending_->is_animated && !pending_->anim_preview)
    {
        constexpr int kMaxPx = static_cast<int>(kPreviewBandH) * 4;
        pending_->anim_preview = ctx.factory.decode_animated_image(
            std::span<const std::uint8_t>(pending_->bytes.data(),
                                          pending_->bytes.size()),
            kMaxPx);
        if (pending_->anim_preview)
        {
            pending_->width =
                static_cast<std::uint32_t>(pending_->anim_preview->width());
            pending_->height =
                static_cast<std::uint32_t>(pending_->anim_preview->height());
        }
    }

    // ── Decode video thumbnail lazily once extraction fills thumb_bytes_raw ──
    if (pending_.has_value() &&
        pending_->kind == PendingAttachment::Kind::Video &&
        !pending_->preview && !pending_->thumb_bytes_raw.empty())
    {
        auto img = ctx.factory.decode_image(std::span<const std::uint8_t>(
            pending_->thumb_bytes_raw.data(),
            pending_->thumb_bytes_raw.size()));
        if (img)
        {
            pending_->thumb_width = static_cast<std::uint32_t>(img->width());
            pending_->thumb_height = static_cast<std::uint32_t>(img->height());
            constexpr int kMaxPx = static_cast<int>(kPreviewBandH) * 4;
            if (auto scaled = ctx.factory.scale_image(*img, kMaxPx, kMaxPx))
                pending_->preview = std::move(scaled);
            else
                pending_->preview = std::move(img);
        }
    }

    // ── Build (or refresh) cached text layouts for file/video/audio chips ──
    if (pending_.has_value() && pending_->kind == PendingAttachment::Kind::File)
    {
        std::string key =
            pending_->filename + "|" + std::to_string(pending_->bytes.size());
        rebuild_chip_layouts_(ctx, key,
            format_size(static_cast<std::uint64_t>(pending_->bytes.size())));
    }

    // Video chip: filename + size (no thumbnail yet) or thumbnail (no chip).
    if (pending_.has_value() &&
        pending_->kind == PendingAttachment::Kind::Video &&
        !pending_->preview)
    {
        std::string key =
            pending_->filename + "|" + std::to_string(pending_->bytes.size());
        std::string size_str =
            format_size(static_cast<std::uint64_t>(pending_->bytes.size()));
        rebuild_chip_layouts_(ctx, key, pending_->loading ? "…" : size_str);
    }

    // Audio chip: filename + duration (or "…" while loading).
    if (pending_.has_value() &&
        pending_->kind == PendingAttachment::Kind::Audio)
    {
        std::string key =
            pending_->filename + "|" + std::to_string(pending_->duration_ms) +
            (pending_->loading ? "L" : "");
        std::string dur_str;
        if (pending_->loading)
        {
            dur_str = "…";
        }
        else if (pending_->duration_ms > 0)
        {
            std::uint64_t secs = pending_->duration_ms / 1000;
            char buf[48];  // worst case: two 20-digit uint64 fields + ':' + NUL
            std::snprintf(buf, sizeof(buf), "%llu:%02llu",
                          static_cast<unsigned long long>(secs / 60),
                          static_cast<unsigned long long>(secs % 60));
            dur_str = buf;
        }
        rebuild_chip_layouts_(ctx, key, dur_str);
    }

    // ── Top banner (edit mode XOR reply mode — topmost when active) ──
    float text_top = bounds.y;
    if (has_editing())
    {
        edit_band_rect_ = {bounds.x + kComposeBarPadX, bounds.y + kComposeBarPadY,
                           std::max(0.0f, bounds.w - kComposeBarPadX * 2), kEditBandH};
        constexpr float kCancelSide = 20.0f;
        constexpr float kCancelInsetX = 8.0f;
        edit_cancel_rect_ = {
            edit_band_rect_.x + edit_band_rect_.w - kCancelSide - kCancelInsetX,
            edit_band_rect_.y + (kEditBandH - kCancelSide) * 0.5f, kCancelSide,
            kCancelSide};
        text_top = edit_band_rect_.y + edit_band_rect_.h + kEditBandGap;
        reply_band_rect_ = {};
        reply_cancel_rect_ = {};
    }
    else if (has_reply())
    {
        reply_band_rect_ = {bounds.x + kComposeBarPadX, bounds.y + kComposeBarPadY,
                            std::max(0.0f, bounds.w - kComposeBarPadX * 2), kReplyBandH};
        constexpr float kCancelSide = 20.0f;
        constexpr float kCancelInsetX = 8.0f;
        reply_cancel_rect_ = {reply_band_rect_.x + reply_band_rect_.w -
                                  kCancelSide - kCancelInsetX,
                              reply_band_rect_.y +
                                  (kReplyBandH - kCancelSide) * 0.5f,
                              kCancelSide, kCancelSide};
        text_top = reply_band_rect_.y + reply_band_rect_.h + kReplyBandGap;
        edit_band_rect_ = {};
        edit_cancel_rect_ = {};
    }
    else
    {
        reply_band_rect_ = {};
        reply_cancel_rect_ = {};
        edit_band_rect_ = {};
        edit_cancel_rect_ = {};
    }

    // ── Attachment band ───────────────────────────────────────────────
    if (pending_.has_value())
    {
        const bool is_floating =
            pending_->kind == PendingAttachment::Kind::Image ||
            (pending_->kind == PendingAttachment::Kind::Video &&
             pending_->preview);

        if (is_floating)
        {
            // Image / video-with-thumbnail: preview floats ABOVE the bar.
            constexpr float kDisplayH = kPreviewBandH * 1.25f;
            float img_w = static_cast<float>(
                pending_->kind == PendingAttachment::Kind::Video
                    ? pending_->thumb_width
                    : pending_->width);
            float img_h = static_cast<float>(
                pending_->kind == PendingAttachment::Kind::Video
                    ? pending_->thumb_height
                    : pending_->height);
            float max_w = std::max(0.0f, bounds.w - kComposeBarPadX * 2);
            float dw, dh;
            if (img_w <= 0 || img_h <= 0)
            {
                dw = max_w;
                dh = kDisplayH;
            }
            else
            {
                float s = std::min(max_w / img_w, kDisplayH / img_h);
                dw = img_w * s;
                dh = img_h * s;
            }
            preview_band_rect_ = {bounds.x + kComposeBarPadX,
                                  bounds.y - dh - kPreviewBandGap, dw, dh};
            preview_image_rect_ = preview_band_rect_;
            remove_btn_rect_ = {
                preview_band_rect_.x + preview_band_rect_.w -
                    kRemoveBtnSide - kRemoveBtnInset,
                preview_band_rect_.y + kRemoveBtnInset,
                kRemoveBtnSide, kRemoveBtnSide};
            // text_top is NOT advanced — bar height is unchanged.
        }
        else
        {
            // File / video-loading / audio chip stays inside the bar.
            float band_y = (has_reply() || has_editing()) ? text_top
                                                          : bounds.y + kComposeBarPadY;
            preview_band_rect_ = {bounds.x + kComposeBarPadX, band_y,
                                  std::max(0.0f, bounds.w - kComposeBarPadX * 2),
                                  kFileBandH};
            preview_image_rect_ = {};
            remove_btn_rect_ = {
                preview_band_rect_.x + preview_band_rect_.w -
                    kRemoveBtnSide - kRemoveBtnInset,
                preview_band_rect_.y +
                    (preview_band_rect_.h - kRemoveBtnSide) * 0.5f,
                kRemoveBtnSide, kRemoveBtnSide};
            text_top =
                preview_band_rect_.y + preview_band_rect_.h + kPreviewBandGap;
        }
    }
    else
    {
        preview_band_rect_ = {};
        preview_image_rect_ = {};
        remove_btn_rect_ = {};
    }

    float text_strip_h = bounds.y + bounds.h - text_top;

    // Send button sits to the right, outside the input card.
    send_rect_ = {bounds.x + bounds.w - kComposeBarPadX - kSendWidth,
                  text_top + (text_strip_h - kButtonSide) * 0.5f, kSendWidth,
                  kButtonSide};

    // Compose card always spans from the left edge to just before send.
    const float card_left = bounds.x + kComposeBarPadX;
    const float card_right = send_rect_.x - kComposeBarGap;
    compose_card_rect_ = {card_left, text_top + kComposeBarPadY,
                          std::max(0.0f, card_right - card_left),
                          std::max(0.0f, text_strip_h - kComposeBarPadY * 2)};

    // Icon buttons live inside the card, stacked right-to-left.
    const float btn_y = text_top + (text_strip_h - kButtonSide) * 0.5f;
    float inner_right = card_right;
    if (mic_available_)
    {
        mic_btn_rect_ = {inner_right - kButtonSide, btn_y, kButtonSide,
                         kButtonSide};
        inner_right = mic_btn_rect_.x - kComposeBarGap;
    }
    else
    {
        mic_btn_rect_ = {};
    }
    sticker_rect_ = {inner_right - kButtonSide, btn_y, kButtonSide, kButtonSide};
    emoji_rect_ = {sticker_rect_.x - kComposeBarGap - kButtonSide, btn_y, kButtonSide,
                   kButtonSide};

    // Text area occupies the left portion of the card, leaving room for
    // the emoji/sticker buttons on the right with a small gap.
    text_area_rect_ = {
        card_left + kComposeBarPadX, text_top + kComposeBarPadY,
        std::max(0.0f, emoji_rect_.x - kComposeBarGap - (card_left + kComposeBarPadX)),
        std::max(0.0f, text_strip_h - kComposeBarPadY * 2)};

    // Waveform strip occupies the card between the × cancel button (left) and
    // the ⏹ stop button (right). Emoji/sticker buttons are hidden while recording.
    constexpr float kVoiceCancelSide = 28.0f;
    voice_cancel_rect_ = {card_left,
                          text_top + (text_strip_h - kVoiceCancelSide) * 0.5f,
                          kVoiceCancelSide, kVoiceCancelSide};
    const float wave_right = (mic_available_ && !mic_btn_rect_.empty())
                                 ? mic_btn_rect_.x - kComposeBarGap
                                 : card_right;
    waveform_strip_rect_ = {
        card_left + kVoiceCancelSide + kComposeBarGap, text_top + kComposeBarPadY,
        std::max(0.0f, wave_right - card_left - kVoiceCancelSide - kComposeBarGap),
        std::max(0.0f, text_strip_h - kComposeBarPadY * 2)};

    if (emoji_btn_)
    {
        emoji_btn_->set_visible(!recording_);
        emoji_btn_->arrange(ctx, recording_ ? tk::Rect{} : emoji_rect_);
    }
    if (sticker_btn_)
    {
        sticker_btn_->set_visible(!recording_);
        sticker_btn_->arrange(ctx, recording_ ? tk::Rect{} : sticker_rect_);
    }
    if (mic_btn_)
    {
        mic_btn_->set_visible(mic_available_);
        mic_btn_->arrange(ctx, mic_btn_rect_.empty() ? tk::Rect{} : mic_btn_rect_);
    }
    if (send_btn_)
    {
        send_btn_->arrange(ctx, send_rect_);
    }
    if (remove_btn_ && pending_.has_value())
    {
        remove_btn_->arrange(ctx, remove_btn_rect_);
    }
}

void ComposeBar::paint(tk::PaintCtx& ctx)
{
    host_ = ctx.host;
    ctx.canvas.fill_rect(bounds_, bar_bg(ctx.theme));
    // 1 px top hairline so the bar reads as a separate strip from the
    // message list above it.
    tk::Rect hairline{bounds_.x, bounds_.y, bounds_.w, 1.0f};
    ctx.canvas.fill_rect(hairline, ctx.theme.palette.border);

    if (pending_.has_value())
    {
        // Subtle card behind the preview band, with a thin border.
        ctx.canvas.fill_rounded_rect(preview_band_rect_, 8.0f,
                                     card_bg(ctx.theme));
        ctx.canvas.stroke_rounded_rect(preview_band_rect_, 8.0f,
                                       ctx.theme.palette.border, 1.0f);
        if (pending_->kind == PendingAttachment::Kind::Image)
        {
            constexpr float kImgInset = 1.0f;
            tk::Rect img_rect{
                preview_image_rect_.x + kImgInset,
                preview_image_rect_.y + kImgInset,
                std::max(0.0f, preview_image_rect_.w - kImgInset * 2),
                std::max(0.0f, preview_image_rect_.h - kImgInset * 2)};

            if (pending_->anim_preview)
            {
                ctx.canvas.draw_image(pending_->anim_preview->current_frame(),
                                      img_rect);
                if (on_request_anim_repaint_)
                    on_request_anim_repaint_(
                        pending_->anim_preview->ms_until_next_frame());
            }
            else if (pending_->preview)
            {
                ctx.canvas.draw_image(*pending_->preview, img_rect);
            }
        }
        else if (pending_->kind == PendingAttachment::Kind::Video)
        {
            if (pending_->preview)
            {
                // Video thumbnail band (same layout as image).
                constexpr float kImgInset = 1.0f;
                tk::Rect img_rect{
                    preview_image_rect_.x + kImgInset,
                    preview_image_rect_.y + kImgInset,
                    std::max(0.0f, preview_image_rect_.w - kImgInset * 2),
                    std::max(0.0f, preview_image_rect_.h - kImgInset * 2)};
                ctx.canvas.draw_image(*pending_->preview, img_rect);

                // ▶ badge: dark rounded rect in bottom-right corner.
                constexpr float kBadgeSize = 20.0f;
                constexpr float kBadgePad = 4.0f;
                tk::Rect badge{
                    img_rect.x + img_rect.w - kBadgeSize - kBadgePad,
                    img_rect.y + img_rect.h - kBadgeSize - kBadgePad,
                    kBadgeSize, kBadgeSize};
                ctx.canvas.fill_rounded_rect(badge, 4.0f,
                                             tk::Color::rgba(0, 0, 0, 160));
                if (!video_badge_layout_)
                {
                    tk::TextStyle ts{};
                    ts.role = tk::FontRole::Small;
                    // U+25B6 BLACK RIGHT-POINTING TRIANGLE
                    video_badge_layout_ =
                        ctx.factory.build_text("\xe2\x96\xb6", ts);
                }
                if (video_badge_layout_)
                {
                    tk::Size gs = video_badge_layout_->measure();
                    ctx.canvas.draw_text(
                        *video_badge_layout_,
                        {badge.x + (badge.w - gs.w) * 0.5f,
                         badge.y + (badge.h - gs.h) * 0.5f},
                        tk::Color::rgba(255, 255, 255, 220));
                }
            }
            else
            {
                // Loading chip: filename + size/ellipsis.
                paint_two_line_chip_(ctx);
            }
        }
        else if (pending_->kind == PendingAttachment::Kind::Audio)
        {
            // Audio chip: filename + duration (or "…" while loading).
            paint_two_line_chip_(ctx);
        }
        else
        {
            // File chip: filename + size. Layout in arrange().
            paint_two_line_chip_(ctx);
        }
    }

    // ── Edit mode banner ─────────────────────────────────────────────
    if (has_editing() && !edit_band_rect_.empty())
    {
        constexpr float kAccentW = 3.0f;
        constexpr float kEditPadX = 8.0f;
        constexpr float kEditPadY = 5.0f;

        ctx.canvas.fill_rounded_rect(edit_band_rect_, 6.0f, card_bg(ctx.theme));
        ctx.canvas.push_clip_rounded_rect(edit_band_rect_, 6.0f);
        ctx.canvas.fill_rect(
            {edit_band_rect_.x, edit_band_rect_.y, kAccentW, edit_band_rect_.h},
            ctx.theme.palette.accent);
        ctx.canvas.pop_clip();

        float text_x = edit_band_rect_.x + kAccentW + kEditPadX;

        tk::TextStyle label_style{};
        label_style.role = tk::FontRole::Small;
        auto label_layout =
            ctx.factory.build_text(tk::tr("Editing message"), label_style);
        if (label_layout)
        {
            tk::Size sz = label_layout->measure();
            float text_y = edit_band_rect_.y +
                           (edit_band_rect_.h - sz.h) * 0.5f;
            ctx.canvas.draw_text(*label_layout, {text_x, text_y},
                                 ctx.theme.palette.text_secondary);
        }

        if (!edit_cancel_rect_.empty())
        {
            tk::TextStyle x_style{};
            x_style.role = tk::FontRole::Body;
            auto x_layout =
                ctx.factory.build_text(std::string("\xC3\x97"), x_style);
            if (x_layout)
            {
                tk::Size sz = x_layout->measure();
                ctx.canvas.draw_text(
                    *x_layout,
                    {edit_cancel_rect_.x + (edit_cancel_rect_.w - sz.w) * 0.5f,
                     edit_cancel_rect_.y + (edit_cancel_rect_.h - sz.h) * 0.5f},
                    press_edit_cancel_ ? ctx.theme.palette.text_primary
                                       : ctx.theme.palette.text_muted);
            }
        }
    }

    // ── Reply preview banner ──────────────────────────────────────────
    if (has_reply() && !reply_band_rect_.empty())
    {
        constexpr float kAccentW = 3.0f;
        constexpr float kReplyPadX = 8.0f;

        ctx.canvas.fill_rounded_rect(reply_band_rect_, 6.0f,
                                     card_bg(ctx.theme));
        ctx.canvas.push_clip_rounded_rect(reply_band_rect_, 6.0f);
        ctx.canvas.fill_rect({reply_band_rect_.x, reply_band_rect_.y, kAccentW,
                              reply_band_rect_.h},
                             ctx.theme.palette.accent);
        ctx.canvas.pop_clip();

        float text_x = reply_band_rect_.x + kAccentW + kReplyPadX;
        // Keep text clear of the trailing "×" cancel button.
        float text_w = std::max(0.0f, reply_cancel_rect_.x - text_x -
                                          kReplyPadX);

        // Build both lines first so we can measure heights and centre the
        // pair vertically within the band. Both are single-line, ellipsized,
        // and width-bounded so a multiline/long original can't overflow.
        tk::TextStyle label_style{};
        label_style.role = tk::FontRole::Caption;
        label_style.trim = tk::TextTrim::Ellipsis;
        label_style.max_width = text_w;
        auto label_layout = ctx.factory.build_text(
            tk::trf(tk::tr("Replying to {0}"), {reply_sender_name_}),
            label_style);

        tk::TextStyle body_style{};
        body_style.role = tk::FontRole::Caption;
        body_style.trim = tk::TextTrim::Ellipsis;
        body_style.max_width = text_w;
        auto body_layout =
            ctx.factory.build_text(reply_body_preview_, body_style);

        constexpr float kLineGap = 2.0f;
        float label_h = label_layout ? label_layout->measure().h : 0.0f;
        float body_h = body_layout ? body_layout->measure().h : 0.0f;
        float total_h = label_h + (body_h > 0.0f ? kLineGap + body_h : 0.0f);
        float text_y = reply_band_rect_.y + (kReplyBandH - total_h) * 0.5f;

        if (label_layout)
        {
            ctx.canvas.draw_text(*label_layout, {text_x, text_y},
                                 ctx.theme.palette.text_secondary);
        }
        if (body_layout)
        {
            ctx.canvas.draw_text(*body_layout,
                                 {text_x, text_y + label_h + kLineGap},
                                 ctx.theme.palette.text_muted);
        }

        // "×" cancel glyph centred in reply_cancel_rect_
        if (!reply_cancel_rect_.empty())
        {
            tk::TextStyle x_style{};
            x_style.role = tk::FontRole::Body;
            // U+00D7 MULTIPLICATION SIGN: C3 97
            auto x_layout =
                ctx.factory.build_text(std::string("\xC3\x97"), x_style);
            if (x_layout)
            {
                tk::Size sz = x_layout->measure();
                ctx.canvas.draw_text(*x_layout,
                                     {reply_cancel_rect_.x +
                                          (reply_cancel_rect_.w - sz.w) * 0.5f,
                                      reply_cancel_rect_.y +
                                          (reply_cancel_rect_.h - sz.h) * 0.5f},
                                     press_reply_cancel_
                                         ? ctx.theme.palette.text_primary
                                         : ctx.theme.palette.text_muted);
            }
        }
    }

    // Draw the compose card: a rounded rect that contains both the text
    // area (left) and the emoji/sticker icon buttons (right).  The host
    // overlays the NativeTextArea on top of text_area_rect_ which lives
    // inside this card, so the card fill provides the input background.
    if (!compose_card_rect_.empty())
    {
        ctx.canvas.fill_rounded_rect(compose_card_rect_, 6.0f,
                                     card_bg(ctx.theme));
        ctx.canvas.stroke_rounded_rect(compose_card_rect_, 6.0f,
                                       ctx.theme.palette.border, 1.0f);
    }

    if (emoji_btn_ && !recording_)
    {
        emoji_btn_->paint(ctx);
    }
    if (sticker_btn_ && !recording_)
    {
        sticker_btn_->paint(ctx);
    }
    if (mic_btn_ && mic_available_ && !mic_btn_rect_.empty())
    {
        mic_btn_->paint(ctx);
    }
    if (send_btn_)
    {
        send_btn_->paint(ctx);
    }

    // ── Voice recording waveform strip ──────────────────────────────────
    if (recording_ && !waveform_strip_rect_.empty())
    {
        // Cancel × button
        if (!voice_cancel_rect_.empty())
        {
            tk::TextStyle x_style{};
            x_style.role = tk::FontRole::Body;
            auto x_layout =
                ctx.factory.build_text(std::string("\xC3\x97"), x_style);
            if (x_layout)
            {
                tk::Size sz = x_layout->measure();
                ctx.canvas.draw_text(
                    *x_layout,
                    {voice_cancel_rect_.x +
                         (voice_cancel_rect_.w - sz.w) * 0.5f,
                     voice_cancel_rect_.y +
                         (voice_cancel_rect_.h - sz.h) * 0.5f},
                    press_voice_cancel_ ? ctx.theme.palette.text_primary
                                        : ctx.theme.palette.text_muted);
            }
        }

        // Amplitude bars
        const float center_y =
            waveform_strip_rect_.y + waveform_strip_rect_.h / 2.0f;
        const float max_h = waveform_strip_rect_.h * 0.8f;
        constexpr float kBarW = 3.0f;
        constexpr float kBarGap = 2.0f;
        std::uint16_t wf_peak = 0;
        for (std::uint16_t v : waveform_samples_) wf_peak = std::max(wf_peak, v);
        const float wf_norm =
            wf_peak > 0 ? 1.0f / static_cast<float>(wf_peak) : 0.0f;

        float x = waveform_strip_rect_.x;
        for (std::uint16_t amp : waveform_samples_)
        {
            float h = std::max(2.0f, static_cast<float>(amp) * wf_norm * max_h);
            ctx.canvas.fill_rect({x, center_y - h / 2.0f, kBarW, h},
                                 ctx.theme.palette.accent);
            x += kBarW + kBarGap;
            if (x > waveform_strip_rect_.right())
                break;
        }

        // Elapsed timer label
        using namespace std::chrono;
        auto now_ms = static_cast<std::uint64_t>(
            duration_cast<milliseconds>(
                steady_clock::now().time_since_epoch())
                .count());
        std::uint64_t elapsed_s =
            (now_ms > recording_start_ms_)
                ? (now_ms - recording_start_ms_) / 1000
                : 0;
        char buf[48];  // worst case: '●' prefix + two 20-digit uint64 + ':' + NUL
        std::snprintf(buf, sizeof(buf), "\xe2\x97\x8f %llu:%02llu",
                      static_cast<unsigned long long>(elapsed_s / 60),
                      static_cast<unsigned long long>(elapsed_s % 60));
        tk::TextStyle timer_style{};
        timer_style.role = tk::FontRole::Body;
        elapsed_layout_ = ctx.factory.build_text(buf, timer_style);
        if (elapsed_layout_)
        {
            float lx = waveform_strip_rect_.right() -
                       elapsed_layout_->measure().w - 4.0f;
            float ly = center_y - elapsed_layout_->ascent() / 2.0f;
            ctx.canvas.draw_text(*elapsed_layout_, {lx, ly},
                                 ctx.theme.palette.text_secondary);
        }
    }
    if (remove_btn_ && pending_.has_value() && !remove_btn_rect_.empty())
    {
        const bool on_image =
            pending_->kind == PendingAttachment::Kind::Image ||
            (pending_->kind == PendingAttachment::Kind::Video &&
             pending_->preview);
        if (on_image)
        {
            // Dark semi-transparent badge so the × is legible on any image.
            constexpr float kRadius = kRemoveBtnSide * 0.5f;
            tk::Color badge = remove_btn_->hovered()
                                  ? tk::Color::rgba(0, 0, 0, 160)
                                  : tk::Color::rgba(0, 0, 0, 110);
            ctx.canvas.fill_rounded_rect(remove_btn_rect_, kRadius, badge);
        }
        else
        {
            // File chip: use the standard Icon-button hover background.
            remove_btn_->paint(ctx);
        }
        if (!remove_layout_)
        {
            tk::TextStyle xs{};
            xs.role = tk::FontRole::UiSemibold;
            remove_layout_ = ctx.factory.build_text("\xc3\x97", xs);
        }
        if (remove_layout_)
        {
            tk::Size sz = remove_layout_->measure();
            float gx = remove_btn_rect_.x + (remove_btn_rect_.w - sz.w) * 0.5f;
            float gy = remove_btn_rect_.y + (remove_btn_rect_.h - sz.h) * 0.5f;
            tk::Color col =
                on_image ? tk::Color::rgba(255, 255, 255, 220)
                : remove_btn_->hovered() ? ctx.theme.palette.text_primary
                                         : ctx.theme.palette.text_secondary;
            ctx.canvas.draw_text(*remove_layout_, {gx, gy}, col);
        }
    }

    // Paint SVG icons centred inside Icon-variant buttons. Monochrome Lucide
    // line icons are tinted to the button text colour so they match the
    // surrounding chrome and adapt to the active theme; hovered icons brighten
    // to text_primary like the attachment remove button above. IconCache keeps
    // them crisp across DPI and recolors on hover / theme change.
    constexpr float kIconPx = 24.0f; // fits inside 40×40 btn w/ 8 px margin
    auto btn_tint = [&](tk::Button* b)
    {
        return (b && b->hovered()) ? ctx.theme.palette.text_primary
                                   : ctx.theme.palette.text_secondary;
    };
    auto paint_icon = [&](tk::Rect rect, tk::IconCache& cache,
                          std::span<const std::uint8_t> svg, tk::Color tint)
    {
        if (!rect.empty())
            cache.draw(ctx.canvas, ctx.factory, svg, rect, kIconPx, tint);
    };

    if (emoji_btn_ && !recording_)
        paint_icon(emoji_rect_, emoji_icon_, kEmojiSvg, btn_tint(emoji_btn_));
    if (sticker_btn_ && !recording_)
        paint_icon(sticker_rect_, sticker_icon_, kStickerSvg,
                   btn_tint(sticker_btn_));
    if (mic_btn_ && mic_available_)
    {
        if (recording_)
            paint_icon(mic_btn_rect_, mic_stop_icon_, kVoiceStopSvg,
                       btn_tint(mic_btn_));
        else
            paint_icon(mic_btn_rect_, mic_icon_, kMicSvg, btn_tint(mic_btn_));
    }
}

bool ComposeBar::contains_world(tk::Point world) const
{
    if (tk::Widget::contains_world(world))
        return true;
    // Also claim the floating preview panel (image or video thumbnail) above the bar.
    if (!pending_.has_value() || preview_band_rect_.empty())
        return false;
    const auto k = pending_->kind;
    const bool is_floating =
        k == PendingAttachment::Kind::Image ||
        (k == PendingAttachment::Kind::Video && pending_->preview);
    return is_floating &&
           world.x >= preview_band_rect_.x &&
           world.x < preview_band_rect_.x + preview_band_rect_.w &&
           world.y >= preview_band_rect_.y &&
           world.y < preview_band_rect_.y + preview_band_rect_.h;
}

tk::Widget* ComposeBar::hit_test(tk::Point world)
{
    if (tk::Widget* w = tk::Widget::hit_test(world))
        return w;

    // Extend to the floating preview panel (image or video thumbnail).
    if (!pending_.has_value() || preview_band_rect_.empty())
        return nullptr;
    const auto k = pending_->kind;
    const bool is_floating =
        k == PendingAttachment::Kind::Image ||
        (k == PendingAttachment::Kind::Video && pending_->preview);
    if (is_floating &&
        world.x >= preview_band_rect_.x &&
        world.x < preview_band_rect_.x + preview_band_rect_.w &&
        world.y >= preview_band_rect_.y &&
        world.y < preview_band_rect_.y + preview_band_rect_.h)
    {
        if (remove_btn_ && !remove_btn_rect_.empty() &&
            world.x >= remove_btn_rect_.x &&
            world.x < remove_btn_rect_.x + remove_btn_rect_.w &&
            world.y >= remove_btn_rect_.y &&
            world.y < remove_btn_rect_.y + remove_btn_rect_.h)
        {
            return remove_btn_;
        }
        return this;
    }
    return nullptr;
}

bool ComposeBar::on_pointer_down(tk::Point local)
{
    const tk::Point world{bounds_.x + local.x, bounds_.y + local.y};
    press_reply_cancel_ = false;
    press_edit_cancel_ = false;
    press_voice_cancel_ = false;
    if (recording_ && !voice_cancel_rect_.empty())
    {
        if (world.x >= voice_cancel_rect_.x &&
            world.x < voice_cancel_rect_.x + voice_cancel_rect_.w &&
            world.y >= voice_cancel_rect_.y &&
            world.y < voice_cancel_rect_.y + voice_cancel_rect_.h)
        {
            press_voice_cancel_ = true;
            return true;
        }
    }
    if (has_editing() && !edit_cancel_rect_.empty())
    {
        if (world.x >= edit_cancel_rect_.x &&
            world.x < edit_cancel_rect_.x + edit_cancel_rect_.w &&
            world.y >= edit_cancel_rect_.y &&
            world.y < edit_cancel_rect_.y + edit_cancel_rect_.h)
        {
            press_edit_cancel_ = true;
            return true;
        }
    }
    if (has_reply() && !reply_cancel_rect_.empty())
    {
        if (world.x >= reply_cancel_rect_.x &&
            world.x < reply_cancel_rect_.x + reply_cancel_rect_.w &&
            world.y >= reply_cancel_rect_.y &&
            world.y < reply_cancel_rect_.y + reply_cancel_rect_.h)
        {
            press_reply_cancel_ = true;
            return true;
        }
    }
    // Absorb clicks on the floating preview (image or video thumbnail).
    if (pending_.has_value() && !preview_band_rect_.empty())
    {
        const auto k = pending_->kind;
        const bool is_floating =
            k == PendingAttachment::Kind::Image ||
            (k == PendingAttachment::Kind::Video && pending_->preview);
        if (is_floating &&
            world.x >= preview_band_rect_.x &&
            world.x < preview_band_rect_.x + preview_band_rect_.w &&
            world.y >= preview_band_rect_.y &&
            world.y < preview_band_rect_.y + preview_band_rect_.h)
        {
            return true;
        }
    }
    // Clicks that reach here did not hit any button (child widgets claim button
    // presses before on_pointer_down is reached). Focus the native text area so
    // clicking anywhere in the compose card works as the user expects.
    if (!recording_ && on_focus_input)
        on_focus_input();
    return tk::Widget::on_pointer_down(local);
}

void ComposeBar::on_pointer_up(tk::Point local, bool inside_self)
{
    const tk::Point world{bounds_.x + local.x, bounds_.y + local.y};
    if (press_voice_cancel_)
    {
        press_voice_cancel_ = false;
        if (inside_self && world.x >= voice_cancel_rect_.x &&
            world.x < voice_cancel_rect_.x + voice_cancel_rect_.w &&
            world.y >= voice_cancel_rect_.y &&
            world.y < voice_cancel_rect_.y + voice_cancel_rect_.h)
        {
            if (on_cancel_voice)
                on_cancel_voice();
        }
        return;
    }
    if (press_edit_cancel_)
    {
        press_edit_cancel_ = false;
        if (inside_self && world.x >= edit_cancel_rect_.x &&
            world.x < edit_cancel_rect_.x + edit_cancel_rect_.w &&
            world.y >= edit_cancel_rect_.y &&
            world.y < edit_cancel_rect_.y + edit_cancel_rect_.h)
        {
            clear_editing();
            if (on_edit_cancelled)
            {
                on_edit_cancelled();
            }
            return;
        }
    }
    if (press_reply_cancel_)
    {
        press_reply_cancel_ = false;
        if (inside_self && world.x >= reply_cancel_rect_.x &&
            world.x < reply_cancel_rect_.x + reply_cancel_rect_.w &&
            world.y >= reply_cancel_rect_.y &&
            world.y < reply_cancel_rect_.y + reply_cancel_rect_.h)
        {
            clear_reply();
            return;
        }
    }
    tk::Widget::on_pointer_up(local, inside_self);
}

tk::Widget* ComposeBar::dispatch_pointer_move(tk::Point world, bool* dirty)
{
    if (!visible_ || !contains_world(world))
        return nullptr;
    // Buttons are the deepest hit children; the default dispatch would return
    // one of them and skip our on_pointer_move(), so tooltips would never fire.
    // Mirror the RoomHeader fix: delegate first, then always call our own
    // on_pointer_move() so we track hover across all button rects.
    Widget* hit = Widget::dispatch_pointer_move(world, dirty);
    if (hit && hit != this)
    {
        tk::Point local{world.x - bounds_.x, world.y - bounds_.y};
        if (on_pointer_move(local) && dirty)
            *dirty = true;
    }
    return this;
}

bool ComposeBar::on_pointer_move(tk::Point local)
{
    const tk::Point world{bounds_.x + local.x, bounds_.y + local.y};

    auto hit = [&](tk::Rect r) -> bool
    {
        return !r.empty() && world.x >= r.x && world.x < r.x + r.w &&
               world.y >= r.y && world.y < r.y + r.h;
    };

    TooltipBtn next = TooltipBtn::None;
    if (!recording_ && emoji_btn_ && hit(emoji_rect_))
        next = TooltipBtn::Emoji;
    else if (!recording_ && sticker_btn_ && hit(sticker_rect_))
        next = TooltipBtn::Sticker;
    else if (mic_btn_ && mic_available_ && hit(mic_btn_rect_))
        next = TooltipBtn::Mic;

    if (next != tooltip_hover_)
    {
        if (tooltip_hover_ != TooltipBtn::None && host_)
            host_->hide_tooltip(this);
        tooltip_hover_ = next;
        if (next != TooltipBtn::None && host_)
        {
            const std::string text =
                next == TooltipBtn::Emoji   ? tk::tr("Emoji")
                : next == TooltipBtn::Sticker ? tk::tr("Stickers")
                : recording_                  ? tk::tr("Stop recording")
                                              : tk::tr("Voice message");
            tk::Rect anchor = next == TooltipBtn::Emoji   ? emoji_rect_
                              : next == TooltipBtn::Sticker ? sticker_rect_
                                                             : mic_btn_rect_;
            host_->show_tooltip(this, text, anchor);
        }
    }
    return true;
}

void ComposeBar::on_pointer_leave()
{
    if (tooltip_hover_ != TooltipBtn::None)
    {
        if (host_) host_->hide_tooltip(this);
        tooltip_hover_ = TooltipBtn::None;
    }
}

} // namespace tesseract::views
