#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/settings/FontsEditor.h"

#include "views/font_prefs.h"

#include <vector>

using tesseract::Settings;
using tesseract::views::FontsEditor;

namespace
{

// Leaves the process-wide font config and the Settings singleton as found.
struct FontStateGuard
{
    FontStateGuard()
    {
        Settings::instance().fonts = {};
        tk::set_font_config({});
    }
    ~FontStateGuard()
    {
        Settings::instance().fonts = {};
        tk::set_font_config({});
    }
};

struct FeStage : vt::Stage
{
    FontStateGuard guard;
    std::unique_ptr<FontsEditor> ed = tk::create_root_widget<FontsEditor>(&host);
    std::vector<Settings::FontPrefs> fired;
    FeStage()
    {
        ed->on_changed = [this](const Settings::FontPrefs& p) { fired.push_back(p); };
        mount(*ed, {0, 0, 700, 900});
    }
};

} // namespace

TEST_CASE("FontsEditor starts from the saved prefs and set_prefs is silent",
          "[tk][view][fonts]")
{
    FeStage s;
    CHECK(s.ed->prefs() == Settings::FontPrefs{});

    Settings::FontPrefs p;
    p.base_pt = 15;
    p.categories[3].delta_pt = 2;
    s.ed->set_prefs(p);
    CHECK(s.ed->prefs() == p);
    CHECK(s.fired.empty());
}

TEST_CASE("FontsEditor base stepper moves the base and fires", "[tk][view][fonts]")
{
    FeStage s;
    s.ed->step_base_for_test(+1);
    REQUIRE(s.fired.size() == 1);
    // From "system" the first step lands one point off the system size.
    const int first = s.fired.back().base_pt;
    CHECK(first >= tk::kFontBaseMinPt);
    s.ed->step_base_for_test(+1);
    REQUIRE(s.fired.size() == 2);
    CHECK(s.fired.back().base_pt == first + 1);
}

TEST_CASE("FontsEditor base stepper clamps at the limits", "[tk][view][fonts]")
{
    FeStage s;
    Settings::FontPrefs p;
    p.base_pt = tk::kFontBaseMaxPt;
    s.ed->set_prefs(p);
    s.ed->step_base_for_test(+1);
    CHECK(s.fired.empty()); // already at the max: nothing to report
    CHECK(s.ed->prefs().base_pt == tk::kFontBaseMaxPt);
}

TEST_CASE("FontsEditor category stepper edits only its category",
          "[tk][view][fonts]")
{
    FeStage s;
    s.ed->step_category_for_test(1, +1);
    s.ed->step_category_for_test(1, +1);
    REQUIRE(s.fired.size() == 2);
    const auto& p = s.fired.back();
    CHECK(p.categories[1].delta_pt == 2);
    CHECK(p.categories[0].delta_pt == 0);
    CHECK(p.base_pt == 0);

    s.ed->step_category_for_test(1, -10);
    s.ed->step_category_for_test(1, -1);
    CHECK(s.ed->prefs().categories[1].delta_pt >= tk::kFontDeltaMinPt);
}

TEST_CASE("FontsEditor reset to defaults clears everything and fires once",
          "[tk][view][fonts]")
{
    FeStage s;
    Settings::FontPrefs p;
    p.base_pt = 18;
    p.ui_family = "Inter";
    p.mono_family = "Fira Code";
    p.categories[2].delta_pt = 3;
    p.categories[2].family = "Georgia";
    s.ed->set_prefs(p);

    s.ed->reset_all_for_test();
    REQUIRE(s.fired.size() == 1);
    CHECK(s.fired.back() == Settings::FontPrefs{});
    CHECK(s.ed->prefs() == Settings::FontPrefs{});

    s.ed->reset_all_for_test(); // already default: no-op
    CHECK(s.fired.size() == 1);
}
