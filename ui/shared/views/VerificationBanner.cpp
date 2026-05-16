#include "VerificationBanner.h"

#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views {

namespace {

constexpr float kHeightNormal    = 48.0f;
constexpr float kHeightShowEmoji = 124.0f;
constexpr float kPadX            = 12.0f;
constexpr float kPadY            = 8.0f;
constexpr float kGap             = 8.0f;
constexpr float kBtnH            = 28.0f;
constexpr float kDismissSide     = 24.0f;
constexpr float kEmojiTileH      = 64.0f;
constexpr float kEmojiGlyphH     = 40.0f;
constexpr float kEmojiLabelH     = 24.0f;
constexpr float kEmojiCorner     = 4.0f;

const tk::Color kBannerBg     = tk::Color::rgb(0xFFF4D6);
const tk::Color kBannerBorder = tk::Color::rgb(0xE0C97A);
const tk::Color kLabelText    = tk::Color::rgb(0x5C4500);
const tk::Color kEmojiTileBg  = tk::Color::rgb(0xFFFBE8);

} // namespace

VerificationBanner::VerificationBanner() {
    auto label = std::make_unique<tk::Label>("", tk::FontRole::Body);
    label->set_colour(kLabelText);
    label->set_halign(tk::TextHAlign::Leading);
    label->set_trim(tk::TextTrim::Ellipsis);
    label_ = add_child(std::move(label));

    // Primary action button (Verify / Accept / They Match / —)
    auto primary = std::make_unique<tk::Button>(
        "Verify", std::function<void()>{}, tk::Button::Variant::Primary);
    primary->set_on_click([this] {
        switch (state_) {
            case State::Prompt:          if (on_verify)   on_verify();   break;
            case State::IncomingRequest: if (on_accept)   on_accept();   break;
            case State::ShowEmojis:      if (on_match)    on_match();    break;
            default: break;
        }
    });
    primary->set_min_size({ 90.0f, kBtnH });
    primary_ = add_child(std::move(primary));

    // Secondary action button (Decline / Cancel / No Match)
    auto secondary = std::make_unique<tk::Button>(
        "Decline", std::function<void()>{}, tk::Button::Variant::Subtle);
    secondary->set_on_click([this] {
        switch (state_) {
            case State::IncomingRequest: if (on_dismiss)  on_dismiss();  break;
            case State::Waiting:         if (on_cancel)   on_cancel();   break;
            case State::ShowEmojis:      if (on_mismatch) on_mismatch(); break;
            default: break;
        }
    });
    secondary->set_min_size({ 90.0f, kBtnH });
    secondary_ = add_child(std::move(secondary));

    // Dismiss "✕" — visible in Prompt and Cancelled states only
    auto dismiss = std::make_unique<tk::Button>(
        "✕", std::function<void()>{}, tk::Button::Variant::Subtle);
    dismiss->set_on_click([this] {
        if (on_dismiss) on_dismiss();
    });
    dismiss->set_min_size({ kDismissSide, kDismissSide });
    dismiss_ = add_child(std::move(dismiss));

    // "Use recovery key" link — visible in Prompt state only
    auto link = std::make_unique<tk::Button>(
        "Use recovery key", std::function<void()>{}, tk::Button::Variant::Subtle);
    link->set_on_click([this] {
        if (on_use_recovery_key) on_use_recovery_key();
    });
    link->set_min_size({ 0.0f, kBtnH });
    link_ = add_child(std::move(link));

    apply_state();
}

void VerificationBanner::set_state(State s) {
    state_ = s;
    if (s != State::Cancelled) cancel_reason_.clear();
    if (s != State::ShowEmojis) emojis_.clear();
    apply_state();
}

void VerificationBanner::set_emojis(const std::vector<VerificationEmoji>& emojis) {
    emojis_ = emojis;
    state_  = State::ShowEmojis;
    apply_state();
}

void VerificationBanner::set_cancel_reason(std::string reason) {
    cancel_reason_ = std::move(reason);
    if (label_) label_->set_text(label_text());
}

std::string VerificationBanner::label_text() const {
    switch (state_) {
        case State::Prompt:
            return "Verify this device to confirm your identity.";
        case State::IncomingRequest:
            return "Another device wants to verify. Accept to compare emoji.";
        case State::Waiting:
            return "Waiting for the other device to accept…";
        case State::ShowEmojis:
            return "Do these emoji match what the other device shows?";
        case State::Confirming:
            return "Confirming…";
        case State::Done:
            return "Device verified ✓";
        case State::Cancelled:
            return cancel_reason_.empty()
                ? std::string{"Verification cancelled."}
                : "Verification cancelled: " + cancel_reason_;
    }
    return {};
}

void VerificationBanner::apply_state() {
    if (label_) label_->set_text(label_text());

    // Determine which buttons are visible and what they say.
    bool show_primary   = false;
    bool show_secondary = false;
    bool show_dismiss   = false;
    std::string primary_text;
    std::string secondary_text;

    switch (state_) {
        case State::Prompt:
            show_primary   = true; primary_text   = "Verify";
            show_dismiss   = true;
            break;
        case State::IncomingRequest:
            show_primary   = true; primary_text   = "Accept";
            show_secondary = true; secondary_text = "Decline";
            break;
        case State::Waiting:
            show_secondary = true; secondary_text = "Cancel";
            break;
        case State::ShowEmojis:
            show_primary   = true; primary_text   = "They Match";
            show_secondary = true; secondary_text = "No Match";
            break;
        case State::Confirming:
        case State::Done:
            break;
        case State::Cancelled:
            show_dismiss   = true;
            break;
    }

    if (primary_)   { primary_->set_label(primary_text);     primary_->set_visible(show_primary); }
    if (secondary_) { secondary_->set_label(secondary_text); secondary_->set_visible(show_secondary); }
    if (dismiss_)   { dismiss_->set_visible(show_dismiss); }
    if (link_)      { link_->set_visible(state_ == State::Prompt); }
}

tk::Size VerificationBanner::measure(tk::LayoutCtx&, tk::Size constraints) {
    float h = (state_ == State::ShowEmojis) ? kHeightShowEmoji : kHeightNormal;
    return { constraints.w, h };
}

void VerificationBanner::arrange(tk::LayoutCtx& ctx, tk::Rect bounds) {
    bounds_ = bounds;

    if (state_ == State::ShowEmojis) {
        // ── ShowEmojis layout ─────────────────────────────────────────────
        // Top 48 px: label centred, no buttons.
        label_rect_ = {
            bounds.x + kPadX,
            bounds.y + (kHeightNormal - 20.0f) * 0.5f,
            bounds.w - kPadX * 2,
            20.0f
        };
        if (label_) label_->arrange(ctx, label_rect_);

        // Bottom 76 px: 7 emoji tiles packed horizontally with equal gaps.
        float tile_y    = bounds.y + kHeightNormal;
        float tile_area = bounds.w - kPadX * 2;
        float tile_w    = (tile_area - kGap * (kEmojiCount - 1)) / kEmojiCount;
        float tile_x    = bounds.x + kPadX;
        for (int i = 0; i < kEmojiCount; ++i) {
            emoji_rects_[i] = { tile_x, tile_y, tile_w, kEmojiTileH };
            emoji_label_rects_[i] = {
                tile_x,
                tile_y + kEmojiGlyphH,
                tile_w,
                kEmojiLabelH
            };
            tile_x += tile_w + kGap;
        }

        // Buttons not shown in ShowEmojis — arrange They Match / No Match
        // in the normal strip area so pointer dispatch works.
        float btn_y = bounds.y + (kHeightNormal - kBtnH) * 0.5f;
        float right = bounds.x + bounds.w - kPadX;
        if (secondary_ && secondary_->visible()) {
            auto sz = secondary_->measure(ctx, { 100.0f, kBtnH });
            secondary_rect_ = { right - sz.w, btn_y, sz.w, kBtnH };
            secondary_->arrange(ctx, secondary_rect_);
            right = secondary_rect_.x - kGap;
        }
        if (primary_ && primary_->visible()) {
            auto sz = primary_->measure(ctx, { 100.0f, kBtnH });
            primary_rect_ = { right - sz.w, btn_y, sz.w, kBtnH };
            primary_->arrange(ctx, primary_rect_);
        }
        dismiss_rect_ = {};
        return;
    }

    // ── Normal 48 px layout ───────────────────────────────────────────────
    float right = bounds.x + bounds.w - kPadX;

    dismiss_rect_ = {};
    if (dismiss_ && dismiss_->visible()) {
        dismiss_rect_ = {
            right - kDismissSide,
            bounds.y + (bounds.h - kDismissSide) * 0.5f,
            kDismissSide,
            kDismissSide
        };
        dismiss_->arrange(ctx, dismiss_rect_);
        right = dismiss_rect_.x - kGap;
    }

    secondary_rect_ = {};
    if (secondary_ && secondary_->visible()) {
        auto sz = secondary_->measure(ctx, { 100.0f, kBtnH });
        secondary_rect_ = { right - sz.w, bounds.y + (bounds.h - kBtnH) * 0.5f,
                             sz.w, kBtnH };
        secondary_->arrange(ctx, secondary_rect_);
        right = secondary_rect_.x - kGap;
    }

    primary_rect_ = {};
    if (primary_ && primary_->visible()) {
        auto sz = primary_->measure(ctx, { 100.0f, kBtnH });
        primary_rect_ = { right - sz.w, bounds.y + (bounds.h - kBtnH) * 0.5f,
                          sz.w, kBtnH };
        primary_->arrange(ctx, primary_rect_);
        right = primary_rect_.x - kGap;
    }

    link_rect_ = {};
    if (link_ && link_->visible()) {
        auto sz = link_->measure(ctx, { 200.0f, kBtnH });
        link_rect_ = { right - sz.w, bounds.y + (bounds.h - kBtnH) * 0.5f,
                       sz.w, kBtnH };
        link_->arrange(ctx, link_rect_);
        right = link_rect_.x - kGap;
    }

    label_rect_ = {
        bounds.x + kPadX,
        bounds.y + (bounds.h - 20.0f) * 0.5f,
        std::max(0.0f, right - (bounds.x + kPadX)),
        20.0f
    };
    if (label_) label_->arrange(ctx, label_rect_);
}

void VerificationBanner::paint(tk::PaintCtx& ctx) {
    ctx.canvas.fill_rect(bounds_, kBannerBg);
    tk::Rect border{ bounds_.x, bounds_.y + bounds_.h - 1.0f, bounds_.w, 1.0f };
    ctx.canvas.fill_rect(border, kBannerBorder);

    if (label_) label_->paint(ctx);

    if (state_ == State::ShowEmojis) {
        // Paint 7 emoji tiles.
        tk::TextStyle glyph_style;
        glyph_style.role   = tk::FontRole::Title;
        glyph_style.halign = tk::TextHAlign::Center;
        glyph_style.valign = tk::TextVAlign::Center;

        tk::TextStyle caption_style;
        caption_style.role   = tk::FontRole::Small;
        caption_style.halign = tk::TextHAlign::Center;
        caption_style.valign = tk::TextVAlign::Center;

        for (int i = 0; i < kEmojiCount; ++i) {
            const auto& r = emoji_rects_[i];
            if (r.w <= 0) continue;

            ctx.canvas.fill_rounded_rect(r, kEmojiCorner, kEmojiTileBg);

            // Glyph
            const std::string& sym = (i < static_cast<int>(emojis_.size()))
                ? emojis_[i].symbol : "";
            if (!sym.empty()) {
                auto layout = ctx.factory.build_text(sym, glyph_style);
                if (layout) {
                    tk::Rect glyph_rect{ r.x, r.y, r.w, kEmojiGlyphH };
                    tk::Point origin{
                        glyph_rect.x + (glyph_rect.w - layout->measure().w) * 0.5f,
                        glyph_rect.y + (glyph_rect.h - layout->measure().h) * 0.5f
                    };
                    ctx.canvas.draw_text(*layout, origin, kLabelText);
                }
            }

            // Description label
            const std::string& desc = (i < static_cast<int>(emojis_.size()))
                ? emojis_[i].description : "";
            if (!desc.empty()) {
                auto layout = ctx.factory.build_text(desc, caption_style);
                if (layout) {
                const auto& lr = emoji_label_rects_[i];
                tk::Point origin{
                    lr.x + (lr.w - layout->measure().w) * 0.5f,
                    lr.y + (lr.h - layout->measure().h) * 0.5f
                };
                ctx.canvas.draw_text(*layout, origin, kLabelText);
                }
            }
        }

        // Buttons in ShowEmojis also live in the top 48 px strip
        if (secondary_ && secondary_->visible()) secondary_->paint(ctx);
        if (primary_ && primary_->visible()) primary_->paint(ctx);
        return;
    }

    // Normal state buttons
    if (primary_ && primary_->visible())     primary_->paint(ctx);
    if (secondary_ && secondary_->visible()) secondary_->paint(ctx);
    if (dismiss_ && dismiss_->visible())     dismiss_->paint(ctx);
    if (link_ && link_->visible())           link_->paint(ctx);
}

} // namespace tesseract::views
