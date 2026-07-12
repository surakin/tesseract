#include "DevicesSection.h"

#include "SettingsGroup.h"

#include "tk/controls.h"
#include "tk/theme.h"
#include "tk/widget.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>

namespace tesseract::views
{

namespace
{

constexpr float kRowPadX     = 12.0f;
constexpr float kRowPadY     = 10.0f;
constexpr float kRowMinH     = 64.0f;
constexpr float kDevicesSectionChipPadX    = 8.0f;
constexpr float kChipH       = 18.0f;
constexpr float kChipGap     = 6.0f;
constexpr float kDevicesSectionLineGap     = 4.0f;
constexpr float kDevicesSectionErrorGap    = 4.0f;
constexpr float kButtonGap   = 8.0f;
constexpr float kRowRadius   = 6.0f;

std::string format_relative_ts(std::uint64_t ts_ms)
{
    if (ts_ms == 0)
        return "never";

    using namespace std::chrono;
    const auto now =
        duration_cast<milliseconds>(system_clock::now().time_since_epoch())
            .count();
    if (static_cast<std::int64_t>(ts_ms) > now + 60'000)
        return "in the future"; // clock skew

    const auto delta_ms = (now > static_cast<std::int64_t>(ts_ms))
                              ? (now - static_cast<std::int64_t>(ts_ms))
                              : 0;
    const auto secs = delta_ms / 1000;
    char buf[64];
    if (secs < 60)
        std::snprintf(buf, sizeof(buf), "just now");
    else if (secs < 3600)
        std::snprintf(buf, sizeof(buf), "%llum ago",
                      (unsigned long long)(secs / 60));
    else if (secs < 86400)
        std::snprintf(buf, sizeof(buf), "%lluh ago",
                      (unsigned long long)(secs / 3600));
    else if (secs < 86400 * 30)
        std::snprintf(buf, sizeof(buf), "%llud ago",
                      (unsigned long long)(secs / 86400));
    else
        std::snprintf(buf, sizeof(buf), "%llumo ago",
                      (unsigned long long)(secs / (86400 * 30)));
    return buf;
}

std::string compose_subline(const tesseract::Client::Device& d)
{
    std::string out = d.id;
    if (!d.last_seen_ip.empty())
    {
        out += "  •  ";
        out += d.last_seen_ip;
    }
    if (d.last_seen_ts != 0)
    {
        out += "  •  ";
        out += format_relative_ts(d.last_seen_ts);
    }
    return out;
}

} // namespace

// ---------------------------------------------------------------------------
// DeviceRow — one device's row. Owns its action buttons as children and paints
// its own text + chips. Switches between "normal" (Sign out) and "UIA pending"
// (Confirm in browser / I've confirmed / Cancel) states.
// ---------------------------------------------------------------------------

class DevicesSection::DeviceRow : public tk::Widget
{
public:
    DeviceRow(tesseract::Client::Device device,
              std::function<void(std::string)> on_delete,
              std::function<void(std::string, std::string)> on_confirm,
              std::function<void(std::string)> on_cancel);

    const std::string& device_id() const { return device_.id; }

    void set_busy(bool busy);
    void set_error(std::string err);
    void enter_uia(std::string fallback_url, std::string session);
    void clear_uia();

    tk::Size measure(tk::LayoutCtx&, tk::Size constraints) override;
    void arrange(tk::LayoutCtx&, tk::Rect bounds) override;
    void paint(tk::PaintCtx&) override;

private:
    void rebuild_buttons_();
    void invalidate_text_();

    tesseract::Client::Device device_;

    bool busy_ = false;
    std::string error_;
    bool uia_ = false;
    std::string uia_fallback_url_;
    std::string uia_session_;

    std::function<void(std::string)>                       on_delete_;
    std::function<void(std::string, std::string)>          on_confirm_;
    std::function<void(std::string)>                       on_cancel_;

    // Borrowed pointers; ownership lives with this widget's child slots.
    tk::Button* signout_btn_ = nullptr;
    tk::Button* open_btn_    = nullptr;
    tk::Button* confirm_btn_ = nullptr;
    tk::Button* cancel_btn_  = nullptr;

    // Cached text layouts. Rebuilt when content or width changes.
    std::unique_ptr<tk::TextLayout> name_layout_;
    std::unique_ptr<tk::TextLayout> sub_layout_;
    std::unique_ptr<tk::TextLayout> verif_layout_;
    std::unique_ptr<tk::TextLayout> this_dev_layout_;
    std::unique_ptr<tk::TextLayout> uia_hint_layout_;
    std::unique_ptr<tk::TextLayout> error_layout_;
    std::unique_ptr<tk::TextLayout> busy_layout_;
    float cached_w_ = -1;
};

DevicesSection::DeviceRow::DeviceRow(
    tesseract::Client::Device device,
    std::function<void(std::string)> on_delete,
    std::function<void(std::string, std::string)> on_confirm,
    std::function<void(std::string)> on_cancel)
    : device_(std::move(device))
    , on_delete_(std::move(on_delete))
    , on_confirm_(std::move(on_confirm))
    , on_cancel_(std::move(on_cancel))
{
    rebuild_buttons_();
}

void DevicesSection::DeviceRow::rebuild_buttons_()
{
    // Drop the old buttons and rebuild from current state. Cheap; this only
    // runs on state transitions (Sign out → UIA, UIA → normal).
    clear_children();
    signout_btn_ = nullptr;
    open_btn_    = nullptr;
    confirm_btn_ = nullptr;
    cancel_btn_  = nullptr;

    if (uia_)
    {
        auto open = std::make_unique<tk::Button>(
            "Open in browser", std::function<void()>{}, tk::Button::Variant::Subtle);
        const std::string url = uia_fallback_url_;
        open->set_on_click([url]
        {
            tesseract::Client::open_in_browser(url);
        });
        open_btn_ = add_child(std::move(open));

        auto confirm = std::make_unique<tk::Button>(
            "I've confirmed", std::function<void()>{},
            tk::Button::Variant::Destructive);
        confirm->set_on_click([this]
        {
            if (on_confirm_)
                on_confirm_(device_.id, uia_session_);
        });
        confirm_btn_ = add_child(std::move(confirm));

        auto cancel = std::make_unique<tk::Button>(
            "Cancel", std::function<void()>{}, tk::Button::Variant::Subtle);
        const std::string id_for_cancel = device_.id;
        auto on_cancel = on_cancel_;
        cancel->set_on_click([on_cancel, id_for_cancel]
        {
            if (on_cancel)
                on_cancel(id_for_cancel);
        });
        cancel_btn_ = add_child(std::move(cancel));
    }
    else if (!device_.is_current)
    {
        auto signout = std::make_unique<tk::Button>(
            "Sign out", std::function<void()>{},
            tk::Button::Variant::Destructive);
        signout->set_on_click([this]
        {
            if (busy_) return;
            if (on_delete_)
                on_delete_(device_.id);
        });
        signout->set_enabled(!busy_);
        signout_btn_ = add_child(std::move(signout));
    }
}

void DevicesSection::DeviceRow::invalidate_text_()
{
    name_layout_.reset();
    sub_layout_.reset();
    verif_layout_.reset();
    this_dev_layout_.reset();
    uia_hint_layout_.reset();
    error_layout_.reset();
    busy_layout_.reset();
    cached_w_ = -1;
}

void DevicesSection::DeviceRow::set_busy(bool busy)
{
    if (busy_ == busy) return;
    busy_ = busy;
    if (signout_btn_)
        signout_btn_->set_enabled(!busy_);
    busy_layout_.reset();
}

void DevicesSection::DeviceRow::set_error(std::string err)
{
    error_ = std::move(err);
    error_layout_.reset();
}

void DevicesSection::DeviceRow::enter_uia(std::string fallback_url,
                                          std::string session)
{
    uia_fallback_url_ = std::move(fallback_url);
    uia_session_      = std::move(session);
    uia_              = true;
    error_.clear();
    invalidate_text_();
    rebuild_buttons_();
}

void DevicesSection::DeviceRow::clear_uia()
{
    if (!uia_) return;
    uia_ = false;
    uia_fallback_url_.clear();
    uia_session_.clear();
    busy_ = false;
    invalidate_text_();
    rebuild_buttons_();
}

tk::Size DevicesSection::DeviceRow::measure(tk::LayoutCtx&, tk::Size constraints)
{
    // The row spans the available width; height is content-driven, with a
    // floor of kRowMinH so isolated short rows still look like rows.
    return {constraints.w, kRowMinH};
}

void DevicesSection::DeviceRow::arrange(tk::LayoutCtx& ctx, tk::Rect bounds)
{
    bounds_ = bounds;

    // Re-measure text on width changes.
    if (cached_w_ != bounds.w)
    {
        invalidate_text_();
        cached_w_ = bounds.w;
    }

    // Lazily build each text layout independently — set_busy/set_error/
    // enter_uia/clear_uia reset just the affected cache, so we can't gate
    // every rebuild behind `if (!name_layout_)`.
    if (!name_layout_)
    {
        tk::TextStyle name_st;
        name_st.role = tk::FontRole::Title;
        name_layout_ = ctx.factory.build_text(
            device_.display_name.empty() ? std::string("(no name)")
                                         : device_.display_name,
            name_st);
    }
    if (!sub_layout_)
    {
        tk::TextStyle sub_st;
        sub_st.role = tk::FontRole::Small;
        sub_layout_ = ctx.factory.build_text(compose_subline(device_), sub_st);
    }
    if (!verif_layout_)
    {
        tk::TextStyle chip_st;
        chip_st.role = tk::FontRole::UiSemibold;
        const char* verif_label = "Unknown";
        if (device_.verification == tesseract::Client::DeviceVerification::Verified)
            verif_label = "Verified";
        else if (device_.verification ==
                 tesseract::Client::DeviceVerification::Unverified)
            verif_label = "Unverified";
        verif_layout_ = ctx.factory.build_text(verif_label, chip_st);
    }
    if (device_.is_current && !this_dev_layout_)
    {
        tk::TextStyle chip_st;
        chip_st.role = tk::FontRole::UiSemibold;
        this_dev_layout_ = ctx.factory.build_text("This device", chip_st);
    }
    if (uia_ && !uia_hint_layout_)
    {
        tk::TextStyle hint_st;
        hint_st.role = tk::FontRole::Small;
        uia_hint_layout_ = ctx.factory.build_text(
            "Complete sign-out in the browser, then click \"I've "
            "confirmed\".",
            hint_st);
    }
    if (!error_.empty() && !error_layout_)
    {
        tk::TextStyle err_st;
        err_st.role = tk::FontRole::Small;
        error_layout_ = ctx.factory.build_text(error_, err_st);
    }
    if (busy_ && !busy_layout_)
    {
        tk::TextStyle busy_st;
        busy_st.role = tk::FontRole::Small;
        busy_layout_ = ctx.factory.build_text("Working…", busy_st);
    }

    // Arrange buttons along the right edge at the row's vertical centre.
    auto place_button = [&](tk::Button* btn, float right_x, float& used) {
        if (!btn) return;
        tk::Size sz = btn->measure(ctx, {bounds.w, kRowMinH});
        const float w = sz.w;
        const float h = std::min(sz.h, kRowMinH - 2.0f * 4.0f);
        const float x = right_x - used - w;
        const float y = bounds.y + (kRowMinH - h) * 0.5f;
        btn->arrange(ctx, {x, y, w, h});
        used += w + kButtonGap;
    };
    const float right_x = bounds.x + bounds.w - kRowPadX;
    float used = 0.0f;
    place_button(cancel_btn_, right_x, used);
    place_button(confirm_btn_, right_x, used);
    place_button(open_btn_, right_x, used);
    place_button(signout_btn_, right_x, used);
}

void DevicesSection::DeviceRow::paint(tk::PaintCtx& ctx)
{
    const auto& pal = ctx.theme.palette;

    // Subtle row card.
    ctx.canvas.fill_rounded_rect(bounds_, kRowRadius, pal.compose_card_bg);

    const float left  = bounds_.x + kRowPadX;
    float       y     = bounds_.y + kRowPadY;
    const float right = bounds_.x + bounds_.w - kRowPadX;

    // First line: name + chips.
    if (name_layout_)
    {
        tk::Size sz = name_layout_->measure();
        ctx.canvas.draw_text(*name_layout_, {left, y}, pal.text_primary);

        float chip_x = left + sz.w + kChipGap;
        const float chip_y = y + 2.0f;

        // "This device" chip.
        if (this_dev_layout_)
        {
            tk::Size cs = this_dev_layout_->measure();
            const tk::Rect chip = {chip_x, chip_y, cs.w + 2.0f * kDevicesSectionChipPadX,
                                   kChipH};
            ctx.canvas.fill_rounded_rect(chip, kChipH * 0.5f, pal.accent);
            ctx.canvas.draw_text(*this_dev_layout_,
                                 {chip_x + kDevicesSectionChipPadX,
                                  chip_y + (kChipH - cs.h) * 0.5f},
                                 pal.text_on_accent);
            chip_x += chip.w + kChipGap;
        }

        // Verification chip (colour by state).
        if (verif_layout_)
        {
            tk::Size cs = verif_layout_->measure();
            const tk::Rect chip = {chip_x, chip_y, cs.w + 2.0f * kDevicesSectionChipPadX,
                                   kChipH};
            tk::Color bg = pal.chip_bg;
            tk::Color fg = pal.chip_text;
            if (device_.verification ==
                tesseract::Client::DeviceVerification::Verified)
            {
                bg = pal.accent;
                fg = pal.text_on_accent;
            }
            else if (device_.verification ==
                     tesseract::Client::DeviceVerification::Unverified)
            {
                bg = pal.destructive;
                fg = pal.text_on_accent;
            }
            // Clip to avoid overlapping with action buttons on narrow rows.
            const float max_right = right - 2.0f * kDevicesSectionChipPadX;
            if (chip.x + chip.w <= max_right)
            {
                ctx.canvas.fill_rounded_rect(chip, kChipH * 0.5f, bg);
                ctx.canvas.draw_text(*verif_layout_,
                                     {chip_x + kDevicesSectionChipPadX,
                                      chip_y + (kChipH - cs.h) * 0.5f},
                                     fg);
            }
        }

        y += sz.h + kDevicesSectionLineGap;
    }

    // Second line: device id • last seen.
    if (sub_layout_)
    {
        ctx.canvas.draw_text(*sub_layout_, {left, y}, pal.text_muted);
        y += sub_layout_->measure().h + kDevicesSectionLineGap;
    }

    // UIA hint or error or busy text.
    if (uia_ && uia_hint_layout_)
    {
        ctx.canvas.draw_text(*uia_hint_layout_, {left, y}, pal.text_secondary);
    }
    else if (!error_.empty() && error_layout_)
    {
        ctx.canvas.draw_text(*error_layout_, {left, y}, pal.destructive);
    }
    else if (busy_ && busy_layout_)
    {
        ctx.canvas.draw_text(*busy_layout_, {left, y}, pal.text_muted);
    }

    // Buttons (children) paint themselves.
    for (auto& ch : children())
    {
        if (ch->visible())
            ch->paint(ctx);
    }
}

// ---------------------------------------------------------------------------
// DevicesSection
// ---------------------------------------------------------------------------

DevicesSection::DevicesSection()
{
    group_ = add_group("Sessions");
}

DevicesSection::~DevicesSection() = default;

void DevicesSection::set_loading(bool loading)
{
    if (loading_ == loading) return;
    loading_ = loading;
    rebuild_();
}

void DevicesSection::set_current_device_id(std::string id)
{
    if (current_device_id_ == id) return;
    current_device_id_ = std::move(id);
    // Update is_current on the cached devices so the list re-renders correctly
    // when set_current_device_id arrives before/after set_devices.
    for (auto& d : devices_)
    {
        d.is_current = (d.id == current_device_id_);
    }
    rebuild_();
}

void DevicesSection::set_devices(std::vector<tesseract::Client::Device> devices)
{
    devices_ = std::move(devices);
    if (!current_device_id_.empty())
    {
        for (auto& d : devices_)
            d.is_current = (d.id == current_device_id_);
    }
    loading_ = false;
    rebuild_();
}

void DevicesSection::set_device_busy(const std::string& device_id, bool busy)
{
    for (auto* row : rows_)
        if (row && row->device_id() == device_id)
            row->set_busy(busy);
}

void DevicesSection::set_device_error(const std::string& device_id,
                                       std::string error)
{
    for (auto* row : rows_)
        if (row && row->device_id() == device_id)
            row->set_error(std::move(error));
}

void DevicesSection::enter_uia_state(const std::string& device_id,
                                      std::string fallback_url,
                                      std::string session)
{
    for (auto* row : rows_)
        if (row && row->device_id() == device_id)
            row->enter_uia(std::move(fallback_url), std::move(session));
}

void DevicesSection::clear_uia_state(const std::string& device_id)
{
    for (auto* row : rows_)
        if (row && row->device_id() == device_id)
            row->clear_uia();
}

void DevicesSection::rebuild_()
{
    // Clear and rebuild the group's children. SettingsGroup forwards its
    // children through tk::VBox so re-adding rows just stacks them again.
    group_->clear_children();
    rows_.clear();
    loading_label_ = nullptr;
    empty_label_   = nullptr;

    if (loading_)
    {
        auto lbl = std::make_unique<tk::Label>("Loading sessions…");
        loading_label_ = group_->add_widget(std::move(lbl));
        return;
    }

    if (devices_.empty())
    {
        auto lbl = std::make_unique<tk::Label>("No sessions.");
        empty_label_ = group_->add_widget(std::move(lbl));
        return;
    }

    for (const auto& d : devices_)
    {
        auto row = std::make_unique<DeviceRow>(
            d,
            [this](std::string id)
            {
                if (on_delete_requested)
                    on_delete_requested(std::move(id));
            },
            [this](std::string id, std::string session)
            {
                if (on_uia_confirmed)
                    on_uia_confirmed(std::move(id), std::move(session));
            },
            [this](std::string id)
            {
                if (on_uia_cancelled)
                    on_uia_cancelled(std::move(id));
            });
        rows_.push_back(group_->add_widget(std::move(row)));
    }
}

} // namespace tesseract::views
