#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/theme.h"
#include "views/EncryptionReminderBanner.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

using namespace tk;
using tesseract::views::EncryptionReminderBanner;

namespace
{

struct ReminderBannerStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 48);
    void run(Widget& root, Rect bounds)
    {
        LayoutCtx lc{surface->factory(), Theme::light()};
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
        root.paint(pc);
    }
};

Button* find_button(Widget& root, const std::string& lbl)
{
    for (auto& ch : root.children())
        if (auto* b = dynamic_cast<Button*>(ch.get()))
            if (b->label() == lbl) return b;
    return nullptr;
}

} // namespace

TEST_CASE("EncryptionReminderBanner is a fixed 48px strip", "[tk][view][encryption]")
{
    ReminderBannerStage st;
    EncryptionReminderBanner banner;
    LayoutCtx lc{st.surface->factory(), Theme::light()};
    CHECK(banner.measure(lc, {640, 400}).h == EncryptionReminderBanner::kHeight);
}

TEST_CASE("EncryptionReminderBanner Locked offers Unlock and fires on_open",
          "[tk][view][encryption]")
{
    ReminderBannerStage st;
    EncryptionReminderBanner banner;
    banner.set_kind(EncryptionReminderBanner::Kind::Locked);
    bool opened = false;
    banner.on_open = [&] { opened = true; };
    st.run(banner, {0, 0, 640, 48});

    CHECK(banner.label_text().find("locked") != std::string::npos);
    Button* b = find_button(banner, "Unlock");
    REQUIRE(b);
    b->click();
    CHECK(opened);
}

TEST_CASE("EncryptionReminderBanner SetupNeeded offers Set up recovery",
          "[tk][view][encryption]")
{
    ReminderBannerStage st;
    EncryptionReminderBanner banner;
    banner.set_kind(EncryptionReminderBanner::Kind::SetupNeeded);
    st.run(banner, {0, 0, 640, 48});
    CHECK(banner.label_text().find("backed up") != std::string::npos);
    CHECK(find_button(banner, "Set up recovery") != nullptr);
    CHECK(find_button(banner, "Unlock") == nullptr);
}

TEST_CASE("EncryptionReminderBanner dismiss fires on_dismiss", "[tk][view][encryption]")
{
    ReminderBannerStage st;
    EncryptionReminderBanner banner;
    bool dismissed = false;
    banner.on_dismiss = [&] { dismissed = true; };
    st.run(banner, {0, 0, 640, 48});
    Button* b = find_button(banner, "\xe2\x9c\x95");
    REQUIRE(b);
    b->click();
    CHECK(dismissed);
}
