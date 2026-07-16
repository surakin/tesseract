#ifdef TESSERACT_CALLS_ENABLED
#include "IncomingCallBanner.h"

#include "icons.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr float kIncomingCallBannerPadX   = 12.0f;
constexpr float kIconSz = 20.0f;
constexpr float kIncomingCallBannerGap    = 10.0f;
constexpr float kIncomingCallBannerBtnW   = 76.0f;
constexpr float kIncomingCallBannerBtnH   = 28.0f;
constexpr float kIncomingCallBannerBtnGap =  8.0f;
} // namespace

IncomingCallBanner::IncomingCallBanner()
{
    auto dec = tk::create_widget<tk::Button>(this,
        tk::tr("Decline"), std::function<void()>{}, tk::Button::Variant::Subtle);
    decline_btn_ = add_child(std::move(dec));

    auto ans = tk::create_widget<tk::Button>(this,
        tk::tr("Answer"), std::function<void()>{}, tk::Button::Variant::Primary);
    answer_btn_ = add_child(std::move(ans));

    set_visible(false);
}

void IncomingCallBanner::set_call(std::string           caller_display_name,
                                   const std::string&    call_intent,
                                   std::function<void()> on_answer,
                                   std::function<void()> on_decline)
{
    caller_name_ = std::move(caller_display_name);
    call_intent_ = call_intent;
    if (answer_btn_)  answer_btn_->set_on_click(std::move(on_answer));
    if (decline_btn_) decline_btn_->set_on_click(std::move(on_decline));
    set_visible(true);
}

void IncomingCallBanner::clear()
{
    caller_name_.clear();
    call_intent_.clear();
    if (answer_btn_)  answer_btn_->set_on_click({});
    if (decline_btn_) decline_btn_->set_on_click({});
    set_visible(false);
}

tk::Size IncomingCallBanner::measure(tk::LayoutCtx&, tk::Size constraints)
{
    if (!visible()) return {0.0f, 0.0f};
    return {constraints.w, kBannerH};
}

void IncomingCallBanner::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;
    if (bounds_.h <= 0.0f) return;

    const float mid_y = bounds_.y + bounds_.h * 0.5f;

    icon_rect_ = {bounds_.x + kIncomingCallBannerPadX,
                  mid_y - kIconSz * 0.5f,
                  kIconSz, kIconSz};

    // Right side: [Decline] [Answer] with kIncomingCallBannerPadX outer margin.
    const float btn_y = mid_y - kIncomingCallBannerBtnH * 0.5f;
    const float right = bounds_.x + bounds_.w - kIncomingCallBannerPadX;
    const tk::Rect ans_r = {right - kIncomingCallBannerBtnW, btn_y, kIncomingCallBannerBtnW, kIncomingCallBannerBtnH};
    const tk::Rect dec_r = {ans_r.x - kIncomingCallBannerBtnGap - kIncomingCallBannerBtnW, btn_y, kIncomingCallBannerBtnW, kIncomingCallBannerBtnH};

    if (answer_btn_)  answer_btn_->arrange(ctx, ans_r);
    if (decline_btn_) decline_btn_->arrange(ctx, dec_r);

    // Text area between icon and decline button.
    const float text_x = icon_rect_.x + kIconSz + kIncomingCallBannerGap;
    const float text_w = std::max(0.0f, dec_r.x - kIncomingCallBannerGap - text_x);
    text_rect_ = {text_x, bounds_.y, text_w, bounds_.h};
}

void IncomingCallBanner::paint(tk::PaintCtx& ctx)
{
    if (bounds_.h <= 0.0f) return;

    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.accent);

    constexpr tk::Color kWhite{255, 255, 255, 255};

    // Choose icon based on call intent.
    const bool is_video = (call_intent_ == "video");
    if (is_video)
        video_icon_.draw(ctx.canvas, ctx.factory, kVideoSvg,
                         icon_rect_, kIconSz, kWhite);
    else
        phone_icon_.draw(ctx.canvas, ctx.factory, kPhoneSvg,
                         icon_rect_, kIconSz, kWhite);

    // Determine label: "Video call", "Voice call", or plain "Call".
    const std::string intent_label = is_video
        ? tk::tr("Video call")
        : (call_intent_ == "audio" ? tk::tr("Voice call") : tk::tr("Call"));

    // Render as "Alice — Video call" (em-dash separator).
    const std::string text = caller_name_.empty()
        ? intent_label
        : caller_name_ + " \xe2\x80\x94 " + intent_label;

    if (text_rect_.w > 0.0f)
    {
        tk::TextStyle ts{};
        ts.role      = tk::FontRole::Body;
        ts.trim      = tk::TextTrim::Ellipsis;
        ts.max_width = text_rect_.w;
        auto layout  = ctx.factory.build_text(text, ts);
        if (layout)
        {
            const tk::Size sz = layout->measure();
            const float ty = text_rect_.y + (text_rect_.h - sz.h) * 0.5f;
            ctx.canvas.draw_text(*layout, {text_rect_.x, ty}, kWhite);
        }
    }

    if (decline_btn_) decline_btn_->paint(ctx);
    if (answer_btn_)  answer_btn_->paint(ctx);
}

} // namespace tesseract::views
#endif // TESSERACT_CALLS_ENABLED
