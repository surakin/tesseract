#include "FontsEditor.h"

#include "FontFamilyPicker.h"

#include "views/font_prefs.h"
#include "tk/canvas.h"
#include "tk/font_config.h"
#include "tk/i18n.h"
#include "tk/widget.h"

#include <algorithm>
#include <array>
#include <string>

namespace tesseract::views
{

namespace
{

using Prefs = tesseract::Settings::FontPrefs;

constexpr float kRowGap = 8.0f;
constexpr float kLabelW = 150.0f;
constexpr float kValueW = 96.0f;
constexpr float kStepBtnW = 28.0f;

// Display name and the role whose size the row shows, per tk::FontCategory.
struct CategoryInfo
{
    const char* label;
    tk::FontRole lead;
};
constexpr std::array<CategoryInfo, tk::kFontCategoryCount> kCategories = {{
    {tk::N_("Messages"), tk::FontRole::Body},
    {tk::N_("Secondary text"), tk::FontRole::Caption},
    {tk::N_("Room list"), tk::FontRole::SidebarName},
    {tk::N_("Headings"), tk::FontRole::Title},
    {tk::N_("Buttons"), tk::FontRole::UiSemibold},
    {tk::N_("Emoji"), tk::FontRole::InlineEmoji},
}};

std::string pt_text(int pt)
{
    return tk::trf(tk::tr("{0} pt"), {std::to_string(pt)});
}

// "[−] value [+]" with the value label returned through `value`.
// `subject` (already translated) names the setting for screen readers: the
// −/+ glyph buttons would otherwise all read the same.
std::unique_ptr<tk::HBox> make_stepper(tk::Widget* parent,
                                       const std::string& subject,
                                       tk::Label*& value,
                                       std::function<void(int)> on_step)
{
    auto box = tk::create_widget<tk::HBox>(parent);
    box->set_spacing(4.0f);
    box->set_cross(tk::Cross::Center);

    auto minus = tk::create_widget<tk::Button>(
        parent, "\xE2\x88\x92", [on_step] { on_step(-1); },
        tk::Button::Variant::Subtle);
    minus->set_min_size({kStepBtnW, 0.0f});
    minus->set_accessible_name(tk::trf(tk::tr("Decrease {0}"), {subject}));
    box->add_child(std::move(minus));

    auto label = tk::create_widget<tk::Label>(parent, std::string{});
    label->set_halign(tk::TextHAlign::Center);
    label->set_min_size({kValueW, 0.0f});
    value = box->add_child(std::move(label));

    auto plus = tk::create_widget<tk::Button>(
        parent, "+", [on_step] { on_step(+1); }, tk::Button::Variant::Subtle);
    plus->set_min_size({kStepBtnW, 0.0f});
    plus->set_accessible_name(tk::trf(tk::tr("Increase {0}"), {subject}));
    box->add_child(std::move(plus));
    return box;
}

} // namespace

// ---------------------------------------------------------------------------
// FontPreview — sample timeline / room-list text drawn with the live roles.
// ---------------------------------------------------------------------------

class FontPreview : public tk::Widget
{
public:
    FontPreview() = default;

    tk::Size measure(tk::LayoutCtx& ctx, tk::Size constraints) override
    {
        ensure_(ctx.factory);
        float h = 2.0f * kPad;
        for (const Line& l : lines_)
            h += l.layout ? l.layout->measure().h : 0.0f;
        h += kLineGap * static_cast<float>(lines_.size() - 1);
        return {constraints.w, h};
    }

    void paint(tk::PaintCtx& ctx) override
    {
        const auto& pal = ctx.theme.palette;
        ctx.canvas.fill_rounded_rect(bounds_, 6.0f, pal.compose_card_bg);
        ctx.canvas.stroke_rounded_rect(bounds_, 6.0f, pal.border, 1.0f);
        ensure_(ctx.factory);
        float y = bounds_.y + kPad;
        for (const Line& l : lines_)
        {
            if (!l.layout)
                continue;
            ctx.canvas.draw_text(*l.layout, {bounds_.x + kPad, y},
                                 l.muted ? pal.text_muted : pal.text_primary);
            y += l.layout->measure().h + kLineGap;
        }
    }

    void on_fonts_changed() override
    {
        for (Line& l : lines_)
            l.layout.reset();
        if (host())
            host()->mark_needs_relayout();
    }

    tk::Role access_role() const override { return tk::Role::None; }

private:
    static constexpr float kPad = 10.0f;
    static constexpr float kLineGap = 4.0f;

    struct Line
    {
        tk::FontRole role;
        bool muted;
        tk::CachedTextLayout layout;
    };

    void ensure_(tk::CanvasFactory& factory)
    {
        if (lines_.empty())
        {
            lines_.push_back({tk::FontRole::Title, false, {}});
            lines_.push_back({tk::FontRole::SenderName, false, {}});
            lines_.push_back({tk::FontRole::Body, false, {}});
            lines_.push_back({tk::FontRole::Caption, true, {}});
            lines_.push_back({tk::FontRole::Timestamp, true, {}});
        }
        const std::array<std::string, 5> texts = {
            tk::tr("Room name"), tk::tr("Alex"),
            tk::tr("The quick brown fox jumps over the lazy dog."),
            tk::tr("Secondary text and hints"), std::string("12:34")};
        for (std::size_t i = 0; i < lines_.size(); ++i)
        {
            if (lines_[i].layout)
                continue;
            tk::TextStyle st;
            st.role = lines_[i].role;
            lines_[i].layout = factory.build_text(texts[i], st);
        }
    }

    std::vector<Line> lines_;
};

// ---------------------------------------------------------------------------
// FontsEditor
// ---------------------------------------------------------------------------

FontsEditor::FontsEditor()
{
    set_spacing(8.0f);
    prefs_ = tesseract::Settings::instance().fonts;

    // Base size.
    {
        auto row = tk::create_widget<tk::HBox>(this);
        row->set_spacing(kRowGap);
        row->set_cross(tk::Cross::Center);
        auto label = tk::create_widget<tk::Label>(this, tk::tr("Base size"));
        label->set_min_size({kLabelW, 0.0f});
        row->add_child(std::move(label));
        row->add_child(make_stepper(this, tk::tr("Base size"), base_value_,
                                    [this](int d) { step_base_(d); }));
        add_child(std::move(row));
    }

    // UI + monospace families.
    {
        auto row = tk::create_widget<tk::HBox>(this);
        row->set_spacing(kRowGap);
        row->set_cross(tk::Cross::Center);
        auto label = tk::create_widget<tk::Label>(this, tk::tr("Font"));
        label->set_min_size({kLabelW, 0.0f});
        row->add_child(std::move(label));
        auto picker = tk::create_widget<FontFamilyPicker>(this, false);
        picker->on_changed = [this](std::string v)
        {
            if (updating_ || prefs_.ui_family == v)
                return;
            prefs_.ui_family = std::move(v);
            changed_();
        };
        ui_family_ = row->add_child(std::move(picker));
        add_child(std::move(row));
    }
    {
        auto row = tk::create_widget<tk::HBox>(this);
        row->set_spacing(kRowGap);
        row->set_cross(tk::Cross::Center);
        auto label = tk::create_widget<tk::Label>(this, tk::tr("Monospace font"));
        label->set_min_size({kLabelW, 0.0f});
        row->add_child(std::move(label));
        auto picker = tk::create_widget<FontFamilyPicker>(this, true);
        picker->on_changed = [this](std::string v)
        {
            if (updating_ || prefs_.mono_family == v)
                return;
            prefs_.mono_family = std::move(v);
            changed_();
        };
        mono_family_ = row->add_child(std::move(picker));
        add_child(std::move(row));
    }

    // Per-category overrides.
    add_child(tk::create_widget<tk::Label>(
        this, tk::tr("Adjust individual text types"), tk::FontRole::Small));
    for (std::size_t i = 0; i < kCats; ++i)
    {
        auto row = tk::create_widget<tk::HBox>(this);
        row->set_spacing(kRowGap);
        row->set_cross(tk::Cross::Center);
        auto label = tk::create_widget<tk::Label>(this, tk::tr(kCategories[i].label));
        label->set_min_size({kLabelW, 0.0f});
        row->add_child(std::move(label));
        row->add_child(make_stepper(this, tk::tr(kCategories[i].label),
                                    rows_[i].value,
                                    [this, i](int d) { step_category_(i, d); }));
        if (i != static_cast<std::size_t>(tk::FontCategory::Emoji))
        {
            auto picker = tk::create_widget<FontFamilyPicker>(this, false);
            picker->on_changed = [this, i](std::string v)
            {
                if (updating_ || prefs_.categories[i].family == v)
                    return;
                prefs_.categories[i].family = std::move(v);
                changed_();
            };
            rows_[i].family = row->add_child(std::move(picker));
        }
        auto reset = tk::create_widget<tk::Button>(
            this, tk::tr("Reset"), [this, i] { reset_category_(i); },
            tk::Button::Variant::Link);
        reset->set_accessible_name(
            tk::trf(tk::tr("Reset {0}"), {tk::tr(kCategories[i].label)}));
        rows_[i].reset = row->add_child(std::move(reset));
        add_child(std::move(row));
    }

    preview_ = add_child(std::make_unique<FontPreview>());

    auto reset_all = tk::create_widget<tk::Button>(
        this, tk::tr("Reset to defaults"), [this] { reset_all_(); },
        tk::Button::Variant::Subtle);
    reset_all_btn_ = add_child(std::move(reset_all));

    refresh_();
}

void FontsEditor::set_prefs(const Prefs& prefs)
{
    prefs_ = prefs;
    refresh_();
}

void FontsEditor::on_fonts_changed()
{
    refresh_();
}

void FontsEditor::step_base_(int dir)
{
    const int sys = tk::system_base_pt() > 0 ? tk::system_base_pt() : 11;
    const int cur = prefs_.base_pt ? prefs_.base_pt : sys;
    const int next = std::clamp(cur + dir, tk::kFontBaseMinPt, tk::kFontBaseMaxPt);
    if (next == prefs_.base_pt)
        return;
    prefs_.base_pt = next;
    changed_();
}

void FontsEditor::step_category_(std::size_t cat, int dir)
{
    auto& d = prefs_.categories[cat].delta_pt;
    const int next = std::clamp(d + dir, tk::kFontDeltaMinPt, tk::kFontDeltaMaxPt);
    if (next == d)
        return;
    d = next;
    changed_();
}

void FontsEditor::reset_category_(std::size_t cat)
{
    if (prefs_.categories[cat] == Prefs::Category{})
        return;
    prefs_.categories[cat] = {};
    changed_();
}

void FontsEditor::reset_all_()
{
    if (prefs_ == Prefs{})
        return;
    prefs_ = {};
    changed_();
}

void FontsEditor::changed_()
{
    if (on_changed)
        on_changed(prefs_);
    refresh_();
}

// Re-reads every control from prefs_ + the live config. Sizes shown are the
// ones the toolkit now uses, so they reflect the platform's real base.
void FontsEditor::refresh_()
{
    const int sys = tk::system_base_pt();
    const tk::FontConfig cfg = tesseract::to_font_config(prefs_);

    if (base_value_)
    {
        base_value_->set_text(
            prefs_.base_pt
                ? pt_text(prefs_.base_pt)
                : (sys > 0 ? tk::trf(tk::tr("System ({0} pt)"),
                                     {std::to_string(sys)})
                           : tk::tr("System")));
    }

    updating_ = true;
    if (ui_family_)
        ui_family_->set_value(prefs_.ui_family);
    if (mono_family_)
        mono_family_->set_value(prefs_.mono_family);
    for (std::size_t i = 0; i < kCats; ++i)
    {
        auto& row = rows_[i];
        if (row.value)
        {
            row.value->set_text(sys > 0
                                    ? pt_text(tk::font_role_pt_with(
                                          cfg, kCategories[i].lead, sys))
                                    : std::to_string(prefs_.categories[i].delta_pt));
        }
        if (row.family)
            row.family->set_value(prefs_.categories[i].family);
        if (row.reset)
            row.reset->set_enabled(!(prefs_.categories[i] == Prefs::Category{}));
    }
    updating_ = false;

    if (reset_all_btn_)
        reset_all_btn_->set_enabled(!(prefs_ == Prefs{}));
}

} // namespace tesseract::views
