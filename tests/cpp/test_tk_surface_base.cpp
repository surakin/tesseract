#include <catch2/catch_test_macros.hpp>

#include "tk/surface.h"
#include "tk/controls.h"
#include "tk_test_host.h"

#include <cstdlib>
#include <memory>

namespace
{

// A backend-free SurfaceBase that records what shared code asks of it.
struct FakeSurface final : tk::SurfaceBase
{
    tk::Host& host() override { return host_; }
    tk::CanvasFactory& factory() override { std::abort(); }
    const tk::Theme& theme() const override { return theme_; }
    void set_root(std::unique_ptr<tk::Widget> r) override { root_ = std::move(r); }
    tk::Widget* root() const override { return root_.get(); }
    void relayout() override { ++relayouts; }
    void set_theme(const tk::Theme& t) override
    {
        theme_ = t;
        ++theme_sets;
    }
    void apply_scale_change(float) override {}
    void set_on_scale_changed(std::function<void(float)>) override {}
    void set_anim_cache(const tk::AnimImageCache*) override {}
    void update_anim_regions() override {}
    void add_layout_listener(std::function<void()>) override {}
    void set_on_file_drop_error(tk::FileDropErrorHandler) override {}

    TestHost host_{nullptr};
    tk::Theme theme_ = tk::Theme::light();
    std::unique_ptr<tk::Widget> root_;
    int relayouts = 0;
    int theme_sets = 0;
};

// Counts how often the widget tree is told about a theme.
struct ThemeProbe : tk::Widget
{
    tk::Size measure(tk::LayoutCtx&, tk::Size avail) override { return avail; }
    void on_theme_changed(const tk::Theme&) override { ++seen; }
    int seen = 0;
};

} // namespace

TEST_CASE("SurfaceBase::apply_theme themes the surface and its widget tree",
          "[tk][surface]")
{
    FakeSurface s;
    auto owner = tk::create_root_widget<ThemeProbe>(&s.host());
    auto* probe = owner.get();
    s.set_root(std::move(owner));
    s.apply_theme(tk::Theme::dark());
    CHECK(s.theme_sets == 1);
    CHECK(probe->seen == 1);
    CHECK(s.theme().mode == tk::ThemeMode::Dark);
}

TEST_CASE("SurfaceBase::apply_theme without a root only sets the theme",
          "[tk][surface]")
{
    FakeSurface s;
    s.apply_theme(tk::Theme::dark());
    CHECK(s.theme_sets == 1);
}
