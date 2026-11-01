#include <catch2/catch_test_macros.hpp>

#include "views/settings/MediaSection.h"
#include "views/settings/SettingBinder.h"
#include "tk_test_surface.h"

#include <string>
#include <vector>

using tesseract::views::MediaSection;
using tesseract::views::SettingBinder;
using tesseract::views::SettingKey;

namespace
{

struct TkMediaSectionStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 800);
    void run(tk::Widget& root)
    {
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        root.measure(lc, {640.0f, 800.0f});
        root.arrange(lc, {0.0f, 0.0f, 640.0f, 800.0f});
        tk::PaintCtx pc{surface->canvas(), surface->factory(), tk::Theme::light()};
        root.paint(pc);
    }
};

// Section + binder + a record of what was reported. Restores the three
// device-id Settings fields the tests touch.
struct MediaFixture
{
    tesseract::Settings& s = tesseract::Settings::instance();
    std::string in = s.audio_input_device_id, out = s.audio_output_device_id,
                cam = s.camera_device_id;
    SettingBinder binder;
    int fires = 0;
    SettingKey last{};
    MediaSection sec{binder};
    MediaFixture()
    {
        binder.on_changed = [this](SettingKey k)
        {
            ++fires;
            last = k;
        };
    }
    ~MediaFixture()
    {
        s.audio_input_device_id = in;
        s.audio_output_device_id = out;
        s.camera_device_id = cam;
    }
};

} // namespace

TEST_CASE("MediaSection: paints without crash with no devices set", "[media_section]")
{
    TkMediaSectionStage st;
    MediaFixture f;
    st.run(f.sec);
}

TEST_CASE("MediaSection: set_audio_input_devices does not report a change", "[media_section]")
{
    MediaFixture f;
    f.sec.set_audio_input_devices({{"hw:0", "Built-in Mic"}, {"hw:1", "USB Mic"}});
    REQUIRE(f.fires == 0);
}

TEST_CASE("MediaSection: set_selected_audio_input does not report a change", "[media_section]")
{
    MediaFixture f;
    f.sec.set_audio_input_devices({{"hw:0", "Built-in Mic"}});
    f.sec.set_selected_audio_input("hw:0");
    REQUIRE(f.fires == 0);
}

TEST_CASE("MediaSection: picking a microphone stores the id and reports AudioInput",
          "[media_section]")
{
    MediaFixture f;
    f.s.audio_input_device_id.clear();
    f.sec.set_audio_input_devices({{"hw:0", "Built-in Mic"}, {"hw:1", "USB Mic"}});
    f.sec.audio_input_combo()->on_changed("hw:1");
    REQUIRE(f.s.audio_input_device_id == "hw:1");
    REQUIRE(f.last == SettingKey::AudioInput);
}

TEST_CASE("MediaSection: picking a speaker stores the id and reports AudioOutput",
          "[media_section]")
{
    MediaFixture f;
    f.s.audio_output_device_id.clear();
    f.sec.set_audio_output_devices({{"spkr:0", "Built-in Speaker"}});
    f.sec.audio_output_combo()->on_changed("spkr:0");
    REQUIRE(f.s.audio_output_device_id == "spkr:0");
    REQUIRE(f.last == SettingKey::AudioOutput);
}

TEST_CASE("MediaSection: picking a camera stores the id and reports Camera", "[media_section]")
{
    MediaFixture f;
    f.s.camera_device_id.clear();
    f.sec.set_camera_devices({{"/dev/video0", "Integrated Camera"}});
    f.sec.camera_combo()->on_changed("/dev/video0");
    REQUIRE(f.s.camera_device_id == "/dev/video0");
    REQUIRE(f.last == SettingKey::Camera);
}

TEST_CASE("MediaSection: empty-string selection means system default", "[media_section]")
{
    MediaFixture f;
    f.s.audio_input_device_id = "hw:0";
    f.sec.set_audio_input_devices({{"hw:0", "Built-in Mic"}});
    f.sec.audio_input_combo()->on_changed("");
    REQUIRE(f.s.audio_input_device_id.empty());
    REQUIRE(f.fires == 1);
}
