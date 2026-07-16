#include <catch2/catch_test_macros.hpp>

#include "tk/controls.h"
#include "tk/form_layout.h"
#include "tk_test_surface.h"

namespace
{

struct TkFormLayoutStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 800);

    tk::LayoutCtx lc()
    {
        return {surface->factory(), tk::Theme::light()};
    }

    void run(tk::Widget& root)
    {
        auto ctx = lc();
        root.measure(ctx, {640.0f, 800.0f});
        root.arrange(ctx, {0.0f, 0.0f, 640.0f, 800.0f});
        tk::PaintCtx pc{surface->canvas(), surface->factory(), tk::Theme::light()};
        root.paint(pc);
    }
};

} // namespace

TEST_CASE("FormLayout: add_row returns the control pointer", "[form_layout]")
{
    auto form_owner = tk::create_root_widget<tk::FormLayout>(nullptr);
    tk::FormLayout& form = *form_owner;
    auto           lbl  = tk::create_root_widget<tk::Label>(nullptr, "Value");
    tk::Label*     ptr  = form.add_row("Key", std::move(lbl));
    REQUIRE(ptr != nullptr);
}

TEST_CASE("FormLayout: empty form measures to zero height", "[form_layout]")
{
    TkFormLayoutStage          st;
    auto           lc = st.lc();
    auto form_owner = tk::create_root_widget<tk::FormLayout>(nullptr);
    tk::FormLayout& form = *form_owner;
    tk::Size       s = form.measure(lc, {640.0f, 800.0f});
    REQUIRE(s.h == 0.0f);
}

TEST_CASE("FormLayout: measure returns positive height with rows", "[form_layout]")
{
    TkFormLayoutStage          st;
    auto           lc = st.lc();
    auto form_owner = tk::create_root_widget<tk::FormLayout>(nullptr);
    tk::FormLayout& form = *form_owner;
    form.add_row("Key", tk::create_root_widget<tk::Label>(nullptr, "Value"));
    tk::Size s = form.measure(lc, {640.0f, 800.0f});
    REQUIRE(s.h > 0.0f);
}

TEST_CASE("FormLayout: all controls align to the same x after arrange",
          "[form_layout]")
{
    TkFormLayoutStage          st;
    auto form_owner = tk::create_root_widget<tk::FormLayout>(nullptr);
    tk::FormLayout& form = *form_owner;
    form.set_label_gap(8.0f).set_spacing(4.0f);
    tk::Label* c1 = form.add_row("Short",        tk::create_root_widget<tk::Label>(nullptr, "A"));
    tk::Label* c2 = form.add_row("Microphone",   tk::create_root_widget<tk::Label>(nullptr, "B"));
    tk::Label* c3 = form.add_row("Cam",          tk::create_root_widget<tk::Label>(nullptr, "C"));
    st.run(form);
    REQUIRE(c1->bounds().x == c2->bounds().x);
    REQUIRE(c2->bounds().x == c3->bounds().x);
}

TEST_CASE("FormLayout: controls x is beyond label x", "[form_layout]")
{
    TkFormLayoutStage          st;
    auto form_owner = tk::create_root_widget<tk::FormLayout>(nullptr);
    tk::FormLayout& form = *form_owner;
    form.set_label_gap(8.0f);
    // For label alignment test we need the labels too; access them via children().
    // Simpler: just check control x > form left edge.
    form.add_row("Label", tk::create_root_widget<tk::Label>(nullptr, "Value"));
    st.run(form);
    // The control should not start at x=0 (label takes space before it).
    REQUIRE(form.children()[1]->bounds().x > form.children()[0]->bounds().x);
}

TEST_CASE("FormLayout: paints without crash with multiple rows", "[form_layout]")
{
    TkFormLayoutStage          st;
    auto form_owner = tk::create_root_widget<tk::FormLayout>(nullptr);
    tk::FormLayout& form = *form_owner;
    form.add_row("Alpha", tk::create_root_widget<tk::Label>(nullptr, "1"));
    form.add_row("Beta",  tk::create_root_widget<tk::Label>(nullptr, "2"));
    form.add_row("Gamma", tk::create_root_widget<tk::Label>(nullptr, "3"));
    st.run(form); // must not throw
}
