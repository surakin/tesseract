#include <catch2/catch_test_macros.hpp>

#include "tk/text_area.h"
#include "tk_test_host.h"
#include "views/SlashCommandController.h"

#include <string>
#include <vector>

using namespace tesseract::views;

namespace
{

struct Rig
{
    TestHost host{nullptr};
    std::unique_ptr<tk::TextArea> area =
        tk::create_root_widget<tk::TextArea>(&host, 40.0f);
    SlashCommandPopup popup;
    std::vector<std::string> sent;
    std::vector<std::string> failures;
    int selfie = 0, location = 0, cleared = 0;
    tesseract::Result send_result{true, ""};
    std::unique_ptr<SlashCommandController> ctl;

    void build(bool with_send_command)
    {
        SlashCommandController::Hooks h;
        h.show = [](tk::Rect, int) {};
        h.hide = [] {};
        h.repaint = [] {};
        h.room_id = [] { return std::string{"!r:x"}; };
        h.clear_composer = [this] { ++cleared; };
        h.on_selfie = [this] { ++selfie; };
        h.on_location = [this] { ++location; };
        h.on_command_failed = [this](std::string e) { failures.push_back(e); };
        if (with_send_command)
            h.send_command = [this](const std::string& b)
            {
                sent.push_back(b);
                return send_result;
            };
        ctl = std::make_unique<SlashCommandController>(area.get(), &popup,
                                                       std::move(h));
    }
    bool type_and_submit(const std::string& text)
    {
        area->set_text(text);
        REQUIRE(ctl->on_text_changed(text, static_cast<int>(text.size())));
        return ctl->on_submit();
    }
};

} // namespace

TEST_CASE("SlashCommandController: no-arg builtins go through send_command",
          "[slash_command_controller]")
{
    for (const char* name : {"poll", "leave", "shrug"})
    {
        Rig r;
        r.build(true);
        CHECK(r.type_and_submit(std::string("/") + name));
        REQUIRE(r.sent.size() == 1);
        CHECK(r.sent[0] == std::string("/") + name);
        CHECK(r.area->text().empty());
        CHECK(r.cleared == 1);
    }
}

TEST_CASE("SlashCommandController: selfie and location keep their own hooks",
          "[slash_command_controller]")
{
    Rig r;
    r.build(true);
    CHECK(r.type_and_submit("/selfie"));
    CHECK(r.selfie == 1);
    CHECK(r.type_and_submit("/location"));
    CHECK(r.location == 1);
    CHECK(r.sent.empty());
}

TEST_CASE("SlashCommandController: failed send_command reports the error",
          "[slash_command_controller]")
{
    Rig r;
    r.send_result = tesseract::Result{false, "nope"};
    r.build(true);
    r.type_and_submit("/leave");
    REQUIRE(r.failures.size() == 1);
    CHECK(r.failures[0] == "nope");
}

TEST_CASE("SlashCommandController: without send_command the old path is a no-op "
          "when no client is available",
          "[slash_command_controller]")
{
    Rig r;
    r.build(false);
    CHECK(r.type_and_submit("/poll"));
    CHECK(r.sent.empty());
    CHECK(r.failures.empty());
}
