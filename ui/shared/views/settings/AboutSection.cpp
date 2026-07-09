#include "AboutSection.h"

#include "SettingsGroup.h"

#include "views/BrandView.h"
#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/layout.h"
#include "tk/theme.h"
#include "tk/widget.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>

namespace tesseract::views
{

// Named (not anonymous) so the helper cell types have external linkage,
// matching AboutSection::CacheSizeRow which stores a CacheValueCell* member
// (avoids -Wsubobject-linkage). Re-exposed via the using-directive below so
// call sites stay unqualified.
namespace about_detail
{

constexpr float kAboutSectionRowH     = 22.0f;
constexpr float kAboutSectionLabelW   = 160.0f;
constexpr float kAboutSectionLabelGap =   8.0f;
// Natural width of a CacheSizeRow: label + gap + value column.
// Caps the Storage group so it doesn't stretch to fill the page.
constexpr float kNaturalW = kAboutSectionLabelW + kAboutSectionLabelGap + 140.0f;

std::string format_bytes(uint64_t n)
{
    if (n < 1024ULL)
        return std::to_string(n) + " B";
    if (n < 1024ULL * 1024)
        return std::to_string(n / 1024) + " KB";
    if (n < 1024ULL * 1024 * 1024)
        return std::to_string(n / (1024 * 1024)) + " MB";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f GB",
                  static_cast<double>(n) / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

std::string format_hit_miss(uint64_t hits, uint64_t misses)
{
    const uint64_t total = hits + misses;
    char buf[128];
    if (total == 0)
    {
        std::snprintf(buf, sizeof(buf), "Hits: 0  ·  Misses: 0");
    }
    else
    {
        const double rate = 100.0 * static_cast<double>(hits) /
                            static_cast<double>(total);
        std::snprintf(buf, sizeof(buf),
                      "Hits: %llu  ·  Misses: %llu  (%.1f%% hit rate)",
                      static_cast<unsigned long long>(hits),
                      static_cast<unsigned long long>(misses),
                      rate);
    }
    return buf;
}

// ---------------------------------------------------------------------------
// CacheNameCell — fixed-width left cell (kAboutSectionLabelW), text_secondary
// ---------------------------------------------------------------------------

class CacheNameCell : public tk::Widget
{
public:
    explicit CacheNameCell(std::string label) : label_text_(std::move(label)) {}

    std::function<void()> on_hover_enter;
    std::function<void()> on_hover_leave;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return {std::min(constraints.w, kAboutSectionLabelW), kAboutSectionRowH};
    }

    void arrange(tk::LayoutCtx&, tk::Rect bounds) override
    {
        if (bounds.w != bounds_.w)
            label_layout_.reset();
        bounds_ = bounds;
    }

    void paint(tk::PaintCtx& ctx) override
    {
        if (!label_layout_)
        {
            tk::TextStyle ls;
            ls.role      = tk::FontRole::Body;
            ls.halign    = tk::TextHAlign::Leading;
            ls.valign    = tk::TextVAlign::Top;
            ls.trim      = tk::TextTrim::Ellipsis;
            ls.max_width = bounds_.w;
            label_layout_ = ctx.factory.build_text(label_text_, ls);
        }
        const float cy =
            bounds_.y + (kAboutSectionRowH - label_layout_->measure().h) * 0.5f;
        ctx.canvas.draw_text(*label_layout_, {bounds_.x, cy},
                             ctx.theme.palette.text_secondary);
    }

    bool on_pointer_move(tk::Point) override
    {
        if (cell_hovered_)
            return false;
        cell_hovered_ = true;
        if (on_hover_enter)
            on_hover_enter();
        return true;
    }

    void on_pointer_leave() override
    {
        if (!cell_hovered_)
            return;
        cell_hovered_ = false;
        if (on_hover_leave)
            on_hover_leave();
    }

private:
    std::string label_text_;
    std::unique_ptr<tk::TextLayout> label_layout_;
    bool cell_hovered_ = false;
};

// ---------------------------------------------------------------------------
// CacheValueCell — fill-main right cell, trailing-aligned, text_muted
// ---------------------------------------------------------------------------

class CacheValueCell : public tk::Widget
{
public:
    CacheValueCell() = default;

    void set_text(std::string t)
    {
        value_text_ = std::move(t);
        value_layout_.reset();
    }

    std::function<void()> on_hover_enter;
    std::function<void()> on_hover_leave;

    tk::Size measure(tk::LayoutCtx&, tk::Size) override
    {
        // Zero natural width; fill_main=true gives this cell the leftover space.
        return {0.0f, kAboutSectionRowH};
    }

    void arrange(tk::LayoutCtx&, tk::Rect bounds) override
    {
        if (bounds.w != bounds_.w)
            value_layout_.reset();
        bounds_ = bounds;
    }

    void paint(tk::PaintCtx& ctx) override
    {
        if (!value_layout_)
        {
            tk::TextStyle vs;
            vs.role      = tk::FontRole::Body;
            vs.halign    = tk::TextHAlign::Trailing;
            vs.valign    = tk::TextVAlign::Top;
            vs.trim      = tk::TextTrim::Ellipsis;
            vs.max_width = bounds_.w;
            value_layout_ = ctx.factory.build_text(value_text_, vs);
        }
        const auto sz  = value_layout_->measure();
        const float cy = bounds_.y + (kAboutSectionRowH - sz.h) * 0.5f;
        ctx.canvas.draw_text(*value_layout_, {bounds_.x, cy},
                             ctx.theme.palette.text_muted);
    }

    bool on_pointer_move(tk::Point) override
    {
        if (cell_hovered_)
            return false;
        cell_hovered_ = true;
        if (on_hover_enter)
            on_hover_enter();
        return true;
    }

    void on_pointer_leave() override
    {
        if (!cell_hovered_)
            return;
        cell_hovered_ = false;
        if (on_hover_leave)
            on_hover_leave();
    }

private:
    std::string value_text_{"—"};
    std::unique_ptr<tk::TextLayout> value_layout_;
    bool cell_hovered_ = false;
};

} // namespace about_detail

using namespace about_detail;

// ---------------------------------------------------------------------------
// CacheSizeRow — HBox[ CacheNameCell | CacheValueCell(fill_main) ]
// ---------------------------------------------------------------------------

class AboutSection::CacheSizeRow : public tk::HBox
{
public:
    explicit CacheSizeRow(std::string label)
    {
        set_spacing(kAboutSectionLabelGap);

        auto name = std::make_unique<CacheNameCell>(std::move(label));
        auto val  = std::make_unique<CacheValueCell>();
        val->set_layout_hints({.fill_main = true});

        name->on_hover_enter = [this] { enter_hover_(); };
        name->on_hover_leave = [this] { leave_hover_(); };
        val->on_hover_enter  = [this] { enter_hover_(); };
        val->on_hover_leave  = [this] { leave_hover_(); };

        add_child(std::move(name));
        value_cell_ = add_child(std::move(val));
    }

    void set_value(std::string v)
    {
        value_cell_->set_text(std::move(v));
    }

    void set_stats(uint64_t hits, uint64_t misses)
    {
        hits_      = hits;
        misses_    = misses;
        has_stats_ = true;
        // Stats may arrive after the initial hover-enter (async load).
        // Fire the tooltip now so the user doesn't have to re-hover.
        if (hover_count_ > 0 && on_show_tooltip)
            on_show_tooltip(format_hit_miss(hits_, misses_), bounds_);
    }

    std::function<void(std::string, tk::Rect)> on_show_tooltip;
    std::function<void()>                      on_hide_tooltip;

    // Override measure so the SettingsGroup reports kNaturalW as its natural
    // cross-axis width. Without this, the HBox would measure to only
    // kAboutSectionLabelW + kAboutSectionLabelGap (the value cell has zero natural width since
    // fill_main children are excluded from FlexBox::measure's used_main),
    // making the value column 0 px wide after arrange.
    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return {std::min(constraints.w, kNaturalW), kAboutSectionRowH};
    }

private:
    CacheValueCell* value_cell_ = nullptr;
    uint64_t hits_      = 0;
    uint64_t misses_    = 0;
    bool     has_stats_ = false;
    // Counter rather than bool: when the pointer crosses from CacheNameCell
    // to CacheValueCell, dispatch_pointer_move fires the new cell's
    // on_pointer_move (enter) before the host calls on_pointer_leave on the
    // old cell (leave). The counter absorbs that overlap so the tooltip
    // neither flickers nor gets orphaned.
    int hover_count_ = 0;

    void enter_hover_()
    {
        if (hover_count_++ == 0 && has_stats_ && on_show_tooltip)
            on_show_tooltip(format_hit_miss(hits_, misses_), bounds_);
    }

    void leave_hover_()
    {
        if (hover_count_ > 0 && --hover_count_ == 0)
        {
            if (on_hide_tooltip)
                on_hide_tooltip();
        }
    }
};

// ---------------------------------------------------------------------------
// AboutSection
// ---------------------------------------------------------------------------

AboutSection::AboutSection()
{
    // BrandView takes all remaining vertical space above the Storage group.
    auto bv = std::make_unique<BrandView>();
    bv->set_layout_hints({.fill_main = true});
    add_widget(std::move(bv));

    // Wrap the Storage group in an HBox so it uses its natural width and
    // sits at the bottom-left rather than stretching to fill the page.
    auto sg = std::make_unique<SettingsGroup>("Storage");
    memory_row_ = sg->add_widget(std::make_unique<CacheSizeRow>("In-memory cache"));
    local_row_  = sg->add_widget(std::make_unique<CacheSizeRow>("Local cache"));
    sdk_row_    = sg->add_widget(std::make_unique<CacheSizeRow>("SDK store"));
    sg->add_widget(std::make_unique<tk::Button>(
        "Clear all caches",
        [this] { if (on_clear_caches) on_clear_caches(); },
        tk::Button::Variant::Destructive));

    // Wire row tooltip callbacks up to the section-level callbacks so callers
    // only need to hook one pair of functions.
    auto wire = [this](CacheSizeRow* row)
    {
        row->on_show_tooltip = [this](std::string t, tk::Rect a)
        {
            if (on_show_tooltip) on_show_tooltip(std::move(t), a);
        };
        row->on_hide_tooltip = [this]
        {
            if (on_hide_tooltip) on_hide_tooltip();
        };
    };
    wire(memory_row_);
    wire(local_row_);
    // sdk_row_ intentionally not wired — no hit/miss data available.

    auto hbox = std::make_unique<tk::HBox>();
    hbox->add_child(std::move(sg));
    add_widget(std::move(hbox));
}

void AboutSection::set_memory_cache_size(uint64_t bytes)
{
    memory_row_->set_value(format_bytes(bytes));
}

void AboutSection::set_local_cache_size(uint64_t bytes)
{
    local_row_->set_value(format_bytes(bytes));
}

void AboutSection::set_sdk_store_size(uint64_t bytes)
{
    sdk_row_->set_value(format_bytes(bytes));
}

void AboutSection::set_memory_cache_stats(uint64_t hits, uint64_t misses)
{
    memory_row_->set_stats(hits, misses);
}

void AboutSection::set_local_cache_stats(uint64_t hits, uint64_t misses)
{
    local_row_->set_stats(hits, misses);
}

} // namespace tesseract::views
