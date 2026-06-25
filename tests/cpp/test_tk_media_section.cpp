#include <catch2/catch_test_macros.hpp>

#include "views/settings/MediaSection.h"
#include "tk_test_surface.h"

#include <string>
#include <vector>

using tesseract::views::MediaSection;

namespace
{

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 800);
    void run(tk::Widget& root)
    {
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        root.measure(lc, {640.0f, 800.0f});
        root.arrange(lc, {0.0f, 0.0f, 640.0f, 800.0f});
        tk::PaintCtx pc{surface->canvas(), surface->factory(),
                        tk::Theme::light()};
        root.paint(pc);
    }
};

} // namespace

TEST_CASE("MediaSection: paints without crash with no devices set",
          "[media_section]")
{
    Stage st;
    MediaSection sec;
    st.run(sec);
}

TEST_CASE("MediaSection: set_audio_input_devices does not fire callback",
          "[media_section]")
{
    MediaSection sec;
    int fires = 0;
    sec.on_audio_input_changed = [&](std::string) { ++fires; };
    sec.set_audio_input_devices({{"hw:0", "Built-in Mic"}, {"hw:1", "USB Mic"}});
    REQUIRE(fires == 0);
}

TEST_CASE("MediaSection: set_selected_audio_input does not fire callback",
          "[media_section]")
{
    MediaSection sec;
    sec.set_audio_input_devices({{"hw:0", "Built-in Mic"}});
    int fires = 0;
    sec.on_audio_input_changed = [&](std::string) { ++fires; };
    sec.set_selected_audio_input("hw:0");
    REQUIRE(fires == 0);
}

TEST_CASE("MediaSection: on_audio_input_changed fires when combo changes",
          "[media_section]")
{
    MediaSection sec;
    sec.set_audio_input_devices({{"hw:0", "Built-in Mic"}, {"hw:1", "USB Mic"}});
    std::string captured;
    sec.on_audio_input_changed = [&](std::string id) { captured = id; };
    sec.audio_input_combo()->on_changed("hw:1");
    REQUIRE(captured == "hw:1");
}

TEST_CASE("MediaSection: on_audio_output_changed fires when combo changes",
          "[media_section]")
{
    MediaSection sec;
    sec.set_audio_output_devices({{"spkr:0", "Built-in Speaker"}});
    std::string captured;
    sec.on_audio_output_changed = [&](std::string id) { captured = id; };
    sec.audio_output_combo()->on_changed("spkr:0");
    REQUIRE(captured == "spkr:0");
}

TEST_CASE("MediaSection: on_camera_changed fires when combo changes",
          "[media_section]")
{
    MediaSection sec;
    sec.set_camera_devices({{"/dev/video0", "Integrated Camera"}});
    std::string captured;
    sec.on_camera_changed = [&](std::string id) { captured = id; };
    sec.camera_combo()->on_changed("/dev/video0");
    REQUIRE(captured == "/dev/video0");
}

TEST_CASE("MediaSection: empty-string selection means system default",
          "[media_section]")
{
    MediaSection sec;
    sec.set_audio_input_devices({{"hw:0", "Built-in Mic"}});
    std::string captured = "sentinel";
    sec.on_audio_input_changed = [&](std::string id) { captured = id; };
    sec.audio_input_combo()->on_changed("");
    REQUIRE(captured.empty());
}
