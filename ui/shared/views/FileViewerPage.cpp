#include "FileViewerPage.h"
#include "icons.h"
#include "media_utils.h"

#include "tk/i18n.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr float kFileCardW = 400.0f;
constexpr float kFileCardH = 300.0f;
constexpr float kFileCardPad = 20.0f;
constexpr float kFileGlyphPx = 56.0f;
constexpr float kFileSaveW = 140.0f;
constexpr float kFileSaveH = 40.0f;
constexpr tk::Color kFileCardFill = tk::Color::rgba(28, 28, 32, 235);
constexpr tk::Color kFileCardBorder = tk::Color::rgba(255, 255, 255, 36);
constexpr tk::Color kFileTextPrimary = tk::Color::rgba(255, 255, 255, 235);
constexpr tk::Color kFileTextSecondary = tk::Color::rgba(255, 255, 255, 140);
} // namespace

FileViewerPage::FileViewerPage(MediaViewerPageHost& host, std::function<void()> on_save)
    : host_(host)
{
    save_btn_ = host_.host_add_label_button(tk::tr("Save"));
    save_btn_->set_leading_icon(kDownloadSvg, 16.0f);
    save_btn_->set_on_click(std::move(on_save));
    save_btn_->set_visible(false);
}

void FileViewerPage::activate(const MediaViewerItem& item)
{
    filename_ = item.filename;
    caption_ = item.caption;
    mime_type_ = item.mime_type;
    file_size_ = item.file_size;
    active_ = true;
    save_btn_->set_visible(true);
}

void FileViewerPage::deactivate()
{
    active_ = false;
    save_btn_->set_visible(false);
}

std::string FileViewerPage::title_() const
{
    if (!filename_.empty())
    {
        return filename_;
    }
    if (!caption_.empty())
    {
        return caption_;
    }
    return tk::tr("File");
}

void FileViewerPage::arrange(tk::LayoutCtx& lc, tk::Rect /*b*/)
{
    const tk::Rect b = host_.host_bounds();
    if (b.w <= 0 || b.h <= 0)
    {
        return;
    }
    const float w = std::min(kFileCardW, std::max(1.0f, b.w - 48.0f));
    const float h = std::min(kFileCardH, std::max(1.0f, b.h - 48.0f));
    card_ = {b.x + (b.w - w) * 0.5f, b.y + (b.h - h) * 0.5f, w, h};
    save_rect_ = {card_.x + (card_.w - kFileSaveW) * 0.5f,
                  card_.y + card_.h - kFileCardPad - kFileSaveH, kFileSaveW, kFileSaveH};
    save_btn_->arrange(lc, save_rect_);
    save_btn_->set_visible(active_);
}

bool FileViewerPage::controls_hovered() const
{
    return save_btn_ && save_btn_->hovered();
}

void FileViewerPage::paint_content(tk::PaintCtx& ctx)
{
    if (!active_)
    {
        return;
    }
    auto& cv = ctx.canvas;
    cv.fill_rounded_rect(card_, 12.0f, kFileCardFill);
    cv.stroke_rounded_rect(card_, 12.0f, kFileCardBorder, 1.0f);

    const float glyph_y = card_.y + kFileCardPad;
    glyph_.draw(cv, ctx.factory, kFileSvg,
                {card_.x + (card_.w - kFileGlyphPx) * 0.5f, glyph_y, kFileGlyphPx,
                 kFileGlyphPx},
                kFileGlyphPx, kFileTextSecondary);

    float y = glyph_y + kFileGlyphPx + 12.0f;
    auto centred = [&](const std::string& text, tk::FontRole role, tk::Color col)
    {
        tk::TextStyle st{};
        st.role = role;
        st.max_width = card_.w - 2.0f * kFileCardPad;
        auto lo = ctx.factory.build_text(text, st);
        if (!lo)
        {
            return;
        }
        const tk::Size sz = lo->measure();
        cv.draw_text(*lo, {card_.x + (card_.w - sz.w) * 0.5f, y}, col);
        y += sz.h + 4.0f;
    };
    centred(title_(), tk::FontRole::UiSemibold, kFileTextPrimary);

    std::string meta;
    if (file_size_ > 0)
    {
        meta = tk::format_size(file_size_);
    }
    if (!mime_type_.empty())
    {
        meta += meta.empty() ? mime_type_ : " \xC2\xB7 " + mime_type_;
    }
    if (!meta.empty())
    {
        centred(meta, tk::FontRole::Timestamp, kFileTextSecondary);
    }
    centred(tk::tr("No preview available"), tk::FontRole::Body, kFileTextSecondary);
}

void FileViewerPage::paint_controls(tk::PaintCtx& ctx)
{
    if (active_)
    {
        save_btn_->paint(ctx);
    }
}

bool FileViewerPage::on_content_pointer_down(tk::Point w, tk::Point /*local*/)
{
    return rect_contains(card_, w);
}

} // namespace tesseract::views
