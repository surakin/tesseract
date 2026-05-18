#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/RecoveryBanner.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

using namespace tk;
using tesseract::views::RecoveryBanner;

namespace
{

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 80);
    LayoutCtx layout_ctx()
    {
        return LayoutCtx{surface->factory(), Theme::light()};
    }
    PaintCtx paint_ctx()
    {
        return PaintCtx{surface->canvas(), surface->factory(), Theme::light()};
    }
    void run(Widget& root, Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

} // namespace

TEST_CASE("RecoveryBanner starts in Form state with field overlay visible",
          "[tk][view][recovery]")
{
    Stage st;
    RecoveryBanner banner;
    st.run(banner, {0, 0, 640, 48});
    CHECK(banner.state() == RecoveryBanner::State::Form);
    CHECK(banner.recovery_key_field_visible());
    auto fr = banner.recovery_key_field_rect();
    CHECK(fr.w > 100.0f);
    CHECK(fr.h > 0.0f);
}

TEST_CASE("RecoveryBanner Verifying state hides the key field",
          "[tk][view][recovery]")
{
    Stage st;
    RecoveryBanner banner;
    banner.set_state(RecoveryBanner::State::Verifying);
    st.run(banner, {0, 0, 640, 48});
    CHECK_FALSE(banner.recovery_key_field_visible());
    CHECK(banner.recovery_key_field_rect().w == 0.0f);
}

TEST_CASE("RecoveryBanner Failed state returns the field with the error",
          "[tk][view][recovery]")
{
    Stage st;
    RecoveryBanner banner;
    banner.set_state(RecoveryBanner::State::Failed);
    banner.set_failure_message("bad key");
    st.run(banner, {0, 0, 640, 48});
    CHECK(banner.recovery_key_field_visible());
    auto fr = banner.recovery_key_field_rect();
    CHECK(fr.w > 100.0f);
}

TEST_CASE("RecoveryBanner verify-button click fires on_verify with current key",
          "[tk][view][recovery]")
{
    Stage st;
    RecoveryBanner banner;
    banner.set_current_key("super-secret");
    std::string got;
    banner.on_verify = [&](const std::string& k)
    {
        got = k;
    };
    st.run(banner, {0, 0, 640, 48});

    // The verify button sits to the left of the dismiss button. Walk the
    // children to find it and synthesise a click.
    Button* verify = nullptr;
    for (auto& ch : banner.children())
    {
        if (auto* b = dynamic_cast<Button*>(ch.get()))
        {
            if (b->label() == "Verify")
            {
                verify = b;
                break;
            }
        }
    }
    REQUIRE(verify);
    verify->click();
    CHECK(got == "super-secret");
}

TEST_CASE("RecoveryBanner dismiss-button click fires on_dismiss",
          "[tk][view][recovery]")
{
    Stage st;
    RecoveryBanner banner;
    int dismissed = 0;
    banner.on_dismiss = [&]
    {
        ++dismissed;
    };
    st.run(banner, {0, 0, 640, 48});

    Button* dismiss = nullptr;
    for (auto& ch : banner.children())
    {
        if (auto* b = dynamic_cast<Button*>(ch.get()))
        {
            if (b->label() == "✕")
            {
                dismiss = b;
                break;
            }
        }
    }
    REQUIRE(dismiss);
    dismiss->click();
    CHECK(dismissed == 1);
}

TEST_CASE("RecoveryBanner Importing label updates with imported_keys count",
          "[tk][view][recovery]")
{
    Stage st;
    RecoveryBanner banner;
    banner.set_state(RecoveryBanner::State::Importing);
    banner.set_import_progress(42);
    st.run(banner, {0, 0, 640, 48});

    // Inspect the embedded label widget for the count.
    Label* label = nullptr;
    for (auto& ch : banner.children())
    {
        if (auto* l = dynamic_cast<Label*>(ch.get()))
        {
            label = l;
            break;
        }
    }
    REQUIRE(label);
    CHECK(label->text().find("42") != std::string::npos);

    banner.set_import_progress(0);
    CHECK(label->text().find("Downloading") != std::string::npos);
}
