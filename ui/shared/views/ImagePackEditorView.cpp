#include "ImagePackEditorView.h"

#include "icons.h"
#include "tk/i18n.h"
#include "tk/theme.h"

#include <algorithm>

namespace tesseract::views
{

namespace
{
constexpr tesseract::PackUsage kUsageSlots[3] = {
    tesseract::PackUsage::Any, tesseract::PackUsage::Emoticon,
    tesseract::PackUsage::Sticker};
} // namespace

// ─────────────────────────────────────────────────────────────────────────
//  ImagePackSectionList
// ─────────────────────────────────────────────────────────────────────────

ImagePackSectionList::ImagePackSectionList() = default;

void ImagePackSectionList::set_packs(const std::vector<StagedPack>* packs)
{
    packs_ = packs;
    refresh();
}

void ImagePackSectionList::set_image_provider(ImagePackImageProvider provider)
{
    image_provider_ = std::move(provider);
}

void ImagePackSectionList::set_active_pack_index(std::optional<std::size_t> idx)
{
    active_pack_index_ = idx;
}

void ImagePackSectionList::set_editing(
    std::optional<std::pair<std::size_t, std::size_t>> pack_and_tile)
{
    editing_ = pack_and_tile;
}

void ImagePackSectionList::set_editing_name(std::optional<std::size_t> pack_idx)
{
    editing_name_ = pack_idx;
}

void ImagePackSectionList::refresh()
{
    clamp_scroll();
}

std::vector<ImagePackSectionList::SectionLayout>
ImagePackSectionList::compute_layout_(float width) const
{
    std::vector<SectionLayout> out;
    if (!packs_)
        return out;

    const float grid_w = std::max(0.0f, width - 2.0f * kTilePad);
    const int cols = std::max(
        1, static_cast<int>((grid_w + kTileSpacing) / (kTileSize + kTileSpacing)));

    float y = 0.0f;
    out.reserve(packs_->size());
    for (const auto& pack : *packs_)
    {
        SectionLayout sec;
        sec.top = y;
        sec.header_rect = {0.0f, y, width, kHeaderH};

        float x = width - kHeaderPadX;
        const float chip_d = kHeaderRemoveR * 2.0f;
        x -= chip_d;
        sec.remove_chip_rect = {x, y + (kHeaderH - chip_d) * 0.5f, chip_d, chip_d};
        for (int seg = 2; seg >= 0; --seg)
        {
            x -= kUsageSegGap + kUsageSegW;
            sec.usage_rect[seg] = {x, y + (kHeaderH - kUsageSegH) * 0.5f, kUsageSegW,
                                   kUsageSegH};
        }
        const float name_left = kActiveBarW + kHeaderPadX;
        sec.name_rect = {name_left, y, std::max(0.0f, x - kUsageSegGap - name_left),
                         kHeaderH};

        y += kHeaderH;

        const std::size_t tile_count = pack.images.size() + 1; // + hint tile
        const int rows = static_cast<int>(
            (tile_count + static_cast<std::size_t>(cols) - 1) /
            static_cast<std::size_t>(cols));
        sec.tiles.reserve(tile_count);
        for (std::size_t t = 0; t < tile_count; ++t)
        {
            const int row = static_cast<int>(t / static_cast<std::size_t>(cols));
            const int col = static_cast<int>(t % static_cast<std::size_t>(cols));
            const float tx = kTilePad + col * (kTileSize + kTileSpacing);
            const float ty = y + kTilePad + row * (kTileSize + kTileSpacing);
            sec.tiles.push_back({{tx, ty, kTileSize, kTileSize}});
        }
        const float grid_h =
            kTilePad * 2.0f +
            (rows > 0 ? rows * kTileSize + (rows - 1) * kTileSpacing : 0.0f);
        y += grid_h + kSectionGap;

        sec.height = y - sec.top;
        out.push_back(std::move(sec));
    }
    return out;
}

float ImagePackSectionList::content_height() const
{
    const auto layout = compute_layout_(bounds_.w);
    if (layout.empty())
        return 0.0f;
    return layout.back().top + layout.back().height;
}

tk::Rect ImagePackSectionList::label_rect_at(std::size_t pack_idx,
                                             std::size_t tile_idx) const
{
    if (!packs_ || pack_idx >= packs_->size())
        return {};
    if (tile_idx >= (*packs_)[pack_idx].images.size())
        return {};
    const auto layout = compute_layout_(bounds_.w);
    if (pack_idx >= layout.size())
        return {};
    const auto& sec = layout[pack_idx];
    if (tile_idx >= sec.tiles.size())
        return {};
    const auto& r = sec.tiles[tile_idx].image_rect;
    const tk::Rect world{bounds_.x + r.x, bounds_.y - scroll_y_ + r.y + kImageH, r.w,
                         kLabelH};
    if (world.bottom() <= bounds_.y || world.y >= bounds_.y + bounds_.h)
        return {}; // scrolled out of the viewport
    return world;
}

tk::Rect ImagePackSectionList::name_rect_at(std::size_t pack_idx) const
{
    if (!packs_ || pack_idx >= packs_->size())
        return {};
    const auto layout = compute_layout_(bounds_.w);
    if (pack_idx >= layout.size())
        return {};
    const auto& sec = layout[pack_idx];
    const tk::Rect world{bounds_.x + sec.name_rect.x,
                         bounds_.y - scroll_y_ + sec.name_rect.y, sec.name_rect.w,
                         sec.name_rect.h};
    if (world.bottom() <= bounds_.y || world.y >= bounds_.y + bounds_.h)
        return {}; // scrolled out of the viewport
    return world;
}

std::optional<std::size_t> ImagePackSectionList::pack_at(tk::Point world) const
{
    if (!packs_)
        return std::nullopt;
    const tk::Point local{world.x - bounds_.x, world.y - bounds_.y};
    if (local.x < 0 || local.x >= bounds_.w || local.y < 0 || local.y >= bounds_.h)
        return std::nullopt;
    const auto layout = compute_layout_(bounds_.w);
    const float y_content = local.y + scroll_y_;
    for (std::size_t i = 0; i < layout.size(); ++i)
    {
        const auto& sec = layout[i];
        if (y_content >= sec.top && y_content < sec.top + sec.height)
            return i;
    }
    return std::nullopt;
}

tk::Size ImagePackSectionList::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void ImagePackSectionList::arrange(tk::LayoutCtx&, tk::Rect bounds)
{
    bounds_ = bounds;
    clamp_scroll();
}

bool ImagePackSectionList::on_wheel(tk::Point /*local*/, float /*dx*/, float dy)
{
    const float prev = scroll_y_;
    scroll_y_ += dy;
    clamp_scroll();
    return scroll_y_ != prev;
}

bool ImagePackSectionList::on_pointer_down(tk::Point local)
{
    if (scrollbar_on_pointer_down(local))
        return true;
    if (!packs_)
        return false;

    const auto layout = compute_layout_(bounds_.w);
    const float y_content = local.y + scroll_y_;

    for (std::size_t i = 0; i < layout.size(); ++i)
    {
        const auto& sec = layout[i];
        if (y_content < sec.top || y_content >= sec.top + sec.height)
            continue;

        {
            const auto& cr = sec.remove_chip_rect;
            const float ccx = cr.x + cr.w * 0.5f;
            const float ccy = cr.y + cr.h * 0.5f;
            const float dx = local.x - ccx;
            const float dy = y_content - ccy;
            if ((dx * dx + dy * dy) <= (kHeaderRemoveR * kHeaderRemoveR))
            {
                if (on_pack_remove_requested)
                    on_pack_remove_requested(i);
                return true;
            }
        }
        for (int seg = 0; seg < 3; ++seg)
        {
            const auto& sr = sec.usage_rect[seg];
            if (local.x >= sr.x && local.x < sr.x + sr.w && y_content >= sr.y &&
                y_content < sr.y + sr.h)
            {
                if (on_pack_usage_changed)
                    on_pack_usage_changed(i, kUsageSlots[seg]);
                return true;
            }
        }
        {
            const auto& nr = sec.name_rect;
            if (local.x >= nr.x && local.x < nr.x + nr.w && y_content >= nr.y &&
                y_content < nr.y + nr.h)
            {
                if (on_pack_name_clicked)
                    on_pack_name_clicked(i);
                return true;
            }
        }
        if (y_content < sec.top + kHeaderH)
        {
            if (on_pack_header_clicked)
                on_pack_header_clicked(i);
            return true;
        }

        const auto& images = (*packs_)[i].images;
        for (std::size_t t = 0; t < sec.tiles.size(); ++t)
        {
            const auto& r = sec.tiles[t].image_rect;
            if (local.x < r.x || local.x >= r.x + r.w || y_content < r.y ||
                y_content >= r.y + r.h)
                continue;
            if (t >= images.size())
                break; // hint tile — not interactive

            const float ccx = r.x + r.w - kRemoveChipR;
            const float ccy = r.y + kRemoveChipR;
            const float dx = local.x - ccx;
            const float dy = y_content - ccy;
            const float tol = kRemoveChipR + 4.0f;
            if ((dx * dx + dy * dy) <= (tol * tol))
            {
                if (on_tile_remove_requested)
                    on_tile_remove_requested(i, t);
                return true;
            }
            if (y_content >= r.y + kImageH && y_content < r.y + kImageH + kLabelH)
            {
                if (on_tile_shortcode_clicked)
                    on_tile_shortcode_clicked(i, t);
                return true;
            }
            break; // clicked the thumbnail itself — no-op
        }
        break; // point was in this section but not on any interactive element
    }
    return false;
}

void ImagePackSectionList::on_pointer_drag(tk::Point local)
{
    scrollbar_on_pointer_drag(local);
}

void ImagePackSectionList::on_pointer_up(tk::Point /*local*/, bool /*inside_self*/)
{
    scrollbar_on_pointer_up();
}

bool ImagePackSectionList::on_pointer_move(tk::Point local)
{
    if (!packs_)
        return false;
    const auto layout = compute_layout_(bounds_.w);
    const float y_content = local.y + scroll_y_;

    std::optional<std::pair<std::size_t, std::size_t>> new_hovered_tile;
    std::optional<std::size_t> new_hovered_header_remove;

    for (std::size_t i = 0; i < layout.size(); ++i)
    {
        const auto& sec = layout[i];
        if (y_content < sec.top || y_content >= sec.top + sec.height)
            continue;

        const auto& cr = sec.remove_chip_rect;
        const float ccx = cr.x + cr.w * 0.5f;
        const float ccy = cr.y + cr.h * 0.5f;
        const float dx = local.x - ccx;
        const float dy = y_content - ccy;
        if ((dx * dx + dy * dy) <= (kHeaderRemoveR * kHeaderRemoveR))
            new_hovered_header_remove = i;

        const auto& images = (*packs_)[i].images;
        for (std::size_t t = 0; t < images.size(); ++t)
        {
            const auto& r = sec.tiles[t].image_rect;
            if (local.x >= r.x && local.x < r.x + r.w && y_content >= r.y &&
                y_content < r.y + kImageH)
            {
                new_hovered_tile = {i, t};
                break;
            }
        }
        break;
    }

    const bool changed = (new_hovered_tile != hovered_tile_) ||
                         (new_hovered_header_remove != hovered_header_remove_);
    hovered_tile_ = new_hovered_tile;
    hovered_header_remove_ = new_hovered_header_remove;
    return changed;
}

void ImagePackSectionList::on_pointer_leave()
{
    hovered_tile_.reset();
    hovered_header_remove_.reset();
}

void ImagePackSectionList::paint_header_(tk::PaintCtx& ctx, std::size_t pack_idx,
                                         const SectionLayout& sec, tk::Point origin,
                                         bool active, bool hovered_remove) const
{
    const auto& pal = ctx.theme.palette;
    const tk::Rect hdr{origin.x + sec.header_rect.x, origin.y + sec.header_rect.y,
                       sec.header_rect.w, sec.header_rect.h};
    ctx.canvas.fill_rect(hdr, active ? pal.subtle_hover : pal.sidebar_bg);
    if (active)
        ctx.canvas.fill_rect({hdr.x, hdr.y, kActiveBarW, hdr.h}, pal.accent);

    const auto& pack = (*packs_)[pack_idx];
    const bool editing_name = editing_name_ && *editing_name_ == pack_idx;
    if (!editing_name)
    {
        tk::TextStyle st;
        st.role      = tk::FontRole::UiSemibold;
        st.halign    = tk::TextHAlign::Leading;
        st.valign    = tk::TextVAlign::Center;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = sec.name_rect.w;
        const std::string name =
            pack.display_name.empty() ? tk::tr("Unnamed pack") : pack.display_name;
        auto lay = ctx.factory.build_text(name, st);
        if (lay)
        {
            const tk::Rect nr{origin.x + sec.name_rect.x, origin.y + sec.name_rect.y,
                              sec.name_rect.w, sec.name_rect.h};
            const tk::Size sz = lay->measure();
            ctx.canvas.draw_text(*lay, {nr.x, nr.y + (nr.h - sz.h) * 0.5f},
                                 pal.text_primary);
        }
    }

    static const char* const kSegLabels[3] = {"Any", "Emoji", "Sticker"};
    for (int seg = 0; seg < 3; ++seg)
    {
        const tk::Rect sr{origin.x + sec.usage_rect[seg].x,
                          origin.y + sec.usage_rect[seg].y, sec.usage_rect[seg].w,
                          sec.usage_rect[seg].h};
        const bool selected = pack.usage == kUsageSlots[seg];
        ctx.canvas.fill_rounded_rect(sr, 4.0f, selected ? pal.accent : pal.subtle_hover);
        // No halign/valign here — this is centered manually below via the
        // measured size, same as the remove-chip glyphs. Qt's own halign
        // math (canvas_qpainter.cpp) treats an unset max_width as an 8192px
        // sentinel box, not "natural text width" — setting halign::Center
        // without a real max_width shoves the glyph thousands of pixels off
        // to the side instead of leaving it in place.
        tk::TextStyle segst;
        segst.role = tk::FontRole::Small;
        auto seglay = ctx.factory.build_text(tk::tr(kSegLabels[seg]), segst);
        if (seglay)
        {
            const tk::Size sz = seglay->measure();
            ctx.canvas.draw_text(
                *seglay, {sr.x + (sr.w - sz.w) * 0.5f, sr.y + (sr.h - sz.h) * 0.5f},
                selected ? pal.text_on_accent : pal.text_secondary);
        }
    }

    // Remove chip — always visible (a persistent header row, unlike a tile
    // in a dense grid, has no natural hover-to-discover expectation).
    {
        const tk::Rect cr{origin.x + sec.remove_chip_rect.x,
                          origin.y + sec.remove_chip_rect.y, sec.remove_chip_rect.w,
                          sec.remove_chip_rect.h};
        ctx.canvas.fill_rounded_rect(
            cr, kHeaderRemoveR,
            hovered_remove ? pal.destructive_hover : tk::Color::rgba(40, 40, 40, 220));
        header_remove_icon_.draw(ctx.canvas, ctx.factory, kCloseSvg, cr, 14.0f,
                                tk::Color::rgb(0xffffff));
    }

    ctx.canvas.fill_rect({hdr.x, hdr.bottom() - 1.0f, hdr.w, 1.0f}, pal.separator);
}

void ImagePackSectionList::paint_tile_(tk::PaintCtx& ctx, std::size_t pack_idx,
                                       std::size_t tile_idx, const tk::Rect& cell_local,
                                       tk::Point origin, bool hovered) const
{
    const tk::Rect cell{origin.x + cell_local.x, origin.y + cell_local.y, cell_local.w,
                        cell_local.h};
    const tk::Rect image_rect{cell.x, cell.y, cell.w, kImageH};
    const auto& img = (*packs_)[pack_idx].images[tile_idx];
    const auto& pal = ctx.theme.palette;

    ctx.canvas.fill_rounded_rect(image_rect, 6.0f, pal.subtle_hover);

    const tk::Image* bmp =
        img.local_preview ? img.local_preview.get()
                          : (image_provider_ && !img.existing_url.empty()
                                 ? image_provider_(img.existing_url)
                                 : nullptr);
    if (bmp)
    {
        const float iw = static_cast<float>(bmp->width());
        const float ih = static_cast<float>(bmp->height());
        if (iw > 0 && ih > 0)
        {
            const float s  = std::min(image_rect.w / iw, image_rect.h / ih);
            const float dw = iw * s;
            const float dh = ih * s;
            const tk::Rect dst{image_rect.x + (image_rect.w - dw) * 0.5f,
                              image_rect.y + (image_rect.h - dh) * 0.5f, dw, dh};
            ctx.canvas.draw_image(*bmp, dst);
        }
    }

    const bool is_editing =
        editing_ && editing_->first == pack_idx && editing_->second == tile_idx;
    if (!is_editing)
    {
        // halign left at default (Leading) — centered manually below via the
        // measured size; see the comment on the usage-segment labels above
        // for why halign::Center would double up with that manual offset.
        tk::TextStyle st;
        st.role      = tk::FontRole::Small;
        st.trim      = tk::TextTrim::Ellipsis;
        st.max_width = cell.w - 4.0f;
        const std::string text =
            img.shortcode.empty() ? tk::tr("(no shortcode)") : img.shortcode;
        auto lay = ctx.factory.build_text(text, st);
        if (lay)
        {
            const tk::Size sz = lay->measure();
            ctx.canvas.draw_text(
                *lay,
                {cell.x + (cell.w - sz.w) * 0.5f,
                 cell.y + kImageH + (kLabelH - sz.h) * 0.5f},
                pal.text_muted);
        }
    }

    if (hovered)
    {
        const float cx = image_rect.x + image_rect.w - kRemoveChipR;
        const float cy = image_rect.y + kRemoveChipR;
        const tk::Rect chip{cx - kRemoveChipR, cy - kRemoveChipR, kRemoveChipR * 2.0f,
                            kRemoveChipR * 2.0f};
        ctx.canvas.fill_rounded_rect(chip, kRemoveChipR,
                                    tk::Color::rgba(40, 40, 40, 220));
        tile_remove_icon_.draw(ctx.canvas, ctx.factory, kCloseSvg, chip, 12.0f,
                              tk::Color::rgb(0xffffff));
    }
}

void ImagePackSectionList::paint_hint_tile_(tk::PaintCtx& ctx,
                                            const tk::Rect& cell_local,
                                            tk::Point origin) const
{
    const tk::Rect cell{origin.x + cell_local.x, origin.y + cell_local.y, cell_local.w,
                        cell_local.h};
    const tk::Rect image_rect{cell.x, cell.y, cell.w, kImageH};
    const auto& pal = ctx.theme.palette;
    ctx.canvas.stroke_rounded_rect(image_rect, 6.0f, pal.border);

    // halign/valign left at default — centered manually below, same
    // reasoning as the usage-segment labels' comment above.
    tk::TextStyle st;
    st.role      = tk::FontRole::Small;
    st.wrap      = true;
    st.max_width = image_rect.w - 8.0f;
    auto lay = ctx.factory.build_text(tk::tr("Drop image or paste"), st);
    if (lay)
    {
        const tk::Size sz = lay->measure();
        ctx.canvas.draw_text(*lay,
                             {image_rect.x + (image_rect.w - sz.w) * 0.5f,
                              image_rect.y + (image_rect.h - sz.h) * 0.5f},
                             pal.text_muted);
    }
}

void ImagePackSectionList::paint(tk::PaintCtx& ctx)
{
    ctx.canvas.fill_rect(bounds_, ctx.theme.palette.bg);
    if (!packs_ || packs_->empty())
        return;

    const auto layout = compute_layout_(bounds_.w);
    ctx.canvas.push_clip_rect(bounds_);
    const tk::Point origin{bounds_.x, bounds_.y - scroll_y_};

    for (std::size_t i = 0; i < layout.size(); ++i)
    {
        const auto& sec = layout[i];
        if (origin.y + sec.top + sec.height <= bounds_.y ||
            origin.y + sec.top >= bounds_.y + bounds_.h)
            continue;

        const bool active = active_pack_index_ && *active_pack_index_ == i;
        const bool hdr_remove_hovered =
            hovered_header_remove_ && *hovered_header_remove_ == i;
        paint_header_(ctx, i, sec, origin, active, hdr_remove_hovered);

        const auto& images = (*packs_)[i].images;
        for (std::size_t t = 0; t < sec.tiles.size(); ++t)
        {
            if (t < images.size())
            {
                const bool hovered =
                    hovered_tile_ && hovered_tile_->first == i && hovered_tile_->second == t;
                paint_tile_(ctx, i, t, sec.tiles[t].image_rect, origin, hovered);
            }
            else
            {
                paint_hint_tile_(ctx, sec.tiles[t].image_rect, origin);
            }
        }
    }
    ctx.canvas.pop_clip();
    paint_scrollbar(ctx);
}

// ─────────────────────────────────────────────────────────────────────────
//  ImagePackEditorView
// ─────────────────────────────────────────────────────────────────────────

ImagePackEditorView::ImagePackEditorView()
{
    auto list = std::make_unique<ImagePackSectionList>();
    list_     = add_child(std::move(list));
    list_->set_packs(&packs_);
    list_->on_pack_header_clicked = [this](std::size_t idx) { select_active_pack_(idx); };
    list_->on_pack_name_clicked = [this](std::size_t idx) { begin_editing_pack_name_(idx); };
    list_->on_pack_usage_changed =
        [this](std::size_t idx, tesseract::PackUsage u)
    {
        if (idx < packs_.size())
            packs_[idx].usage = u;
    };
    list_->on_pack_remove_requested = [this](std::size_t idx) { remove_pack_(idx); };
    list_->on_tile_remove_requested =
        [this](std::size_t p, std::size_t t) { remove_tile_(p, t); };
    list_->on_tile_shortcode_clicked =
        [this](std::size_t p, std::size_t t) { begin_editing_shortcode_(p, t); };

    create_btn_ = add_child(
        std::make_unique<tk::Button>(tk::tr("Create"), std::function<void()>{},
                                     tk::Button::Variant::Subtle));
    create_btn_->set_on_click(
        [this]()
        {
            if (!committing_)
                create_pack_();
        });

    accept_btn_ = add_child(
        std::make_unique<tk::Button>(tk::tr("Accept"), std::function<void()>{},
                                     tk::Button::Variant::Primary));
    cancel_btn_ = add_child(
        std::make_unique<tk::Button>(tk::tr("Cancel"), std::function<void()>{},
                                     tk::Button::Variant::Subtle));

    accept_btn_->set_on_click(
        [this]()
        {
            if (committing_)
                return;
            committing_ = true;
            create_btn_->set_enabled(false);
            cancel_btn_->set_enabled(false);
            refresh_accept_enabled_();
            if (on_accept)
            {
                ImagePackEditorResult result;
                result.room_id         = room_id_;
                result.packs           = packs_;
                result.removed_pack_ids = removed_pack_ids_;
                on_accept(std::move(result));
            }
        });
    cancel_btn_->set_on_click(
        [this]()
        {
            if (committing_)
                return;
            if (on_cancel)
                on_cancel();
        });

    // Closed-by-default; same idiom as RoomSettingsView/RoomInfoPanel.
    set_visible(false);
}

ImagePackEditorView::~ImagePackEditorView() = default;

void ImagePackEditorView::open(std::string room_id)
{
    room_id_    = std::move(room_id);
    open_       = true;
    committing_ = false;
    packs_.clear();
    removed_pack_ids_.clear();
    active_pack_index_.reset();
    next_local_id_ = 0;
    new_pack_name_draft_.clear();
    editing_.reset();
    editing_pack_name_.reset();

    list_->set_active_pack_index(std::nullopt);
    list_->set_editing(std::nullopt);
    list_->set_editing_name(std::nullopt);
    list_->refresh();

    create_btn_->set_enabled(true);
    accept_btn_->set_enabled(true);
    cancel_btn_->set_enabled(true);

    set_visible(true);
    layout_changed_();
}

void ImagePackEditorView::close()
{
    if (!open_)
        return;
    open_ = false;
    editing_.reset();
    editing_pack_name_.reset();
    list_->set_editing(std::nullopt);
    list_->set_editing_name(std::nullopt);
    set_visible(false);
    layout_changed_();
}

void ImagePackEditorView::set_available_packs(
    std::vector<tesseract::ImagePack> packs)
{
    if (!open_)
        return;
    packs_.clear();
    packs_.reserve(packs.size());
    for (auto& p : packs)
    {
        StagedPack sp;
        sp.is_new       = false;
        sp.pack_id      = p.id;
        sp.state_key    = p.source_state_key;
        sp.display_name = p.display_name;
        sp.usage        = p.usage;
        packs_.push_back(std::move(sp));
    }
    removed_pack_ids_.clear();
    active_pack_index_ = packs_.empty() ? std::nullopt
                                       : std::optional<std::size_t>(0);
    editing_.reset();
    list_->set_active_pack_index(active_pack_index_);
    list_->set_editing(std::nullopt);
    list_->refresh();
    refresh_accept_enabled_();
    layout_changed_();

    if (on_pack_images_needed)
    {
        for (const auto& p : packs_)
            on_pack_images_needed(p.pack_id);
    }
}

void ImagePackEditorView::set_pack_images(
    std::string pack_id, std::vector<tesseract::ImagePackImage> images)
{
    for (auto& p : packs_)
    {
        if (p.pack_id != pack_id)
            continue;
        p.images.clear();
        p.images.reserve(images.size());
        for (auto& img : images)
        {
            StagedPackImage s;
            s.shortcode    = std::move(img.shortcode);
            s.existing_url = std::move(img.url);
            s.body         = std::move(img.body);
            s.info_json    = std::move(img.info_json);
            s.usage        = img.usage;
            s.favorite     = img.favorite;
            p.images.push_back(std::move(s));
        }
        list_->refresh();
        layout_changed_();
        return;
    }
}

void ImagePackEditorView::set_image_provider(ImagePackImageProvider p)
{
    list_->set_image_provider(std::move(p));
}

tk::Rect ImagePackEditorView::new_pack_name_field_rect() const
{
    if (!open_)
        return {};
    return new_pack_name_field_rect_;
}

void ImagePackEditorView::set_new_pack_name_text(std::string text)
{
    new_pack_name_draft_ = std::move(text);
}

tk::Rect ImagePackEditorView::shortcode_edit_rect() const
{
    if (!open_ || !editing_)
        return {};
    return list_->label_rect_at(editing_->first, editing_->second);
}

void ImagePackEditorView::set_editing_shortcode_text(std::string text)
{
    if (!editing_)
        return;
    const auto [p, t] = *editing_;
    if (p >= packs_.size() || t >= packs_[p].images.size())
        return;
    packs_[p].images[t].shortcode = std::move(text);
}

void ImagePackEditorView::commit_editing_shortcode()
{
    if (!editing_)
        return;
    editing_.reset();
    list_->set_editing(std::nullopt);
    layout_changed_();
}

void ImagePackEditorView::begin_editing_shortcode_(std::size_t pack_idx,
                                                   std::size_t tile_idx)
{
    if (pack_idx >= packs_.size() || tile_idx >= packs_[pack_idx].images.size())
        return;
    editing_ = {pack_idx, tile_idx};
    list_->set_editing(editing_);
    layout_changed_();
}

tk::Rect ImagePackEditorView::pack_name_edit_rect() const
{
    if (!open_ || !editing_pack_name_)
        return {};
    return list_->name_rect_at(*editing_pack_name_);
}

std::string ImagePackEditorView::pack_name_edit_initial_text() const
{
    if (!editing_pack_name_ || *editing_pack_name_ >= packs_.size())
        return {};
    return packs_[*editing_pack_name_].display_name;
}

void ImagePackEditorView::set_editing_pack_name_text(std::string text)
{
    if (!editing_pack_name_ || *editing_pack_name_ >= packs_.size())
        return;
    packs_[*editing_pack_name_].display_name = std::move(text);
}

void ImagePackEditorView::commit_editing_pack_name()
{
    if (!editing_pack_name_)
        return;
    editing_pack_name_.reset();
    list_->set_editing_name(std::nullopt);
    layout_changed_();
}

void ImagePackEditorView::begin_editing_pack_name_(std::size_t pack_idx)
{
    if (pack_idx >= packs_.size())
        return;
    select_active_pack_(pack_idx);
    editing_pack_name_ = pack_idx;
    list_->set_editing_name(editing_pack_name_);
    layout_changed_();
}

void ImagePackEditorView::remove_tile_(std::size_t pack_idx, std::size_t tile_idx)
{
    if (pack_idx >= packs_.size() || tile_idx >= packs_[pack_idx].images.size())
        return;
    auto& images = packs_[pack_idx].images;
    images.erase(images.begin() + static_cast<std::ptrdiff_t>(tile_idx));

    if (editing_ && editing_->first == pack_idx)
    {
        if (editing_->second == tile_idx)
        {
            editing_.reset();
            list_->set_editing(std::nullopt);
        }
        else if (editing_->second > tile_idx)
        {
            editing_->second -= 1;
            list_->set_editing(editing_);
        }
    }
    list_->refresh();
    layout_changed_();
}

void ImagePackEditorView::select_active_pack_(std::size_t idx)
{
    if (idx >= packs_.size())
        return;
    active_pack_index_ = idx;
    list_->set_active_pack_index(active_pack_index_);
    layout_changed_();
}

void ImagePackEditorView::remove_pack_(std::size_t idx)
{
    if (idx >= packs_.size())
        return;
    if (!packs_[idx].is_new)
        removed_pack_ids_.push_back(packs_[idx].pack_id);
    packs_.erase(packs_.begin() + static_cast<std::ptrdiff_t>(idx));

    if (active_pack_index_)
    {
        if (*active_pack_index_ == idx)
            active_pack_index_.reset();
        else if (*active_pack_index_ > idx)
            *active_pack_index_ -= 1;
    }
    if (editing_)
    {
        if (editing_->first == idx)
            editing_.reset();
        else if (editing_->first > idx)
            editing_->first -= 1;
    }
    if (editing_pack_name_)
    {
        if (*editing_pack_name_ == idx)
            editing_pack_name_.reset();
        else if (*editing_pack_name_ > idx)
            *editing_pack_name_ -= 1;
    }
    list_->set_active_pack_index(active_pack_index_);
    list_->set_editing(editing_);
    list_->set_editing_name(editing_pack_name_);
    list_->refresh();
    refresh_accept_enabled_();
    layout_changed_();
}

void ImagePackEditorView::create_pack_()
{
    StagedPack p;
    p.is_new        = true;
    p.display_name  = new_pack_name_draft_.empty() ? tk::tr("Unnamed pack")
                                                   : new_pack_name_draft_;
    p.usage         = tesseract::PackUsage::Any;
    packs_.push_back(std::move(p));
    active_pack_index_ = packs_.size() - 1;
    new_pack_name_draft_.clear();
    ++new_pack_name_reset_gen_;

    list_->set_active_pack_index(active_pack_index_);
    list_->refresh();
    refresh_accept_enabled_();
    layout_changed_();
}

void ImagePackEditorView::add_pending_image_to_pack_(
    std::size_t pack_idx, std::vector<std::uint8_t> bytes, std::string mime)
{
    if (pack_idx >= packs_.size())
        return;
    StagedPackImage img;
    img.local_id      = ++next_local_id_;
    img.pending_bytes = std::move(bytes);
    img.pending_mime  = std::move(mime);
    packs_[pack_idx].images.push_back(std::move(img));
    const std::size_t new_tile_idx = packs_[pack_idx].images.size() - 1;
    list_->refresh();

    if (on_pending_image_added)
    {
        const auto& staged_img = packs_[pack_idx].images[new_tile_idx];
        on_pending_image_added(staged_img.local_id, staged_img.pending_bytes,
                               staged_img.pending_mime);
    }
    begin_editing_shortcode_(pack_idx, new_tile_idx);
}

void ImagePackEditorView::add_pending_image_to_active(
    std::vector<std::uint8_t> bytes, std::string mime)
{
    if (!open_ || !active_pack_index_)
        return;
    add_pending_image_to_pack_(*active_pack_index_, std::move(bytes), std::move(mime));
}

void ImagePackEditorView::add_pending_image_at(tk::Point world,
                                               std::vector<std::uint8_t> bytes,
                                               std::string mime)
{
    if (!open_)
        return;
    auto idx = list_->pack_at(world);
    if (!idx)
        idx = active_pack_index_;
    if (!idx)
        return;
    add_pending_image_to_pack_(*idx, std::move(bytes), std::move(mime));
}

void ImagePackEditorView::set_tile_preview(std::uint64_t local_id,
                                           std::shared_ptr<tk::Image> image)
{
    for (auto& pack : packs_)
    {
        for (auto& img : pack.images)
        {
            if (img.local_id == local_id)
            {
                img.local_preview = std::move(image);
                list_->refresh();
                return;
            }
        }
    }
}

tk::Rect ImagePackEditorView::list_rect() const
{
    if (!open_ || !list_)
        return {};
    return list_->bounds();
}

void ImagePackEditorView::refresh_accept_enabled_()
{
    accept_btn_->set_enabled(!committing_);
}

void ImagePackEditorView::layout_changed_()
{
    if (on_layout_changed)
        on_layout_changed();
}

// ── layout ────────────────────────────────────────────────────────────────

tk::Size ImagePackEditorView::measure(tk::LayoutCtx&, tk::Size constraints)
{
    return constraints;
}

void ImagePackEditorView::arrange(tk::LayoutCtx& lc, tk::Rect bounds)
{
    tk::Widget::arrange(lc, bounds);

    float y = bounds.y + kPadY;
    y += kLabelH + kLabelGap;

    const float create_w = 88.0f;
    const float field_w =
        std::max(0.0f, bounds.w - 2.0f * kPadX - create_w - kBtnGap);
    new_pack_name_field_rect_ = {bounds.x + kPadX, y, field_w, kRowH};
    if (create_btn_)
        create_btn_->arrange(
            lc, {bounds.x + kPadX + field_w + kBtnGap, y, create_w, kRowH});
    y += kRowH + kPadY;

    const float footer_y = bounds.y + bounds.h - kFooterH;
    const float list_h   = std::max(0.0f, footer_y - y);
    if (list_)
        list_->arrange(lc, {bounds.x, y, bounds.w, list_h});

    const float btn_w_min = 88.0f;
    tk::Size accept_sz = accept_btn_ ? accept_btn_->measure(lc, {-1.0f, kBtnH})
                                    : tk::Size{btn_w_min, kBtnH};
    tk::Size cancel_sz = cancel_btn_ ? cancel_btn_->measure(lc, {-1.0f, kBtnH})
                                    : tk::Size{btn_w_min, kBtnH};
    const float accept_w = std::max(accept_sz.w, btn_w_min);
    const float cancel_w = std::max(cancel_sz.w, btn_w_min);
    const float btns_y   = footer_y + (kFooterH - kBtnH) * 0.5f;
    const float accept_x = bounds.x + bounds.w - kPadX - accept_w;
    const float cancel_x = accept_x - kBtnGap - cancel_w;

    if (cancel_btn_)
        cancel_btn_->arrange(lc, {cancel_x, btns_y, cancel_w, kBtnH});
    if (accept_btn_)
        accept_btn_->arrange(lc, {accept_x, btns_y, accept_w, kBtnH});
}

void ImagePackEditorView::paint(tk::PaintCtx& ctx)
{
    if (!open_)
        return;

    auto& cv        = ctx.canvas;
    const auto& pal = ctx.theme.palette;
    cv.fill_rect(bounds_, pal.bg);

    if (!new_pack_label_layout_)
    {
        tk::TextStyle st;
        st.role   = tk::FontRole::Caption;
        st.halign = tk::TextHAlign::Leading;
        new_pack_label_layout_ = ctx.factory.build_text(tk::tr("New pack name"), st);
    }
    if (new_pack_label_layout_)
        cv.draw_text(*new_pack_label_layout_, {bounds_.x + kPadX, bounds_.y + kPadY},
                    pal.text_secondary);

    cv.stroke_rounded_rect(new_pack_name_field_rect_, 4.0f, pal.border);

    if (create_btn_ && create_btn_->visible())
        create_btn_->paint(ctx);

    if (list_ && list_->visible())
    {
        if (packs_.empty())
        {
            tk::TextStyle st;
            st.role   = tk::FontRole::Small;
            st.halign = tk::TextHAlign::Leading;
            auto lay = ctx.factory.build_text(
                tk::tr("No image packs yet \xe2\x80\x94 create one above."), st);
            if (lay)
                cv.draw_text(*lay,
                            {list_->bounds().x + kPadX, list_->bounds().y + kPadY},
                            pal.text_muted);
        }
        else
        {
            list_->paint(ctx);
        }
    }

    const float footer_y = bounds_.y + bounds_.h - kFooterH;
    cv.fill_rect({bounds_.x, footer_y, bounds_.w, 1.0f}, pal.separator);
    cv.fill_rect({bounds_.x, footer_y + 1.0f, bounds_.w, kFooterH - 1.0f},
                pal.sidebar_bg);

    if (cancel_btn_ && cancel_btn_->visible())
        cancel_btn_->paint(ctx);
    if (accept_btn_ && accept_btn_->visible())
        accept_btn_->paint(ctx);
}

} // namespace tesseract::views
