#include "UserPackEditor.h"

#include "tk/theme.h"

namespace tesseract::views
{

UserPackEditor::UserPackEditor() = default;
UserPackEditor::~UserPackEditor() = default;

void UserPackEditor::set_images(std::vector<tesseract::ImagePackImage> images)
{
    dirty_ = false;
    images_.clear();
    images_.reserve(images.size());
    for (auto& img : images)
    {
        StagedPackImage s;
        s.shortcode    = std::move(img.shortcode);
        s.existing_url = std::move(img.url);
        s.body         = std::move(img.body);
        s.info_json    = std::move(img.info_json);
        s.usage        = img.usage;
        s.favorite     = img.favorite;
        images_.push_back(std::move(s));
    }
    removed_shortcodes_.clear();
    editing_.reset();
    clamp_scroll();
    layout_changed_();
}

void UserPackEditor::set_tile_preview(std::uint64_t local_id,
                                      std::shared_ptr<tk::Image> image)
{
    for (auto& img : images_)
    {
        if (img.local_id == local_id)
        {
            img.local_preview = std::move(image);
            clamp_scroll();
            return;
        }
    }
}

void UserPackEditor::add_pending_image_(std::vector<std::uint8_t> bytes,
                                        std::string mime, std::string filename)
{
    if (committing_)
        return;
    StagedPackImage img;
    img.local_id      = ++next_local_id_;
    img.pending_bytes = std::move(bytes);
    img.pending_mime  = std::move(mime);
    if (!filename.empty())
        img.shortcode = dedupe_pack_shortcode(images_, suggest_pack_shortcode_from_filename(filename));
    images_.push_back(std::move(img));
    const std::size_t new_tile_idx = images_.size() - 1;
    dirty_ = true;
    clamp_scroll();

    if (on_pending_image_added)
    {
        const auto& staged_img = images_[new_tile_idx];
        on_pending_image_added(staged_img.local_id, staged_img.pending_bytes,
                               staged_img.pending_mime);
    }
    begin_editing_shortcode_(new_tile_idx);
}

void UserPackEditor::add_pasted_image(std::vector<std::uint8_t> bytes,
                                      std::string mime)
{
    add_pending_image_(std::move(bytes), std::move(mime), {});
}

void UserPackEditor::add_dropped_image(tk::Point /*world*/,
                                       std::vector<std::uint8_t> bytes,
                                       std::string mime, std::string filename)
{
    add_pending_image_(std::move(bytes), std::move(mime), std::move(filename));
}

bool UserPackEditor::on_file_drop(tk::Point /*local*/, tk::FileDropPayload& payload)
{
    add_pending_image_(std::move(payload.bytes), std::move(payload.mime),
                       std::move(payload.filename));
    return true;
}

bool UserPackEditor::on_drag_hover(tk::Point /*local*/)
{
    drag_hover_ = true;
    return true;
}

void UserPackEditor::on_drag_leave()
{
    drag_hover_ = false;
}

tk::Rect UserPackEditor::shortcode_edit_rect() const
{
    if (!editing_ || committing_)
        return {};
    const auto layout = layout_tile_row_(bounds_.w, images_.size() + 1, 0.0f);
    if (*editing_ >= layout.size())
        return {};
    const auto& r = layout[*editing_];
    const tk::Rect world{bounds_.x + r.x, bounds_.y - scroll_y_ + r.y + kImageH, r.w,
                         kLabelH};
    if (world.bottom() <= bounds_.y || world.y >= bounds_.y + bounds_.h)
        return {}; // scrolled out of the viewport
    return world;
}

std::string UserPackEditor::shortcode_edit_initial_text() const
{
    if (!editing_ || *editing_ >= images_.size())
        return {};
    return images_[*editing_].shortcode;
}

void UserPackEditor::set_editing_shortcode_text(std::string text)
{
    if (!editing_ || *editing_ >= images_.size())
        return;
    images_[*editing_].shortcode = std::move(text);
}

void UserPackEditor::commit_editing_shortcode()
{
    if (!editing_)
        return;
    dirty_ = true;
    editing_.reset();
    layout_changed_();
}

void UserPackEditor::cancel_editing_shortcode()
{
    if (!editing_)
        return;
    if (*editing_ < images_.size())
        images_[*editing_].shortcode = editing_shortcode_original_;
    editing_.reset();
    layout_changed_();
}

void UserPackEditor::begin_editing_shortcode_(std::size_t tile_idx)
{
    if (tile_idx >= images_.size())
        return;
    editing_ = tile_idx;
    editing_shortcode_original_ = images_[tile_idx].shortcode;
    ++shortcode_edit_reset_gen_;
    layout_changed_();
}

void UserPackEditor::remove_tile_(std::size_t tile_idx)
{
    if (tile_idx >= images_.size() || committing_)
        return;
    if (!images_[tile_idx].existing_url.empty() &&
        !images_[tile_idx].shortcode.empty())
    {
        removed_shortcodes_.push_back(images_[tile_idx].shortcode);
    }
    images_.erase(images_.begin() + static_cast<std::ptrdiff_t>(tile_idx));
    dirty_ = true;

    if (editing_)
    {
        if (*editing_ == tile_idx)
            editing_.reset();
        else if (*editing_ > tile_idx)
            *editing_ -= 1;
    }
    clamp_scroll();
    layout_changed_();
}

UserPackEditor::Result UserPackEditor::build_result() const
{
    Result result;
    result.images            = images_;
    result.removed_shortcodes = removed_shortcodes_;
    return result;
}

void UserPackEditor::set_committing(bool committing)
{
    committing_ = committing;
    layout_changed_();
}

void UserPackEditor::layout_changed_()
{
    if (on_layout_changed)
        on_layout_changed();
}

// ── layout ────────────────────────────────────────────────────────────────

float UserPackEditor::content_height() const
{
    const auto layout = layout_tile_row_(bounds_.w, images_.size() + 1, 0.0f);
    if (layout.empty())
        return 0.0f;
    const auto& last = layout.back();
    return last.y + kTileSize + kTilePad;
}

tk::Size UserPackEditor::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return {constraints.w, kViewportH};
}

void UserPackEditor::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    bounds_ = bounds;
    clamp_scroll();
}

bool UserPackEditor::on_wheel(tk::Point /*local*/, float /*dx*/, float dy, bool is_touchpad)
{
    return on_wheel_scroll(dy, is_touchpad);
}

bool UserPackEditor::on_pointer_down(tk::Point local)
{
    if (scrollbar_on_pointer_down(local))
        return true;

    const auto layout = layout_tile_row_(bounds_.w, images_.size() + 1, 0.0f);
    const float y_content = local.y + scroll_y_;

    for (std::size_t t = 0; t < layout.size(); ++t)
    {
        const auto& r = layout[t];
        if (local.x < r.x || local.x >= r.x + r.w || y_content < r.y ||
            y_content >= r.y + r.h)
            continue;
        if (t >= images_.size())
            break; // hint tile — not interactive

        if (!committing_)
        {
            if (hit_remove_chip_(local, y_content, r))
            {
                remove_tile_(t);
                return true;
            }

            if (y_content >= r.y + kImageH && y_content < r.y + kImageH + kLabelH)
            {
                begin_editing_shortcode_(t);
                return true;
            }
        }
        break; // clicked the thumbnail itself — no-op
    }
    return false;
}

void UserPackEditor::on_pointer_drag(tk::Point local)
{
    scrollbar_on_pointer_drag(local);
}

void UserPackEditor::on_pointer_up(tk::Point /*local*/, bool /*inside_self*/)
{
    scrollbar_on_pointer_up();
}

bool UserPackEditor::on_pointer_move(tk::Point local)
{
    const auto layout = layout_tile_row_(bounds_.w, images_.size() + 1, 0.0f);
    const float y_content = local.y + scroll_y_;

    std::optional<std::size_t> new_hovered;
    for (std::size_t t = 0; t < images_.size() && t < layout.size(); ++t)
    {
        const auto& r = layout[t];
        if (local.x >= r.x && local.x < r.x + r.w && y_content >= r.y &&
            y_content < r.y + kImageH)
        {
            new_hovered = t;
            break;
        }
    }
    const bool changed = new_hovered != hovered_tile_;
    hovered_tile_ = new_hovered;
    return changed;
}

void UserPackEditor::on_pointer_leave()
{
    hovered_tile_.reset();
}

void UserPackEditor::paint(tk::PaintCtx& ctx)
{
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);
    step_kinetic();

    const auto layout = layout_tile_row_(bounds_.w, images_.size() + 1, 0.0f);
    ctx.canvas.push_clip_rect(bounds_);
    const tk::Point origin{bounds_.x, bounds_.y - scroll_y_};

    for (std::size_t t = 0; t < layout.size(); ++t)
    {
        const auto& r = layout[t];
        if (origin.y + r.y + kTileSize <= bounds_.y || origin.y + r.y >= bounds_.y + bounds_.h)
            continue;
        if (t < images_.size())
        {
            const bool hovered = hovered_tile_ && *hovered_tile_ == t;
            const bool is_editing = editing_ && *editing_ == t;
            paint_tile_shared_(ctx, images_[t], r, origin, hovered, is_editing);
        }
        else
        {
            paint_hint_tile_shared_(ctx, r, origin);
        }
    }
    if (drag_hover_)
        tk::paint_drag_hover_highlight(ctx, bounds_);
    ctx.canvas.pop_clip();
    paint_scrollbar(ctx);
}

} // namespace tesseract::views
