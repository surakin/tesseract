#include "MessageListView.h"

#include "tk/theme.h"
#include <tesseract/visual.h>

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <string>

namespace tesseract::views {

namespace {

constexpr float kPadX        = tesseract::visual::kSpaceMD;          // 12
constexpr float kPadY        = tesseract::visual::kMsgRowVerticalPad; // 6
constexpr float kAvatarSize  = tesseract::visual::kMsgAvatarSize;    // 32
constexpr float kAvatarGap   = tesseract::visual::kMsgAvatarGap;     // 8
constexpr float kSenderH     = tesseract::visual::kMsgSenderNameHeight; // 16
constexpr float kTimestampH  = tesseract::visual::kMsgTimestampHeight;  // 14
constexpr float kChipH       = tesseract::visual::kReactionChipHeight;  // 22
constexpr float kChipGap     = tesseract::visual::kReactionChipGap;     // 4
constexpr float kChipPadX    = 8.0f;
constexpr float kChipRadius  = kChipH * 0.5f;
constexpr float kImageMaxW   = tesseract::visual::kMaxInlineImageWidth;  // 320
constexpr float kImageMaxH   = tesseract::visual::kMaxInlineImageHeight; // 200
constexpr float kStickerSize = tesseract::visual::kStickerSize;          // 256
constexpr float kFileCardH   = 56.0f;
constexpr float kFileCardW   = 280.0f;

std::string format_hhmm(std::uint64_t timestamp_ms) {
    if (timestamp_ms == 0) return {};
    std::time_t t = static_cast<std::time_t>(timestamp_ms / 1000);
    std::tm     local{};
#if defined(_WIN32)
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%02d:%02d",
                   local.tm_hour, local.tm_min);
    return std::string(buf);
}

std::string format_size(std::uint64_t bytes) {
    if (bytes < 1024)              return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024)       return std::to_string(bytes / 1024) + " KB";
    if (bytes < 1024ull * 1024 * 1024)
                                    return std::to_string(bytes / (1024 * 1024)) + " MB";
    return std::to_string(bytes / (1024ull * 1024 * 1024)) + " GB";
}

// Inline-media draw size that fits inside (max_w, max_h) preserving
// aspect ratio. Returns natural (clamped) media size when the sender
// didn't supply intrinsic dimensions.
tk::Size fit_media(float natural_w, float natural_h, float max_w, float max_h) {
    if (natural_w <= 0 || natural_h <= 0) return { max_w, max_h * 0.5f };
    float sx = max_w / natural_w;
    float sy = max_h / natural_h;
    float s  = std::min({ sx, sy, 1.0f });
    return { natural_w * s, natural_h * s };
}

float body_text_max_width(float row_width) {
    return std::max(0.0f,
                     row_width - kPadX - kAvatarSize - kAvatarGap - kPadX);
}

} // namespace

class MessageListView::Adapter : public tk::ListAdapter {
public:
    explicit Adapter(MessageListView& owner) : owner_(owner) {}

    std::size_t count() const override { return owner_.messages_.size(); }

    float measure_row_height(std::size_t index, tk::LayoutCtx& ctx,
                              float width) override {
        if (index >= owner_.messages_.size()) return 0;
        const auto& m = owner_.messages_[index];
        float body_w = body_text_max_width(width);

        float content_h = kSenderH;     // sender row
        content_h += measure_body_block_height(m, ctx, body_w);
        if (!m.reactions.empty() || m.timestamp_ms != 0) {
            content_h += kChipH;        // reactions + timestamp share row
        }
        // The avatar column reserves at least kAvatarSize, so the row
        // is always tall enough for a 32 px disc.
        float row_h = std::max(content_h, kAvatarSize) + kPadY * 2;
        return row_h;
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                    bool /*selected*/, bool hovered) override {
        if (index >= owner_.messages_.size()) return;
        const auto& m = owner_.messages_[index];

        if (hovered) {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.subtle_hover);
        }

        // Avatar (top-left, vertically pinned to the sender row).
        float avatar_cx = bounds.x + kPadX + kAvatarSize * 0.5f;
        float avatar_cy = bounds.y + kPadY + kAvatarSize * 0.5f;
        const tk::Image* avatar = nullptr;
        if (owner_.avatar_provider_ && !m.sender_avatar_url.empty()) {
            avatar = owner_.avatar_provider_(m.sender_avatar_url);
        }
        if (avatar) {
            ctx.canvas.draw_circle_image(*avatar,
                                          { avatar_cx, avatar_cy },
                                          kAvatarSize);
        } else {
            ctx.canvas.draw_initials_circle(
                m.sender_name.empty() ? m.sender : m.sender_name,
                { avatar_cx, avatar_cy },
                kAvatarSize,
                ctx.theme.palette.avatar_initials_bg,
                ctx.theme.palette.avatar_initials_text);
        }

        // Right-of-avatar column.
        float col_x  = bounds.x + kPadX + kAvatarSize + kAvatarGap;
        float col_w  = std::max(0.0f, bounds.x + bounds.w - col_x - kPadX);
        float cursor = bounds.y + kPadY;

        // Sender name.
        {
            tk::TextStyle s{};
            s.role      = tk::FontRole::SenderName;
            s.trim      = tk::TextTrim::Ellipsis;
            s.max_width = col_w;
            auto layout = ctx.factory.build_text(
                m.sender_name.empty() ? m.sender : m.sender_name, s);
            if (layout) {
                ctx.canvas.draw_text(*layout, { col_x, cursor },
                                      ctx.theme.palette.text_secondary);
            }
        }
        cursor += kSenderH;

        // Body block: text / image / sticker / file card.
        cursor = paint_body_block(m, ctx, col_x, cursor, col_w);

        // Reactions row + timestamp.
        if (!m.reactions.empty() || m.timestamp_ms != 0) {
            float chip_y = cursor;
            float chip_x = col_x;
            for (const auto& r : m.reactions) {
                std::string chip_text = r.key + " " + std::to_string(r.count);
                tk::TextStyle st{};
                st.role = tk::FontRole::Small;
                auto layout = ctx.factory.build_text(chip_text, st);
                if (!layout) continue;
                float w = std::max(layout->measure().w + kChipPadX * 2, 28.0f);
                tk::Rect pill{ chip_x, chip_y, w, kChipH };
                tk::Color bg     = r.reacted_by_me ? ctx.theme.palette.chip_bg_me
                                                   : ctx.theme.palette.chip_bg;
                tk::Color border = r.reacted_by_me ? ctx.theme.palette.chip_border_me
                                                   : ctx.theme.palette.chip_border;
                tk::Color text   = r.reacted_by_me ? ctx.theme.palette.chip_text_me
                                                   : ctx.theme.palette.chip_text;
                ctx.canvas.fill_rounded_rect(pill, kChipRadius, bg);
                ctx.canvas.stroke_rounded_rect(pill, kChipRadius, border, 1.0f);
                ctx.canvas.draw_text(
                    *layout,
                    { pill.x + kChipPadX,
                      pill.y + (pill.h - layout->measure().h) * 0.5f },
                    text);
                chip_x += w + kChipGap;
            }

            std::string ts = format_hhmm(m.timestamp_ms);
            if (!ts.empty()) {
                tk::TextStyle st{};
                st.role   = tk::FontRole::Timestamp;
                st.halign = tk::TextHAlign::Trailing;
                auto layout = ctx.factory.build_text(ts, st);
                if (layout) {
                    float tx = bounds.x + bounds.w - kPadX
                                - layout->measure().w;
                    float ty = chip_y + (kChipH - layout->measure().h) * 0.5f;
                    ctx.canvas.draw_text(*layout, { tx, ty },
                                          ctx.theme.palette.text_muted);
                }
            }
        }
    }

private:
    float measure_body_block_height(const MessageRowData& m,
                                     tk::LayoutCtx& ctx,
                                     float col_w) const {
        switch (m.kind) {
            case MessageRowData::Kind::Text:
            case MessageRowData::Kind::Redacted:
            case MessageRowData::Kind::Unhandled:
                return measure_text_height(m.body.empty()
                    ? std::string("(empty message)")
                    : m.body,
                    ctx, col_w);

            case MessageRowData::Kind::Image: {
                tk::Size sz = fit_media(m.media_w, m.media_h,
                                         std::min(col_w, kImageMaxW),
                                         kImageMaxH);
                float h = sz.h;
                if (m.has_filename_caption && !m.body.empty()) {
                    h += 4.0f + measure_text_height(m.body, ctx, col_w);
                }
                return h;
            }
            case MessageRowData::Kind::Sticker: {
                float side = std::min({ kStickerSize, col_w, kStickerSize });
                return side;
            }
            case MessageRowData::Kind::File:
                return kFileCardH;
        }
        return 0;
    }

    float paint_body_block(const MessageRowData& m, tk::PaintCtx& ctx,
                            float x, float y, float col_w) const {
        switch (m.kind) {
            case MessageRowData::Kind::Text:
            case MessageRowData::Kind::Unhandled: {
                float h = paint_wrapped_text(m.body, ctx, x, y, col_w,
                                              ctx.theme.palette.text_primary);
                return y + h;
            }
            case MessageRowData::Kind::Redacted: {
                float h = paint_wrapped_text("Message deleted", ctx, x, y,
                                              col_w,
                                              ctx.theme.palette.text_muted);
                return y + h;
            }
            case MessageRowData::Kind::Image: {
                tk::Size sz = fit_media(m.media_w, m.media_h,
                                         std::min(col_w, kImageMaxW),
                                         kImageMaxH);
                tk::Rect r{ x, y, sz.w, sz.h };
                paint_inline_media(m, ctx, r);
                float cursor = y + sz.h;
                if (m.has_filename_caption && !m.body.empty()) {
                    cursor += 4.0f;
                    float ch = paint_wrapped_text(m.body, ctx, x, cursor,
                                                    col_w,
                                                    ctx.theme.palette.text_primary);
                    cursor += ch;
                }
                return cursor;
            }
            case MessageRowData::Kind::Sticker: {
                float side = std::min(kStickerSize, col_w);
                tk::Rect r{ x, y, side, side };
                paint_inline_media(m, ctx, r);
                return y + side;
            }
            case MessageRowData::Kind::File: {
                float card_w = std::min(kFileCardW, col_w);
                tk::Rect r{ x, y, card_w, kFileCardH };
                paint_file_card(m, ctx, r);
                return y + kFileCardH;
            }
        }
        return y;
    }

    float measure_text_height(const std::string& text, tk::LayoutCtx& ctx,
                                float w) const {
        if (text.empty()) return 0;
        tk::TextStyle s{};
        s.role      = tk::FontRole::Body;
        s.wrap      = true;
        s.max_width = w;
        auto layout = ctx.factory.build_text(text, s);
        return layout ? layout->measure().h : 0;
    }

    float paint_wrapped_text(const std::string& text, tk::PaintCtx& ctx,
                              float x, float y, float w, tk::Color color) const {
        if (text.empty()) return 0;
        tk::TextStyle s{};
        s.role      = tk::FontRole::Body;
        s.wrap      = true;
        s.max_width = w;
        auto layout = ctx.factory.build_text(text, s);
        if (!layout) return 0;
        ctx.canvas.draw_text(*layout, { x, y }, color);
        return layout->measure().h;
    }

    void paint_inline_media(const MessageRowData& m, tk::PaintCtx& ctx,
                             tk::Rect dst) const {
        const tk::Image* img = nullptr;
        if (owner_.image_provider_ && !m.media_url.empty()) {
            img = owner_.image_provider_(m.media_url);
        }
        if (img) {
            ctx.canvas.push_clip_rounded_rect(dst, 8.0f);
            ctx.canvas.draw_image(*img, dst);
            ctx.canvas.pop_clip();
        } else {
            // Placeholder while bytes are still downloading.
            ctx.canvas.fill_rounded_rect(dst, 8.0f,
                                          ctx.theme.palette.chrome_bg);
            ctx.canvas.stroke_rounded_rect(dst, 8.0f,
                                            ctx.theme.palette.border, 1.0f);
        }
    }

    void paint_file_card(const MessageRowData& m, tk::PaintCtx& ctx,
                          tk::Rect dst) const {
        ctx.canvas.fill_rounded_rect(dst, 8.0f, ctx.theme.palette.chrome_bg);
        ctx.canvas.stroke_rounded_rect(dst, 8.0f, ctx.theme.palette.border, 1.0f);

        std::string name = m.file_name.empty() ? m.body : m.file_name;
        std::string size = format_size(m.file_size);

        tk::TextStyle ns{}; ns.role = tk::FontRole::UiSemibold;
        ns.trim = tk::TextTrim::Ellipsis;
        ns.max_width = dst.w - 16.0f;
        auto name_lo = ctx.factory.build_text(name, ns);

        tk::TextStyle ss{}; ss.role = tk::FontRole::Timestamp;
        auto size_lo = ctx.factory.build_text(size, ss);

        if (name_lo) {
            ctx.canvas.draw_text(*name_lo,
                                  { dst.x + 12.0f, dst.y + 10.0f },
                                  ctx.theme.palette.text_primary);
        }
        if (size_lo) {
            ctx.canvas.draw_text(*size_lo,
                                  { dst.x + 12.0f, dst.y + 30.0f },
                                  ctx.theme.palette.text_secondary);
        }
    }

    MessageListView& owner_;
};

// ─────────────────────────────────────────────────────────────────────────

MessageListView::~MessageListView() = default;

MessageListView::MessageListView()
    : adapter_(std::make_unique<Adapter>(*this)) {
    set_adapter(adapter_.get());
    on_row_clicked = [this](int idx) {
        if (idx < 0 || static_cast<std::size_t>(idx) >= messages_.size()) return;
        if (on_message_clicked) on_message_clicked(messages_[idx].event_id);
    };
}

void MessageListView::set_messages(std::vector<MessageRowData> msgs) {
    messages_ = std::move(msgs);
    invalidate_data();
    scroll_to_bottom();
}

void MessageListView::append_message(MessageRowData msg) {
    bool at_bottom =
        scroll_y() + bounds().h + 1.0f >= content_height();
    messages_.push_back(std::move(msg));
    invalidate_data();
    if (at_bottom) scroll_to_bottom();
}

void MessageListView::set_avatar_provider(ImageProvider p) {
    avatar_provider_ = std::move(p);
}

void MessageListView::set_image_provider(ImageProvider p) {
    image_provider_ = std::move(p);
}

} // namespace tesseract::views
