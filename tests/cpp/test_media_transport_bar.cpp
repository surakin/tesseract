#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include "tk/layout.h"
#include "tk/slider.h"
#include "view_test_util.h"
#include "views/MediaTransportBar.h"
#include "views/MediaViewerPage.h"

#include <memory>
#include <vector>

using namespace tk;
using tesseract::views::MediaTransportBar;
using tesseract::views::MediaViewerPageHost;

namespace
{

// Just enough host for a MediaTransportBar: children are real widgets under
// one root, nothing else is exercised.
struct BarHost final : MediaViewerPageHost
{
    Widget* root = nullptr;

    Rect host_bounds() const override
    {
        return {0, 0, 400, 300};
    }
    bool host_fullscreen() const override
    {
        return false;
    }
    bool host_chrome_shown() const override
    {
        return true;
    }
    void host_refresh_chrome_autohide() override
    {
    }
    void host_request_repaint() override
    {
    }
    const Image* host_image(const std::string&) const override
    {
        return nullptr;
    }
    Button* host_add_icon_button() override
    {
        return root->add_child(
            create_widget<Button>(root, "", std::function<void()>{}, Button::Variant::Icon));
    }
    Button* host_add_label_button(std::string label) override
    {
        return root->add_child(create_widget<Button>(
            root, std::move(label), std::function<void()>{}, Button::Variant::Primary));
    }
    Slider* host_add_slider() override
    {
        return root->add_child(create_widget<Slider>(root, 0.0, 1.0, 0.001));
    }
    void host_playback_started() override
    {
    }
};

struct BarStage : vt::Stage
{
    std::unique_ptr<VBox> root = create_root_widget<VBox>(&host);
    BarHost bar_host;
    std::unique_ptr<MediaTransportBar> bar;
    std::vector<float> seeks;
    MediaTransportBar::State state;

    BarStage()
    {
        bar_host.root = root.get();
        bar = std::make_unique<MediaTransportBar>(bar_host, /*with_speed=*/false);
        bar->on_seek = [this](float f)
        {
            seeks.push_back(f);
        };
        state.duration_ms = 10000;
        host.set_root(root.get());
        refresh();
    }
    void refresh()
    {
        auto l = lc();
        root->measure(l, {400, 300});
        root->arrange(l, {0, 0, 400, 300});
        bar->arrange(l, {0, 0, 400, MediaTransportBar::kHeight}, true);
        auto p = pc();
        bar->paint(p, state);
    }
    Slider* slider() const
    {
        return bar->scrub_slider();
    }
    // Pointer position `frac` of the way along the slider, in its local space.
    Point at(float frac) const
    {
        const Rect r = slider()->bounds();
        return {r.x + r.w * frac, r.y + r.h * 0.5f};
    }
    void press(float frac)
    {
        Widget* hit = root->dispatch_pointer_down(at(frac));
        REQUIRE(hit == slider());
    }
};

} // namespace

TEST_CASE("transport bar scrub slider seeks to the pressed fraction", "[tk][transportbar]")
{
    BarStage s;
    s.press(0.5f);
    REQUIRE(s.seeks.size() == 1);
    CHECK(s.seeks.back() == Catch::Approx(0.5f).margin(0.02));
    CHECK(s.bar->scrubbing());
    s.slider()->on_pointer_up({0, 0}, true);
    CHECK_FALSE(s.bar->scrubbing());
}

TEST_CASE("transport bar scrub slider never seeks past the downloaded part", "[tk][transportbar]")
{
    BarStage s;
    s.state.buffered_frac = 0.4f;
    s.refresh();
    s.press(0.9f);
    REQUIRE_FALSE(s.seeks.empty());
    CHECK(s.seeks.back() <= 0.4f + 1e-4f);
    CHECK(s.seeks.back() == Catch::Approx(0.4f).margin(1e-3));
}

TEST_CASE("transport bar scrub slider ignores a stream of unknown duration", "[tk][transportbar]")
{
    BarStage s;
    s.state.duration_ms = 0;
    s.refresh();
    s.press(0.5f);
    CHECK(s.seeks.empty());
}

TEST_CASE("transport bar does not move the slider under the user's drag", "[tk][transportbar]")
{
    BarStage s;
    s.press(0.25f);
    const double dragged = s.slider()->value();
    s.state.position_ms = 9000; // the player reports a far-off position mid-drag
    s.refresh();
    CHECK(s.slider()->value() == dragged);
    s.slider()->on_pointer_up({0, 0}, true);
    s.refresh();
    CHECK(s.slider()->value() == Catch::Approx(0.9).margin(0.01));
}

TEST_CASE("transport bar scrub slider is not a tab stop", "[tk][transportbar]")
{
    BarStage s;
    CHECK_FALSE(s.slider()->focusable());
    CHECK(s.slider()->access_role() == Role::Slider);
}

TEST_CASE("transport bar counts the scrub slider as hovered and cancels a drag on deactivate",
          "[tk][transportbar]")
{
    BarStage s;
    CHECK_FALSE(s.bar->hovered());
    s.host.dispatch_pointer_move(s.at(0.5f));
    CHECK(s.bar->hovered());

    s.host.dispatch_pointer_move({-50.0f, -50.0f});
    s.press(0.5f);
    CHECK(s.bar->hovered()); // a drag in progress keeps the controls up
    s.bar->deactivate();
    CHECK_FALSE(s.bar->scrubbing());
}
