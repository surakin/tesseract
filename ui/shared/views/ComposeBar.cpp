#include "ComposeBar.h"

#include "format.h"
#include "tk/theme.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <utility>

namespace tesseract::views {

namespace {

constexpr float kPadX        = 8.0f;
constexpr float kPadY        = 8.0f;
constexpr float kButtonSide  = 40.0f;
constexpr float kSendWidth   = 64.0f;
constexpr float kGap         = 6.0f;
constexpr float kRemoveBtnSide = 24.0f;
constexpr float kRemoveBtnInset = 4.0f;

// Compose-bar background is a tint of the surface — sits below the
// message list and above the bottom edge. Border is a 1px hairline on
// the top edge separating from the timeline.
inline tk::Color bar_bg(const tk::Theme& t)  { return t.palette.chrome_bg; }
inline tk::Color card_bg(const tk::Theme& t) { return t.palette.compose_card_bg; }

} // namespace

std::string ComposeBar::make_filename(const std::string& mime) {
    using namespace std::chrono;
    auto now  = system_clock::now();
    std::time_t tt = system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &tt);
#else
    localtime_r(&tt, &tm);
#endif
    char buf[64];
    std::snprintf(buf, sizeof(buf),
                  "clipboard-%04d%02d%02d-%02d%02d%02d",
                  tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec);
    std::string ext = "bin";
    if      (mime == "image/png")  ext = "png";
    else if (mime == "image/jpeg") ext = "jpg";
    else if (mime == "image/webp") ext = "webp";
    else if (mime == "image/bmp")  ext = "bmp";
    else if (mime == "image/gif")  ext = "gif";
    return std::string(buf) + "." + ext;
}

ComposeBar::ComposeBar() {
    auto emoji = std::make_unique<tk::Button>(
        // U+1F600 GRINNING FACE. We keep the glyph as the Button's label
        // (even though Icon variant doesn't paint it) so test scaffolding
        // that scans buttons by label can still find it; the glyph itself
        // is painted in ComposeBar::paint() at Title size — see below.
        std::string("\xF0\x9F\x98\x80"),
        std::function<void()>{},
        tk::Button::Variant::Icon);
    emoji->set_on_click([this] { if (on_emoji) on_emoji(); });
    emoji->set_min_size({ kButtonSide, kButtonSide });
    emoji_btn_ = add_child(std::move(emoji));

    // Sticker button. Glyph: U+1F5BC FE0F FRAMED PICTURE — distinct from
    // the emoji face so the two icons are visually unambiguous. Same
    // Icon variant + Title-size glyph painted on top as the emoji button.
    auto sticker = std::make_unique<tk::Button>(
        std::string("\xF0\x9F\x96\xBC\xEF\xB8\x8F"),
        std::function<void()>{},
        tk::Button::Variant::Icon);
    sticker->set_on_click([this] { if (on_sticker) on_sticker(); });
    sticker->set_min_size({ kButtonSide, kButtonSide });
    sticker_btn_ = add_child(std::move(sticker));

    auto send = std::make_unique<tk::Button>(
        "Send",
        std::function<void()>{},
        tk::Button::Variant::Primary);
    send->set_on_click([this] { trigger_send(); });
    send->set_min_size({ kSendWidth, kButtonSide });
    send_btn_ = add_child(std::move(send));

    auto remove = std::make_unique<tk::Button>(
        // ✕ small (U+2715)
        std::string("\xE2\x9C\x95"),
        std::function<void()>{},
        tk::Button::Variant::Icon);
    remove->set_on_click([this] { clear_pending_image(); });
    remove->set_min_size({ kRemoveBtnSide, kRemoveBtnSide });
    remove->set_visible(false);
    remove_btn_ = add_child(std::move(remove));

    refresh_send_enabled();
}

void ComposeBar::trigger_send() {
    if (pending_.has_value()) {
        std::string reply_id = reply_event_id_;
        if (pending_->kind == PendingAttachment::Kind::Image) {
            if (on_send_image) {
                on_send_image(std::move(pending_->bytes),
                              std::move(pending_->mime),
                              std::move(pending_->filename),
                              current_text_,
                              pending_->width,
                              pending_->height,
                              std::move(reply_id));
            }
        } else {
            if (on_send_file) {
                on_send_file(std::move(pending_->bytes),
                             std::move(pending_->mime),
                             std::move(pending_->filename),
                             current_text_,
                             std::move(reply_id));
            }
        }
        pending_.reset();
        file_name_layout_.reset();
        file_size_layout_.reset();
        file_layout_key_.clear();
        clear_reply();
        recompute_height();
        if (remove_btn_) remove_btn_->set_visible(false);
        refresh_send_enabled();
        if (on_size_changed) on_size_changed();
    } else if (has_editing()) {
        std::string ev   = edit_event_id_;
        std::string text = current_text_;
        clear_editing();
        if (on_send_edit) on_send_edit(ev, text);
    } else if (has_reply()) {
        std::string id   = reply_event_id_;
        std::string text = current_text_;
        clear_reply();
        if (on_send_reply) on_send_reply(id, text);
    } else {
        if (on_send) on_send(current_text_);
    }
}

void ComposeBar::set_text_area_natural_height(float h) {
    text_area_natural_ = h;
    recompute_height();
}

void ComposeBar::recompute_height() {
    float text_h   = std::clamp(text_area_natural_ + kPadY * 2,
                                 kMinHeight, kMaxHeight);
    float top_h    = 0.0f;
    if (has_editing())
        top_h = kEditBandH + kEditBandGap;
    else if (has_reply())
        top_h = kReplyBandH + kReplyBandGap;
    float band_h = 0.0f;
    if (pending_.has_value()) {
        band_h = pending_->kind == PendingAttachment::Kind::Image
            ? kPreviewBandH
            : kFileBandH;
        band_h += kPreviewBandGap;
    }
    // arrange() insets the first band kPadY from the bar's top edge; add
    // that offset once when any band is present so natural_height_ matches
    // the full space that arrange() actually consumes.
    float total_bands = top_h + band_h;
    natural_height_ = text_h + total_bands + (total_bands > 0.0f ? kPadY : 0.0f);
}

void ComposeBar::set_reply_to(std::string event_id,
                               std::string sender_name,
                               std::string body_preview) {
    reply_event_id_     = std::move(event_id);
    reply_sender_name_  = std::move(sender_name);
    reply_body_preview_ = std::move(body_preview);
    recompute_height();
    if (on_size_changed) on_size_changed();
}

void ComposeBar::clear_reply() {
    if (reply_event_id_.empty()) return;
    reply_event_id_.clear();
    reply_sender_name_.clear();
    reply_body_preview_.clear();
    reply_band_rect_   = {};
    reply_cancel_rect_ = {};
    recompute_height();
    if (on_size_changed) on_size_changed();
}

void ComposeBar::set_editing(std::string event_id) {
    // Edit mode and reply mode are mutually exclusive — silently drop reply.
    reply_event_id_.clear();
    reply_sender_name_.clear();
    reply_body_preview_.clear();
    reply_band_rect_   = {};
    reply_cancel_rect_ = {};

    edit_event_id_  = std::move(event_id);
    recompute_height();
    if (on_size_changed) on_size_changed();
}

void ComposeBar::clear_editing() {
    if (edit_event_id_.empty()) return;
    edit_event_id_.clear();
    edit_band_rect_   = {};
    edit_cancel_rect_ = {};
    recompute_height();
    if (on_size_changed) on_size_changed();
}

void ComposeBar::set_current_text(std::string text) {
    current_text_ = std::move(text);
    refresh_send_enabled();
}

void ComposeBar::set_enabled(bool e) {
    if (enabled_ == e) return;
    enabled_ = e;
    if (emoji_btn_)   emoji_btn_->set_enabled(e);
    if (sticker_btn_) sticker_btn_->set_enabled(e);
    if (remove_btn_)  remove_btn_->set_enabled(e);
    refresh_send_enabled();
}

void ComposeBar::set_pending_image(std::vector<std::uint8_t> bytes,
                                    std::string mime,
                                    std::string filename) {
    PendingAttachment pa;
    pa.kind     = PendingAttachment::Kind::Image;
    pa.bytes    = std::move(bytes);
    pa.mime     = std::move(mime);
    pa.filename = filename.empty() ? make_filename(pa.mime)
                                   : std::move(filename);
    pending_    = std::move(pa);
    file_name_layout_.reset();
    file_size_layout_.reset();
    file_layout_key_.clear();
    // Dimensions + preview are filled in lazily on the next arrange()
    // pass (we don't have a CanvasFactory here).
    recompute_height();
    if (remove_btn_) remove_btn_->set_visible(true);
    refresh_send_enabled();
    if (on_size_changed) on_size_changed();
}

void ComposeBar::set_pending_file(std::vector<std::uint8_t> bytes,
                                   std::string mime,
                                   std::string filename) {
    PendingAttachment pa;
    pa.kind     = PendingAttachment::Kind::File;
    pa.bytes    = std::move(bytes);
    pa.mime     = std::move(mime);
    pa.filename = std::move(filename);
    pending_    = std::move(pa);
    file_name_layout_.reset();
    file_size_layout_.reset();
    file_layout_key_.clear();
    recompute_height();
    if (remove_btn_) remove_btn_->set_visible(true);
    refresh_send_enabled();
    if (on_size_changed) on_size_changed();
}

void ComposeBar::clear_pending() {
    if (!pending_.has_value()) return;
    pending_.reset();
    file_name_layout_.reset();
    file_size_layout_.reset();
    file_layout_key_.clear();
    recompute_height();
    if (remove_btn_) remove_btn_->set_visible(false);
    refresh_send_enabled();
    if (on_size_changed) on_size_changed();
}

void ComposeBar::refresh_send_enabled() {
    if (!send_btn_) return;
    bool any_text = false;
    for (char c : current_text_) {
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            any_text = true; break;
        }
    }
    send_btn_->set_enabled(enabled_ && (any_text || pending_.has_value()));
}

tk::Size ComposeBar::measure(tk::LayoutCtx&, tk::Size constraints) {
    return { constraints.w, natural_height_ };
}

void ComposeBar::arrange(tk::LayoutCtx& ctx, tk::Rect bounds) {
    bounds_ = bounds;

    // ── Decode the pending image lazily (now that we have a factory) ──
    if (pending_.has_value()
        && pending_->kind == PendingAttachment::Kind::Image
        && !pending_->preview) {
        auto img = ctx.factory.decode_image(
            std::span<const std::uint8_t>(pending_->bytes.data(),
                                            pending_->bytes.size()));
        if (img) {
            pending_->width  = static_cast<std::uint32_t>(img->width());
            pending_->height = static_cast<std::uint32_t>(img->height());
            pending_->preview = std::move(img);
        }
    }

    // ── Build (or refresh) cached text layouts for the file chip ──────
    if (pending_.has_value()
        && pending_->kind == PendingAttachment::Kind::File) {
        std::string key = pending_->filename + "|"
                        + std::to_string(pending_->bytes.size());
        if (file_layout_key_ != key) {
            tk::TextStyle name_style{};
            name_style.role = tk::FontRole::Body;
            file_name_layout_ = ctx.factory.build_text(
                pending_->filename, name_style);

            // Size line: human-readable bytes.
            std::string size_str = format_size(
                static_cast<std::uint64_t>(pending_->bytes.size()));
            tk::TextStyle size_style{};
            size_style.role = tk::FontRole::Small;
            file_size_layout_ = ctx.factory.build_text(size_str, size_style);
            file_layout_key_ = std::move(key);
        }
    }

    // ── Top banner (edit mode XOR reply mode — topmost when active) ──
    float text_top = bounds.y;
    if (has_editing()) {
        edit_band_rect_ = {
            bounds.x + kPadX,
            bounds.y + kPadY,
            std::max(0.0f, bounds.w - kPadX * 2),
            kEditBandH
        };
        constexpr float kCancelSide   = 20.0f;
        constexpr float kCancelInsetX =  8.0f;
        edit_cancel_rect_ = {
            edit_band_rect_.x + edit_band_rect_.w - kCancelSide - kCancelInsetX,
            edit_band_rect_.y + (kEditBandH - kCancelSide) * 0.5f,
            kCancelSide, kCancelSide
        };
        text_top = edit_band_rect_.y + edit_band_rect_.h + kEditBandGap;
        reply_band_rect_   = {};
        reply_cancel_rect_ = {};
    } else if (has_reply()) {
        reply_band_rect_ = {
            bounds.x + kPadX,
            bounds.y + kPadY,
            std::max(0.0f, bounds.w - kPadX * 2),
            kReplyBandH
        };
        constexpr float kCancelSide   = 20.0f;
        constexpr float kCancelInsetX =  8.0f;
        reply_cancel_rect_ = {
            reply_band_rect_.x + reply_band_rect_.w - kCancelSide - kCancelInsetX,
            reply_band_rect_.y + (kReplyBandH - kCancelSide) * 0.5f,
            kCancelSide, kCancelSide
        };
        text_top = reply_band_rect_.y + reply_band_rect_.h + kReplyBandGap;
        edit_band_rect_   = {};
        edit_cancel_rect_ = {};
    } else {
        reply_band_rect_   = {};
        reply_cancel_rect_ = {};
        edit_band_rect_    = {};
        edit_cancel_rect_  = {};
    }

    // ── Attachment band (below reply band, or at top when no reply) ───
    if (pending_.has_value()) {
        float band_h = pending_->kind == PendingAttachment::Kind::Image
            ? kPreviewBandH : kFileBandH;
        // When a reply band already consumed kPadY, start immediately after it.
        // Otherwise, inset the first band by kPadY from the widget top.
        float band_y = has_reply() ? text_top : bounds.y + kPadY;
        preview_band_rect_ = {
            bounds.x + kPadX,
            band_y,
            std::max(0.0f, bounds.w - kPadX * 2),
            band_h
        };
        if (pending_->kind == PendingAttachment::Kind::Image) {
            // Thumbnail letterboxed inside the band, preserving aspect.
            float img_w = static_cast<float>(pending_->width);
            float img_h = static_cast<float>(pending_->height);
            if (img_w <= 0 || img_h <= 0) {
                preview_image_rect_ = preview_band_rect_;
            } else {
                float s = std::min(preview_band_rect_.w / img_w,
                                    preview_band_rect_.h / img_h);
                float dw = img_w * s;
                float dh = img_h * s;
                preview_image_rect_ = {
                    preview_band_rect_.x,
                    preview_band_rect_.y + (preview_band_rect_.h - dh) * 0.5f,
                    dw, dh
                };
            }
            // Remove button overlaid top-right of the thumbnail.
            remove_btn_rect_ = {
                preview_image_rect_.x + preview_image_rect_.w
                    - kRemoveBtnSide - kRemoveBtnInset,
                preview_image_rect_.y + kRemoveBtnInset,
                kRemoveBtnSide, kRemoveBtnSide
            };
        } else {
            // File chip: remove button anchored to right edge of band,
            // vertically centred. Image rect unused.
            preview_image_rect_ = {};
            remove_btn_rect_ = {
                preview_band_rect_.x + preview_band_rect_.w
                    - kRemoveBtnSide - kRemoveBtnInset,
                preview_band_rect_.y
                    + (preview_band_rect_.h - kRemoveBtnSide) * 0.5f,
                kRemoveBtnSide, kRemoveBtnSide
            };
        }
        text_top = preview_band_rect_.y + preview_band_rect_.h
                    + kPreviewBandGap;
    } else {
        preview_band_rect_ = {};
        preview_image_rect_ = {};
        remove_btn_rect_ = {};
    }

    float text_strip_h = bounds.y + bounds.h - text_top;

    // Send button sits to the right, outside the input card.
    send_rect_ = {
        bounds.x + bounds.w - kPadX - kSendWidth,
        text_top + (text_strip_h - kButtonSide) * 0.5f,
        kSendWidth,
        kButtonSide
    };

    // The compose card spans from the left edge to just before the send button.
    // Emoji and sticker buttons live inside the card on the right; the text
    // area fills the left portion of the card.
    const float card_left  = bounds.x + kPadX;
    const float card_right = send_rect_.x - kGap;
    compose_card_rect_ = {
        card_left,
        text_top + kPadY,
        std::max(0.0f, card_right - card_left),
        std::max(0.0f, text_strip_h - kPadY * 2)
    };

    const float btn_y = text_top + (text_strip_h - kButtonSide) * 0.5f;
    sticker_rect_ = {
        card_right - kButtonSide,
        btn_y,
        kButtonSide,
        kButtonSide
    };
    emoji_rect_ = {
        sticker_rect_.x - kGap - kButtonSide,
        btn_y,
        kButtonSide,
        kButtonSide
    };

    // Text area occupies the left portion of the card, leaving room for
    // the emoji/sticker buttons on the right with a small gap.
    text_area_rect_ = {
        card_left + kPadX,
        text_top + kPadY,
        std::max(0.0f, emoji_rect_.x - kGap - (card_left + kPadX)),
        std::max(0.0f, text_strip_h - kPadY * 2)
    };

    if (emoji_btn_)   emoji_btn_->arrange(ctx, emoji_rect_);
    if (sticker_btn_) sticker_btn_->arrange(ctx, sticker_rect_);
    if (send_btn_)    send_btn_->arrange(ctx, send_rect_);
    if (remove_btn_ && pending_.has_value())
        remove_btn_->arrange(ctx, remove_btn_rect_);
}

void ComposeBar::paint(tk::PaintCtx& ctx) {
    ctx.canvas.fill_rect(bounds_, bar_bg(ctx.theme));
    // 1 px top hairline so the bar reads as a separate strip from the
    // message list above it.
    tk::Rect hairline{
        bounds_.x, bounds_.y, bounds_.w, 1.0f
    };
    ctx.canvas.fill_rect(hairline, ctx.theme.palette.border);

    if (pending_.has_value()) {
        // Subtle card behind the preview band.
        ctx.canvas.fill_rounded_rect(preview_band_rect_, 8.0f,
                                       card_bg(ctx.theme));
        if (pending_->kind == PendingAttachment::Kind::Image) {
            if (pending_->preview) {
                ctx.canvas.draw_image(*pending_->preview, preview_image_rect_);
            }
        } else {
            // File chip: paperclip glyph + filename (top line) + size
            // (caption line). Layout was prepared in arrange().
            constexpr float kChipPadX = 12.0f;
            float text_x = preview_band_rect_.x + kChipPadX;
            float text_right = remove_btn_rect_.empty()
                ? preview_band_rect_.x + preview_band_rect_.w - kChipPadX
                : remove_btn_rect_.x - kPadX;
            float avail_w = std::max(0.0f, text_right - text_x);

            if (file_name_layout_ && file_size_layout_) {
                tk::Size name_sz = file_name_layout_->measure();
                tk::Size size_sz = file_size_layout_->measure();
                float total_h = name_sz.h + size_sz.h;
                float ty = preview_band_rect_.y
                    + (preview_band_rect_.h - total_h) * 0.5f;
                ctx.canvas.draw_text(*file_name_layout_,
                    { text_x, ty }, ctx.theme.palette.text_primary);
                ctx.canvas.draw_text(*file_size_layout_,
                    { text_x, ty + name_sz.h },
                    ctx.theme.palette.text_secondary);
                (void)avail_w;  // truncation handled by layout backend
            }
        }
    }

    // ── Edit mode banner ─────────────────────────────────────────────
    if (has_editing() && !edit_band_rect_.empty()) {
        constexpr float kAccentW   =  3.0f;
        constexpr float kEditPadX  =  8.0f;
        constexpr float kEditPadY  =  5.0f;

        ctx.canvas.fill_rounded_rect(edit_band_rect_, 6.0f,
                                      card_bg(ctx.theme));
        ctx.canvas.fill_rect(
            { edit_band_rect_.x, edit_band_rect_.y,
              kAccentW, edit_band_rect_.h },
            ctx.theme.palette.accent);

        float text_x = edit_band_rect_.x + kAccentW + kEditPadX;

        tk::TextStyle label_style{};
        label_style.role = tk::FontRole::Small;
        auto label_layout = ctx.factory.build_text("Editing message", label_style);
        if (label_layout) {
            ctx.canvas.draw_text(*label_layout,
                { text_x, edit_band_rect_.y + kEditPadY },
                ctx.theme.palette.text_secondary);
        }

        if (!edit_cancel_rect_.empty()) {
            tk::TextStyle x_style{};
            x_style.role = tk::FontRole::Body;
            auto x_layout = ctx.factory.build_text(
                std::string("\xC3\x97"), x_style);
            if (x_layout) {
                tk::Size sz = x_layout->measure();
                ctx.canvas.draw_text(*x_layout,
                    { edit_cancel_rect_.x + (edit_cancel_rect_.w - sz.w) * 0.5f,
                      edit_cancel_rect_.y + (edit_cancel_rect_.h - sz.h) * 0.5f },
                    press_edit_cancel_
                        ? ctx.theme.palette.text_primary
                        : ctx.theme.palette.text_muted);
            }
        }
    }

    // ── Reply preview banner ──────────────────────────────────────────
    if (has_reply() && !reply_band_rect_.empty()) {
        constexpr float kAccentW      =  3.0f;
        constexpr float kReplyPadX    =  8.0f;

        ctx.canvas.fill_rounded_rect(reply_band_rect_, 6.0f,
                                      card_bg(ctx.theme));
        ctx.canvas.fill_rect(
            { reply_band_rect_.x, reply_band_rect_.y,
              kAccentW, reply_band_rect_.h },
            ctx.theme.palette.accent);

        float text_x = reply_band_rect_.x + kAccentW + kReplyPadX;

        // Build both lines first so we can measure heights and centre the
        // pair vertically within the band.
        tk::TextStyle label_style{};
        label_style.role = tk::FontRole::Small;
        auto label_layout = ctx.factory.build_text(
            "Replying to " + reply_sender_name_, label_style);

        tk::TextStyle body_style{};
        body_style.role = tk::FontRole::Small;
        auto body_layout = ctx.factory.build_text(
            reply_body_preview_, body_style);

        constexpr float kLineGap = 2.0f;
        float label_h = label_layout ? label_layout->measure().h : 0.0f;
        float body_h  = body_layout  ? body_layout->measure().h  : 0.0f;
        float total_h = label_h + (body_h > 0.0f ? kLineGap + body_h : 0.0f);
        float text_y  = reply_band_rect_.y
                        + (kReplyBandH - total_h) * 0.5f;

        if (label_layout)
            ctx.canvas.draw_text(*label_layout,
                { text_x, text_y },
                ctx.theme.palette.text_secondary);
        if (body_layout)
            ctx.canvas.draw_text(*body_layout,
                { text_x, text_y + label_h + kLineGap },
                ctx.theme.palette.text_muted);

        // "×" cancel glyph centred in reply_cancel_rect_
        if (!reply_cancel_rect_.empty()) {
            tk::TextStyle x_style{};
            x_style.role = tk::FontRole::Body;
            // U+00D7 MULTIPLICATION SIGN: C3 97
            auto x_layout = ctx.factory.build_text(
                std::string("\xC3\x97"), x_style);
            if (x_layout) {
                tk::Size sz = x_layout->measure();
                ctx.canvas.draw_text(*x_layout,
                    { reply_cancel_rect_.x + (reply_cancel_rect_.w - sz.w) * 0.5f,
                      reply_cancel_rect_.y + (reply_cancel_rect_.h - sz.h) * 0.5f },
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
    if (!compose_card_rect_.empty()) {
        ctx.canvas.fill_rounded_rect(compose_card_rect_, 6.0f,
                                       card_bg(ctx.theme));
        ctx.canvas.stroke_rounded_rect(compose_card_rect_, 6.0f,
                                         ctx.theme.palette.border, 1.0f);
    }

    if (emoji_btn_)   emoji_btn_->paint(ctx);
    if (sticker_btn_) sticker_btn_->paint(ctx);
    if (send_btn_)    send_btn_->paint(ctx);
    if (remove_btn_ && pending_.has_value()) remove_btn_->paint(ctx);

    // Paint the icon glyphs over the Icon-variant buttons at the same
    // size and centring as reaction chips so the icon surfaces match.
    // Colour-emoji glyphs fill the ascent region and leave the descender
    // empty, so box-centring leaves them visually high.
    constexpr float kAscentRatio = 0.78f;
    auto paint_glyph_over = [&](tk::Rect rect,
                                 std::unique_ptr<tk::TextLayout>& cache,
                                 const char* glyph) {
        if (rect.empty()) return;
        if (!cache) {
            tk::TextStyle st{};
            st.role = tk::FontRole::Title;
            cache = ctx.factory.build_text(std::string(glyph), st);
        }
        if (!cache) return;
        tk::Size sz = cache->measure();
        float x = rect.x + (rect.w - sz.w) * 0.5f;
        float y = rect.y + (rect.h - sz.h * kAscentRatio) * 0.5f;
        ctx.canvas.draw_text(*cache, { x, y }, ctx.theme.palette.text_primary);
    };

    if (emoji_btn_)
        paint_glyph_over(emoji_rect_, emoji_layout_, "\xF0\x9F\x98\x80");
    if (sticker_btn_)
        paint_glyph_over(sticker_rect_, sticker_layout_,
                         "\xF0\x9F\x96\xBC\xEF\xB8\x8F");
}

bool ComposeBar::on_pointer_down(tk::Point local) {
    press_reply_cancel_ = false;
    press_edit_cancel_  = false;
    if (has_editing() && !edit_cancel_rect_.empty()) {
        if (local.x >= edit_cancel_rect_.x
            && local.x <  edit_cancel_rect_.x + edit_cancel_rect_.w
            && local.y >= edit_cancel_rect_.y
            && local.y <  edit_cancel_rect_.y + edit_cancel_rect_.h) {
            press_edit_cancel_ = true;
            return true;
        }
    }
    if (has_reply() && !reply_cancel_rect_.empty()) {
        if (local.x >= reply_cancel_rect_.x
            && local.x <  reply_cancel_rect_.x + reply_cancel_rect_.w
            && local.y >= reply_cancel_rect_.y
            && local.y <  reply_cancel_rect_.y + reply_cancel_rect_.h) {
            press_reply_cancel_ = true;
            return true;
        }
    }
    return tk::Widget::on_pointer_down(local);
}

void ComposeBar::on_pointer_up(tk::Point local, bool inside_self) {
    if (press_edit_cancel_) {
        press_edit_cancel_ = false;
        if (inside_self
            && local.x >= edit_cancel_rect_.x
            && local.x <  edit_cancel_rect_.x + edit_cancel_rect_.w
            && local.y >= edit_cancel_rect_.y
            && local.y <  edit_cancel_rect_.y + edit_cancel_rect_.h) {
            clear_editing();
            if (on_edit_cancelled) on_edit_cancelled();
            return;
        }
    }
    if (press_reply_cancel_) {
        press_reply_cancel_ = false;
        if (inside_self
            && local.x >= reply_cancel_rect_.x
            && local.x <  reply_cancel_rect_.x + reply_cancel_rect_.w
            && local.y >= reply_cancel_rect_.y
            && local.y <  reply_cancel_rect_.y + reply_cancel_rect_.h) {
            clear_reply();
            return;
        }
    }
    tk::Widget::on_pointer_up(local, inside_self);
}

} // namespace tesseract::views
