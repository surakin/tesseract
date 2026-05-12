#include "MessageListView.h"

#include "tk/theme.h"
#include <tesseract/settings.h>
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
constexpr float kChipPadX    = 10.0f;

inline float chip_h()      { return static_cast<float>(tesseract::Settings::instance().reaction_chip_height); }
inline float chip_gap()    { return static_cast<float>(tesseract::Settings::instance().reaction_chip_gap); }
inline float chip_radius() { return chip_h() * 0.5f; }
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
        float body_w  = body_text_max_width(width);
        float body_h  = measure_body_block_height(m, ctx, body_w);
        float chips_h = (!m.reactions.empty() || m.timestamp_ms != 0)
                            ? chip_h() : 0.0f;
        // Sender name is centered inside the avatar's vertical band, so
        // the avatar reserves the whole header height; body + reactions
        // stack below it.
        return kPadY + kAvatarSize + body_h + chips_h + kPadY;
    }

    void paint_row(std::size_t index, tk::PaintCtx& ctx, tk::Rect bounds,
                    bool /*selected*/, bool hovered) override {
        if (index >= owner_.messages_.size()) return;
        const auto& m = owner_.messages_[index];

        if (hovered) {
            ctx.canvas.fill_rect(bounds, ctx.theme.palette.subtle_hover);
            // Reset the per-row chip cache for the hovered row. We
            // rebuild it below as the chip strip paints, then the
            // owner's pointer handlers hit-test against it.
            owner_.hovered_row_geom_.row_index   = index;
            owner_.hovered_row_geom_.row_bounds  = bounds;
            owner_.hovered_row_geom_.chips.clear();
            owner_.hovered_row_geom_.add_button  = tk::Rect{};
            owner_.hovered_row_geom_.add_visible = false;
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

        // Sender name — vertically centered against the avatar disc.
        {
            float sender_y =
                bounds.y + kPadY + (kAvatarSize - kSenderH) * 0.5f;
            tk::TextStyle s{};
            s.role      = tk::FontRole::SenderName;
            s.trim      = tk::TextTrim::Ellipsis;
            s.max_width = col_w;
            auto layout = ctx.factory.build_text(
                m.sender_name.empty() ? m.sender : m.sender_name, s);
            if (layout) {
                ctx.canvas.draw_text(*layout, { col_x, sender_y },
                                      ctx.theme.palette.text_secondary);
            }
        }

        // Body block starts below the avatar's bottom edge.
        float cursor = bounds.y + kPadY + kAvatarSize;
        cursor = paint_body_block(m, ctx, col_x, cursor, col_w);

        // Reactions row + timestamp.
        if (!m.reactions.empty() || m.timestamp_ms != 0 || hovered) {
            float chip_y = cursor;
            float chip_x = col_x;
            for (std::size_t ri = 0; ri < m.reactions.size(); ++ri) {
                const auto& r = m.reactions[ri];
                tk::TextStyle est{};
                est.role = tk::FontRole::Title;
                auto emoji_layout = ctx.factory.build_text(r.key, est);
                tk::TextStyle cst{};
                cst.role = tk::FontRole::UiSemibold;
                auto count_layout =
                    ctx.factory.build_text(std::to_string(r.count), cst);
                if (!emoji_layout || !count_layout) {
                    if (hovered) owner_.hovered_row_geom_.chips.push_back({});
                    continue;
                }
                tk::Size esz = emoji_layout->measure();
                tk::Size csz = count_layout->measure();
                constexpr float kChipInnerGap = 4.0f;
                float content_w = esz.w + kChipInnerGap + csz.w;
                float w = std::max(content_w + kChipPadX * 2,
                                    chip_h() + 8.0f);
                tk::Rect pill{ chip_x, chip_y, w, chip_h() };
                bool chip_hovered = hovered
                    && owner_.hover_target_ == HoverTarget::Chip
                    && owner_.hover_chip_idx_ == static_cast<int>(ri);
                tk::Color bg     = r.reacted_by_me ? ctx.theme.palette.chip_bg_me
                                                   : ctx.theme.palette.chip_bg;
                tk::Color border = r.reacted_by_me ? ctx.theme.palette.chip_border_me
                                                   : ctx.theme.palette.chip_border;
                tk::Color text   = r.reacted_by_me ? ctx.theme.palette.chip_text_me
                                                   : ctx.theme.palette.chip_text;
                if (chip_hovered) {
                    border = ctx.theme.palette.accent;
                }
                ctx.canvas.fill_rounded_rect(pill, chip_radius(), bg);
                ctx.canvas.stroke_rounded_rect(pill, chip_radius(), border,
                                                chip_hovered ? 1.5f : 1.0f);
                // Centre the emoji by its *ascent* (top of layout box to
                // baseline), not its full line-height: colour-emoji glyphs
                // fill the ascent region and leave the descender empty, so
                // box-centring leaves them visually high in the chip.
                constexpr float kAscentRatio = 0.78f;
                float emoji_y = pill.y
                              + (pill.h - esz.h * kAscentRatio) * 0.5f;
                float count_y = pill.y + (pill.h - csz.h) * 0.5f;
                float emoji_x = pill.x + kChipPadX;
                ctx.canvas.draw_text(
                    *emoji_layout, { emoji_x, emoji_y }, text);
                ctx.canvas.draw_text(
                    *count_layout,
                    { emoji_x + esz.w + kChipInnerGap, count_y },
                    text);
                if (hovered) owner_.hovered_row_geom_.chips.push_back(pill);
                chip_x += w + chip_gap();
            }

            // Trailing "+" pseudo-chip: only painted while the row is
            // hovered. Reads as a discoverable affordance, not a real
            // reaction — muted background, subtle border.
            if (hovered) {
                tk::TextStyle st{};
                st.role = tk::FontRole::Title;
                auto layout = ctx.factory.build_text("+", st);
                if (layout) {
                    float w = std::max(layout->measure().w + kChipPadX * 2,
                                        chip_h() + 8.0f);
                    tk::Rect pill{ chip_x, chip_y, w, chip_h() };
                    bool add_hovered =
                        owner_.hover_target_ == HoverTarget::AddButton;
                    tk::Color bg = add_hovered
                        ? ctx.theme.palette.subtle_pressed
                        : ctx.theme.palette.subtle_hover;
                    tk::Color border = add_hovered
                        ? ctx.theme.palette.accent
                        : ctx.theme.palette.border;
                    ctx.canvas.fill_rounded_rect(pill, chip_radius(), bg);
                    ctx.canvas.stroke_rounded_rect(pill, chip_radius(), border,
                                                    add_hovered ? 1.5f : 1.0f);
                    ctx.canvas.draw_text(
                        *layout,
                        { pill.x + kChipPadX,
                          pill.y + (pill.h - layout->measure().h) * 0.5f },
                        ctx.theme.palette.text_secondary);
                    owner_.hovered_row_geom_.add_button  = pill;
                    owner_.hovered_row_geom_.add_visible = true;
                    chip_x += w + chip_gap();
                }
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
                    float ty = chip_y + (chip_h() - layout->measure().h) * 0.5f;
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

void MessageListView::insert_message(std::size_t index, MessageRowData msg) {
    if (index > messages_.size()) index = messages_.size();

    // Insertion at (or past) the end is an append: follow the live tail
    // when the user is already pinned there.
    if (index == messages_.size()) {
        bool at_bottom =
            scroll_y() + bounds().h + 1.0f >= content_height();
        messages_.push_back(std::move(msg));
        invalidate_data();
        if (at_bottom) scroll_to_bottom();
        return;
    }

    // Insertion above (or at) the viewport's first visible row: anchor
    // the existing rows so the user's visual position stays put. For
    // mid-viewport inserts the same preserve-top math is a benign no-op
    // (the row the user is looking at stays under their cursor because
    // its row offset shifts by the new row's height, which is what
    // preserve_top_through compensates for).
    preserve_top_through([&]{
        messages_.insert(messages_.begin() + index, std::move(msg));
        invalidate_data();
    });
}

void MessageListView::update_message(std::size_t index, MessageRowData msg) {
    if (index >= messages_.size()) return;
    messages_[index] = std::move(msg);
    invalidate_data();
}

void MessageListView::remove_message(std::size_t index) {
    if (index >= messages_.size()) return;
    preserve_top_through([&]{
        messages_.erase(messages_.begin() + index);
        invalidate_data();
    });
}

void MessageListView::append_message(MessageRowData msg) {
    insert_message(messages_.size(), std::move(msg));
}

void MessageListView::set_avatar_provider(ImageProvider p) {
    avatar_provider_ = std::move(p);
}

void MessageListView::set_image_provider(ImageProvider p) {
    image_provider_ = std::move(p);
}

namespace {

bool rect_contains(const tk::Rect& r, tk::Point p) {
    return p.x >= r.x && p.y >= r.y &&
           p.x <  r.x + r.w && p.y <  r.y + r.h;
}

} // namespace

// Resolve which chip (if any) is under a widget-local point. `local`
// is in widget-local coordinates (relative to MessageListView::bounds_);
// the cached geometry is in world coordinates, so we add the widget
// origin back before comparing.
static MessageListView::HoverTarget chip_hit_at(
        const MessageListView::RowChipGeom& g,
        tk::Rect widget_bounds,
        tk::Point local,
        int& out_chip_idx) {
    out_chip_idx = -1;
    if (g.row_index == static_cast<std::size_t>(-1)) {
        return MessageListView::HoverTarget::None;
    }
    tk::Point world{ local.x + widget_bounds.x,
                       local.y + widget_bounds.y };
    if (!rect_contains(g.row_bounds, world)) {
        return MessageListView::HoverTarget::None;
    }
    for (std::size_t i = 0; i < g.chips.size(); ++i) {
        if (g.chips[i].w <= 0) continue;
        if (rect_contains(g.chips[i], world)) {
            out_chip_idx = static_cast<int>(i);
            return MessageListView::HoverTarget::Chip;
        }
    }
    if (g.add_visible && rect_contains(g.add_button, world)) {
        return MessageListView::HoverTarget::AddButton;
    }
    return MessageListView::HoverTarget::None;
}

void MessageListView::on_pointer_move(tk::Point local) {
    tk::ListView::on_pointer_move(local);
    // Row hover may have changed; if the new hovered row is different
    // from the one we have geometry for, invalidate so paint_row will
    // rebuild it. (Paint will populate hovered_row_geom_ on its next
    // frame; until then chip hit-tests will return None.)
    int row = hovered_row_index();
    if (row < 0 ||
        static_cast<std::size_t>(row) != hovered_row_geom_.row_index) {
        hovered_row_geom_.row_index   = static_cast<std::size_t>(-1);
        hovered_row_geom_.chips.clear();
        hovered_row_geom_.add_visible = false;
    }
    int chip_idx = -1;
    HoverTarget t = chip_hit_at(hovered_row_geom_, bounds(),
                                 local, chip_idx);
    if (t != hover_target_ || chip_idx != hover_chip_idx_) {
        hover_target_   = t;
        hover_chip_idx_ = chip_idx;
    }
}

void MessageListView::on_pointer_leave() {
    tk::ListView::on_pointer_leave();
    hovered_row_geom_.row_index   = static_cast<std::size_t>(-1);
    hovered_row_geom_.chips.clear();
    hovered_row_geom_.add_visible = false;
    hover_target_   = HoverTarget::None;
    hover_chip_idx_ = -1;
    press_pill_     = false;
}

bool MessageListView::should_show_pill() const {
    if (content_height() <= bounds().h) return false;
    return scroll_y() + bounds().h + 1.0f < content_height();
}

bool MessageListView::on_pointer_down(tk::Point local) {
    if (pill_visible_) {
        tk::Point world{ local.x + bounds().x, local.y + bounds().y };
        if (rect_contains(pill_rect_, world)) {
            press_pill_ = true;
            return true;
        }
    }

    int chip_idx = -1;
    HoverTarget t = chip_hit_at(hovered_row_geom_, bounds(),
                                 local, chip_idx);
    if (t == HoverTarget::None) {
        return tk::ListView::on_pointer_down(local);
    }
    std::size_t row = hovered_row_geom_.row_index;
    if (row >= messages_.size()) {
        return tk::ListView::on_pointer_down(local);
    }
    press_target_    = t;
    press_chip_idx_  = chip_idx;
    press_event_id_  = messages_[row].event_id;
    return true;
}

void MessageListView::on_pointer_up(tk::Point local, bool inside_self) {
    if (press_pill_) {
        bool fire = inside_self;
        press_pill_ = false;
        if (fire) {
            tk::Point world{ local.x + bounds().x, local.y + bounds().y };
            if (rect_contains(pill_rect_, world)) scroll_to_bottom();
        }
        return;
    }
    if (press_target_ == HoverTarget::None) {
        tk::ListView::on_pointer_up(local, inside_self);
        return;
    }
    HoverTarget t = press_target_;
    int idx       = press_chip_idx_;
    std::string ev = std::move(press_event_id_);
    press_target_   = HoverTarget::None;
    press_chip_idx_ = -1;
    press_event_id_.clear();
    if (!inside_self) return;

    if (t == HoverTarget::Chip) {
        // Confirm the release still lands on the same chip.
        int now_idx = -1;
        HoverTarget now_t = chip_hit_at(hovered_row_geom_, bounds(),
                                         local, now_idx);
        if (now_t != HoverTarget::Chip || now_idx != idx) return;
        // Find the row that geometry was captured for and read the
        // reaction key directly off the model.
        std::size_t row = hovered_row_geom_.row_index;
        if (row >= messages_.size()) return;
        const auto& reactions = messages_[row].reactions;
        if (idx < 0 || static_cast<std::size_t>(idx) >= reactions.size())
            return;
        if (on_reaction_toggled) {
            on_reaction_toggled(ev, reactions[idx].key);
        }
    } else if (t == HoverTarget::AddButton) {
        int now_idx = -1;
        HoverTarget now_t = chip_hit_at(hovered_row_geom_, bounds(),
                                         local, now_idx);
        if (now_t != HoverTarget::AddButton) return;
        if (on_add_reaction_requested) {
            on_add_reaction_requested(ev, hovered_row_geom_.add_button);
        }
    }
}

void MessageListView::paint(tk::PaintCtx& ctx) {
    tk::ListView::paint(ctx);

    // Scroll-to-bottom pill — overlays the bottom-right corner of the
    // viewport when the user is not pinned to the live tail. Painted
    // before the chip tooltip so the tooltip (rare, hover-only) wins on
    // any geometric overlap. Click handling lives in on_pointer_*.
    pill_visible_ = should_show_pill();
    if (pill_visible_) {
        constexpr float kSz = 36.0f, kInsetR = 12.0f, kInsetB = 16.0f;
        tk::Rect v = bounds();
        pill_rect_ = { v.x + v.w - kSz - kInsetR,
                       v.y + v.h - kSz - kInsetB, kSz, kSz };
        auto bg = press_pill_ ? ctx.theme.palette.subtle_pressed
                              : ctx.theme.palette.chrome_bg;
        ctx.canvas.fill_rounded_rect  (pill_rect_, kSz * 0.5f, bg);
        ctx.canvas.stroke_rounded_rect(pill_rect_, kSz * 0.5f,
                                        ctx.theme.palette.border, 1.0f);
        tk::TextStyle gs{};
        gs.role = tk::FontRole::UiSemibold;
        gs.wrap = false;
        auto glyph = ctx.factory.build_text("\xE2\x86\x93", gs); // U+2193 ↓
        if (glyph) {
            tk::Size sz = glyph->measure();
            ctx.canvas.draw_text(*glyph,
                { pill_rect_.x + (kSz - sz.w) * 0.5f,
                  pill_rect_.y + (kSz - sz.h) * 0.5f },
                ctx.theme.palette.text_primary);
        }
    } else {
        pill_rect_ = {};
    }

    // Tooltip overlay: paint a small panel listing senders of the
    // hovered reaction chip. We paint after rows so the panel sits on
    // top of subsequent rows when it dips below the chip.
    if (hover_target_ != HoverTarget::Chip) return;
    if (hover_chip_idx_ < 0) return;
    std::size_t row = hovered_row_geom_.row_index;
    if (row >= messages_.size()) return;
    const auto& reactions = messages_[row].reactions;
    if (static_cast<std::size_t>(hover_chip_idx_) >= reactions.size()) return;
    const auto& r = reactions[hover_chip_idx_];
    if (r.senders.empty()) return;

    // Build one TextLayout per line. Canvas backends measure single-line
    // text via advance width / font height; a multi-line string returns
    // single-line dimensions even though draw renders the newlines, which
    // would clip the panel. Stacking per-line layouts gives the panel an
    // accurate height and correct max width across all backends.
    std::vector<std::string> lines;
    lines.reserve(r.senders.size() + 1);
    {
        std::string header = "Reacted with ";
        header += r.key;
        header += ":";
        lines.push_back(std::move(header));
    }
    for (const auto& s : r.senders) lines.push_back(s);

    tk::TextStyle st{};
    st.role = tk::FontRole::Small;
    st.wrap = false;

    struct LineLayout {
        std::unique_ptr<tk::TextLayout> layout;
        tk::Size size{};
    };
    std::vector<LineLayout> ls;
    ls.reserve(lines.size());
    float max_w = 0.0f;
    float total_h = 0.0f;
    for (const auto& line : lines) {
        auto layout = ctx.factory.build_text(line, st);
        if (!layout) return;
        tk::Size sz = layout->measure();
        max_w = std::max(max_w, sz.w);
        total_h += sz.h;
        ls.push_back({ std::move(layout), sz });
    }

    constexpr float kTipPadX = 8.0f;
    constexpr float kTipPadY = 6.0f;
    float panel_w = max_w + kTipPadX * 2;
    float panel_h = total_h + kTipPadY * 2;

    tk::Rect chip = hovered_row_geom_.chips[hover_chip_idx_];
    tk::Rect view = bounds();

    // Prefer above the chip; flip below if it would clip the top.
    float panel_y = chip.y - panel_h - 4.0f;
    if (panel_y < view.y) {
        panel_y = chip.y + chip.h + 4.0f;
    }
    float panel_x = chip.x;
    if (panel_x + panel_w > view.x + view.w) {
        panel_x = view.x + view.w - panel_w - 4.0f;
    }
    if (panel_x < view.x + 4.0f) {
        panel_x = view.x + 4.0f;
    }

    tk::Rect panel{ panel_x, panel_y, panel_w, panel_h };
    ctx.canvas.fill_rounded_rect(panel, 6.0f, ctx.theme.palette.chrome_bg);
    ctx.canvas.stroke_rounded_rect(panel, 6.0f, ctx.theme.palette.border, 1.0f);

    float y = panel.y + kTipPadY;
    for (const auto& line : ls) {
        ctx.canvas.draw_text(*line.layout,
                              { panel.x + kTipPadX, y },
                              ctx.theme.palette.text_primary);
        y += line.size.h;
    }
}

} // namespace tesseract::views
