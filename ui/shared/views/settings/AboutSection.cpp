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

namespace
{

constexpr float kRowH     = 22.0f;
constexpr float kLabelW   = 160.0f;
constexpr float kLabelGap =   8.0f;
// Natural width of a CacheSizeRow: label + gap + value column.
// Caps the Storage group so it doesn't stretch to fill the page.
constexpr float kNaturalW = kLabelW + kLabelGap + 140.0f;

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

} // namespace

// ---------------------------------------------------------------------------
// CacheSizeRow — read-only "Label / value" line with theme-resolved colours.
// Same pattern as ServerSection::HomeserverRow.
// ---------------------------------------------------------------------------

namespace
{

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

} // namespace

class AboutSection::CacheSizeRow : public tk::Widget
{
public:
    explicit CacheSizeRow(std::string label)
        : label_text_(std::move(label))
    {
    }

    void set_value(std::string value)
    {
        value_text_ = std::move(value);
        value_layout_.reset();
    }

    void set_stats(uint64_t hits, uint64_t misses)
    {
        hits_      = hits;
        misses_    = misses;
        has_stats_ = true;
    }

    std::function<void(std::string, tk::Rect)> on_show_tooltip;
    std::function<void()>                      on_hide_tooltip;

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override
    {
        return {std::min(constraints.w, kNaturalW), kRowH};
    }

    void arrange(tk::LayoutCtx&, tk::Rect bounds) override
    {
        if (bounds.w != bounds_.w)
        {
            label_layout_.reset();
            value_layout_.reset();
        }
        bounds_ = bounds;
    }

    void paint(tk::PaintCtx& ctx) override
    {
        const auto& pal = ctx.theme.palette;

        if (!label_layout_)
        {
            tk::TextStyle ls;
            ls.role      = tk::FontRole::Body;
            ls.halign    = tk::TextHAlign::Leading;
            ls.valign    = tk::TextVAlign::Top;
            ls.trim      = tk::TextTrim::Ellipsis;
            ls.max_width = kLabelW;
            label_layout_ = ctx.factory.build_text(label_text_, ls);
        }
        if (!value_layout_)
        {
            const float value_w =
                std::max(0.0f, bounds_.w - kLabelW - kLabelGap);
            tk::TextStyle vs;
            vs.role      = tk::FontRole::Body;
            vs.halign    = tk::TextHAlign::Trailing;
            vs.valign    = tk::TextVAlign::Top;
            vs.trim      = tk::TextTrim::Ellipsis;
            vs.max_width = value_w;
            value_layout_ = ctx.factory.build_text(value_text_, vs);
        }

        const float cy = bounds_.y + (kRowH - label_layout_->measure().h) * 0.5f;
        ctx.canvas.draw_text(*label_layout_, {bounds_.x, cy},
                             pal.text_secondary);

        const auto vsz = value_layout_->measure();
        ctx.canvas.draw_text(*value_layout_,
                             {bounds_.x + bounds_.w - vsz.w, cy},
                             pal.text_muted);
    }

    bool on_pointer_move(tk::Point /*local*/) override
    {
        if (!has_stats_)
            return false;
        if (hovered_)
            return false;
        hovered_ = true;
        if (on_show_tooltip)
            on_show_tooltip(format_hit_miss(hits_, misses_), bounds_);
        return true;
    }

    void on_pointer_leave() override
    {
        if (!hovered_)
            return;
        hovered_ = false;
        if (on_hide_tooltip)
            on_hide_tooltip();
    }

private:
    std::string label_text_;
    std::string value_text_{"—"};
    std::unique_ptr<tk::TextLayout> label_layout_;
    std::unique_ptr<tk::TextLayout> value_layout_;

    uint64_t hits_     = 0;
    uint64_t misses_   = 0;
    bool     has_stats_ = false;
    bool     hovered_   = false;
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
