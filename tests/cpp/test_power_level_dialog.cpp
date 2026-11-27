#include <catch2/catch_test_macros.hpp>

#include "tk_test_host.h"
#include "tk_test_surface.h"
#include "views/PowerLevelDialog.h"
#include "views/power_level_presets.h"

#include "tk/theme.h"

#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using tesseract::views::PowerLevelDialog;

namespace
{

constexpr std::int64_t kPowerLevelCreator = std::numeric_limits<std::int64_t>::max();

struct PowerLevelSave
{
    std::string room_id, user_id, display_name;
    std::int64_t level = 0;
};

struct PowerLevelFixture
{
    StubHost host;
    std::unique_ptr<PowerLevelDialog> dlg;
    std::vector<PowerLevelSave> saves;
    int closes = 0;

    PowerLevelFixture()
    {
        dlg = tk::create_root_widget<PowerLevelDialog>(&host);
        host.set_root(dlg.get());
        dlg->on_save =
            [this](std::string room_id, std::string user_id, std::string name, std::int64_t level)
        {
            saves.push_back({std::move(room_id), std::move(user_id), std::move(name), level});
        };
        dlg->on_close = [this]
        {
            ++closes;
        };
    }

    void open(std::int64_t current, std::int64_t own)
    {
        PowerLevelDialog::Target t;
        t.room_id = "!r:x";
        t.room_name = "Room";
        t.user_id = "@u:x";
        t.display_name = "U";
        t.current_level = current;
        t.own_level = own;
        dlg->open(std::move(t));
    }

    // What a user pick in the dropdown does: select + fire on_changed.
    void select(const std::string& value)
    {
        auto* combo = dlg->level_combo();
        REQUIRE(combo);
        combo->set_selected_value(value);
        REQUIRE(combo->on_changed);
        combo->on_changed(value);
    }

    // What typing into the custom field does: set text + fire on_changed.
    void type_custom(const std::string& text)
    {
        select("custom");
        REQUIRE(dlg->custom_field());
        dlg->custom_field()->set_text(text);
        REQUIRE_FALSE(host.fields_created.empty());
        auto* native = host.fields_created.back();
        if (native->on_changed)
        {
            native->on_changed(text);
        }
    }

    std::vector<std::string> option_values() const
    {
        std::vector<std::string> out;
        for (const auto& o : dlg->level_combo()->options())
        {
            out.push_back(o.value);
        }
        return out;
    }
};

} // namespace

TEST_CASE("PowerLevelDialog: power_level_preset_options omits presets above the cap",
          "[dialog][power_level]")
{
    using tesseract::views::power_level_preset_options;
    auto all = power_level_preset_options();
    REQUIRE(all.size() == 3);
    CHECK(all[0].value == "0");
    CHECK(all[1].value == "50");
    CHECK(all[2].value == "100");
    CHECK(power_level_preset_options(50).size() == 2);
    CHECK(power_level_preset_options(-1).empty());
    CHECK(tesseract::views::is_power_level_preset(50));
    CHECK_FALSE(tesseract::views::is_power_level_preset(25));
}

TEST_CASE("PowerLevelDialog: opens on the preset matching the current level",
          "[dialog][power_level]")
{
    PowerLevelFixture f;
    f.open(50, 100);
    CHECK(f.dlg->is_open());
    CHECK(f.dlg->level_combo()->selected_value() == "50");
    REQUIRE(f.dlg->custom_field());
    CHECK_FALSE(f.dlg->custom_field()->visible());
    CHECK_FALSE(f.dlg->save_enabled());
}

TEST_CASE("PowerLevelDialog: non-preset current level opens on Custom prefilled",
          "[dialog][power_level]")
{
    PowerLevelFixture f;
    f.open(25, 100);
    CHECK(f.dlg->level_combo()->selected_value() == "custom");
    REQUIRE(f.dlg->custom_field());
    CHECK(f.dlg->custom_field()->visible());
    CHECK(f.dlg->custom_field()->text() == "25");
    CHECK(f.dlg->chosen_level() == std::optional<std::int64_t>(25));
    CHECK_FALSE(f.dlg->save_enabled());
}

TEST_CASE("PowerLevelDialog: presets above own level are not offered", "[dialog][power_level]")
{
    PowerLevelFixture f;
    f.open(0, 50);
    CHECK(f.option_values() == std::vector<std::string>{"0", "50", "custom"});
}

TEST_CASE("PowerLevelDialog: custom above own level shows error and disables Save",
          "[dialog][power_level]")
{
    PowerLevelFixture f;
    f.open(0, 50);
    f.type_custom("75");
    CHECK(f.dlg->chosen_level() == std::optional<std::int64_t>(75));
    CHECK(f.dlg->error_shown());
    CHECK_FALSE(f.dlg->save_enabled());
}

TEST_CASE("PowerLevelDialog: invalid custom text disables Save", "[dialog][power_level]")
{
    PowerLevelFixture f;
    f.open(0, 100);
    for (const char* bad : {"", "abc", "1e3", "9007199254740992", "-", " 5 5", "--5"})
    {
        CAPTURE(bad);
        f.type_custom(bad);
        CHECK(f.dlg->chosen_level() == std::nullopt);
        CHECK_FALSE(f.dlg->save_enabled());
        CHECK_FALSE(f.dlg->error_shown());
    }
    f.type_custom("-5");
    CHECK(f.dlg->chosen_level() == std::optional<std::int64_t>(-5));
    CHECK(f.dlg->save_enabled());
    f.type_custom("  42 ");
    CHECK(f.dlg->chosen_level() == std::optional<std::int64_t>(42));
    f.type_custom("-9007199254740991");
    CHECK(f.dlg->chosen_level() == std::optional<std::int64_t>(-9007199254740991LL));
}

TEST_CASE("PowerLevelDialog: choosing own level warns but allows Save", "[dialog][power_level]")
{
    PowerLevelFixture f;
    f.open(0, 100);
    CHECK_FALSE(f.dlg->same_level_warning_shown());
    f.select("100");
    CHECK(f.dlg->same_level_warning_shown());
    CHECK_FALSE(f.dlg->error_shown());
    CHECK(f.dlg->save_enabled());
}

TEST_CASE("PowerLevelDialog: creator: no same-level warning, any valid level allowed",
          "[dialog][power_level]")
{
    PowerLevelFixture f;
    f.open(0, kPowerLevelCreator);
    CHECK(f.option_values() == std::vector<std::string>{"0", "50", "100", "custom"});
    f.type_custom("1000");
    CHECK(f.dlg->save_enabled());
    CHECK_FALSE(f.dlg->same_level_warning_shown());
    CHECK_FALSE(f.dlg->error_shown());
}

TEST_CASE("PowerLevelDialog: Save fires on_save with the chosen level and closes",
          "[dialog][power_level]")
{
    PowerLevelFixture f;
    f.open(0, 100);
    f.select("50");
    REQUIRE(f.dlg->save_enabled());
    f.dlg->save_for_test();
    REQUIRE(f.saves.size() == 1);
    CHECK(f.saves[0].room_id == "!r:x");
    CHECK(f.saves[0].user_id == "@u:x");
    CHECK(f.saves[0].display_name == "U");
    CHECK(f.saves[0].level == 50);
    CHECK_FALSE(f.dlg->is_open());
    CHECK(f.closes == 1);
}

TEST_CASE("PowerLevelDialog: Save does nothing while disabled", "[dialog][power_level]")
{
    PowerLevelFixture f;
    f.open(50, 100);
    REQUIRE_FALSE(f.dlg->save_enabled());
    f.dlg->save_for_test();
    CHECK(f.saves.empty());
    CHECK(f.dlg->is_open());
}

TEST_CASE("PowerLevelDialog: Escape / Cancel closes without saving", "[dialog][power_level]")
{
    SECTION("Escape")
    {
        PowerLevelFixture f;
        f.open(0, 100);
        f.select("50");
        tk::KeyEvent esc;
        esc.key = tk::Key::Escape;
        CHECK(f.dlg->on_key_down(esc));
        CHECK_FALSE(f.dlg->is_open());
        CHECK(f.saves.empty());
        CHECK(f.closes == 1);
    }
    SECTION("Cancel")
    {
        PowerLevelFixture f;
        f.open(0, 100);
        f.select("50");
        REQUIRE(f.dlg->cancel_button());
        f.dlg->cancel_button()->click();
        CHECK_FALSE(f.dlg->is_open());
        CHECK(f.saves.empty());
        CHECK(f.closes == 1);
    }
}

TEST_CASE("PowerLevelDialog: is a modal dialog only while open", "[dialog][power_level]")
{
    PowerLevelFixture f;
    CHECK(f.dlg->access_role() == tk::Role::Dialog);
    CHECK_FALSE(f.dlg->access_modal());
    f.open(0, 100);
    CHECK(f.dlg->access_modal());
    CHECK(f.dlg->access_name() == "Change power level");
    f.dlg->close();
    CHECK_FALSE(f.dlg->access_modal());
}

TEST_CASE("PowerLevelDialog: lays out and paints without a provider", "[dialog][power_level]")
{
    PowerLevelFixture f;
    f.open(25, 50);
    f.type_custom("75");
    auto surface = TestSurface::create(800, 600);
    const tk::Theme theme = tk::Theme::light();
    tk::LayoutCtx lc{surface->factory(), theme};
    f.dlg->measure(lc, {800, 600});
    f.dlg->arrange(lc, {0, 0, 800, 600});
    tk::PaintCtx pc{surface->canvas(), surface->factory(), theme};
    f.dlg->paint(pc);
    CHECK(f.dlg->custom_field()->bounds().w > 0.0f);
    CHECK(f.dlg->level_combo()->bounds().w > 0.0f);
}

TEST_CASE("PowerLevelDialog: choosing Custom focuses the custom field", "[dialog][power_level]")
{
    PowerLevelFixture f;
    f.open(0, 100);
    REQUIRE_FALSE(f.dlg->custom_field()->visible());
    f.select("custom");
    CHECK(f.dlg->custom_field()->visible());
    CHECK(f.host.focused_widget() == f.dlg->custom_field());
}
