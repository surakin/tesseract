#include <catch2/catch_test_macros.hpp>

#include "tk/controls.h"
#include "tk/theme.h"
#include "tk/video_capture.h"
#include "tk_test_surface.h"
#include "views/CallLobbyView.h"
#include "views/CameraWidget.h"

#include <memory>
#include <vector>

using tesseract::views::CallLobbyView;
using tesseract::views::CameraWidget;
using Error = tk::VideoCapture::Error;

namespace
{

// Scriptable capture: fails synchronously in start() when start_error is
// set, or later (as a backend thread would) via fail().
class FakeVideoCapture : public tk::VideoCapture
{
public:
    Error start_error = Error::None;
    bool  running     = false;

    void start() override
    {
        clear_error_();
        if (start_error != Error::None)
        {
            report_error_(start_error);
            return;
        }
        running = true;
    }
    void stop() override { running = false; }
    void set_callback(FrameCallback) override {}
    void fail(Error e) { report_error_(e); }
};

// Installs a factory handing out FakeVideoCaptures (or nullptr when
// no_device is set) and records every one created; restores the real
// factory on scope exit.
struct FakeFactory
{
    std::vector<FakeVideoCapture*> created;
    Error next_start_error = Error::None;
    bool  no_device        = false;

    FakeFactory()
    {
        tk::VideoCapture::set_factory_for_testing(
            [this]() -> std::unique_ptr<tk::VideoCapture>
            {
                if (no_device)
                    return nullptr;
                auto c = std::make_unique<FakeVideoCapture>();
                c->start_error = next_start_error;
                created.push_back(c.get());
                return c;
            });
    }
    ~FakeFactory() { tk::VideoCapture::set_factory_for_testing({}); }
};

struct Stage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(640, 480);

    void layout_and_paint(tk::Widget& w)
    {
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        w.measure(lc, {640, 480});
        w.arrange(lc, {0, 0, 640, 480});
        tk::PaintCtx pc{surface->canvas(), surface->factory(), tk::Theme::light()};
        w.paint(pc);
    }
};

} // namespace

TEST_CASE("VideoCapture reports only the first error after start", "[video_capture]")
{
    FakeVideoCapture cap;
    std::vector<Error> seen;
    cap.set_error_callback([&](Error e) { seen.push_back(e); });

    cap.start();
    REQUIRE(cap.error() == Error::None);

    cap.fail(Error::Busy);
    cap.fail(Error::Failed); // e.g. the state-change failure after a bus error
    REQUIRE(cap.error() == Error::Busy);
    REQUIRE(seen == std::vector<Error>{Error::Busy});

    // A restart clears the latch so the next failure is reported again.
    cap.start();
    REQUIRE(cap.error() == Error::None);
    cap.fail(Error::NoDevice);
    REQUIRE(seen == std::vector<Error>{Error::Busy, Error::NoDevice});
}

TEST_CASE("VideoCapture::describe has a message for every failure", "[video_capture]")
{
    REQUIRE(tk::VideoCapture::describe(Error::None).empty());
    for (Error e : {Error::NoDevice, Error::Busy, Error::PermissionDenied, Error::Failed})
        REQUIRE_FALSE(tk::VideoCapture::describe(e).empty());
    REQUIRE(tk::VideoCapture::describe(Error::Busy) !=
            tk::VideoCapture::describe(Error::NoDevice));
}

TEST_CASE("call lobby turns the camera off and explains a busy camera", "[call_lobby]")
{
    FakeFactory factory;
    Stage stage;
    CallLobbyView lobby;

    lobby.open("!room:x", "call#default", /*default_audio_only=*/false);
    REQUIRE(lobby.camera_enabled());
    REQUIRE(factory.created.size() == 1);

    // Failure arrives later from the capture thread; the next paint sees it.
    factory.created[0]->fail(Error::Busy);
    stage.layout_and_paint(lobby);

    REQUIRE_FALSE(lobby.camera_enabled());
    REQUIRE(lobby.camera_error() == Error::Busy);
}

TEST_CASE("call lobby retries with a fresh capture when the camera is re-enabled",
          "[call_lobby]")
{
    FakeFactory factory;
    factory.next_start_error = Error::Busy;
    Stage stage;
    CallLobbyView lobby;

    lobby.open("!room:x", "call#default", false);
    // Synchronous start() failure is picked up without waiting for a paint.
    REQUIRE(lobby.camera_error() == Error::Busy);
    REQUIRE_FALSE(lobby.camera_enabled());

    // The other app let go; toggling the camera on tries again.
    factory.next_start_error = Error::None;
    stage.layout_and_paint(lobby);
    // Mic is the first Button child, camera the second.
    tk::Button* cam_btn = nullptr;
    int buttons = 0;
    for (const auto& ch : lobby.children())
        if (auto* b = dynamic_cast<tk::Button*>(ch.get()); b && ++buttons == 2)
            cam_btn = b;
    REQUIRE(cam_btn);
    cam_btn->click();

    REQUIRE(factory.created.size() == 2);
    REQUIRE(factory.created[1]->running);
    REQUIRE(lobby.camera_enabled());
    REQUIRE(lobby.camera_error() == Error::None);
}

TEST_CASE("call lobby with a failed camera joins with video muted, not audio-only",
          "[call_lobby]")
{
    FakeFactory factory;
    factory.no_device = true;
    CallLobbyView lobby;
    bool audio_only  = true;
    bool video_muted = false;
    lobby.on_join = [&](const std::string&, const std::string&, bool ao, bool,
                        bool vm)
    {
        audio_only  = ao;
        video_muted = vm;
    };

    lobby.open("!room:x", "call#default", false);
    REQUIRE(lobby.camera_error() == Error::NoDevice);
    REQUIRE_FALSE(lobby.camera_enabled());

    // Join button is the third Button child.
    int buttons = 0;
    for (const auto& ch : lobby.children())
        if (auto* b = dynamic_cast<tk::Button*>(ch.get()); b && ++buttons == 3)
            b->click();
    // Video stays available so the user can retry once the camera is free.
    REQUIRE_FALSE(audio_only);
    REQUIRE(video_muted);
}

TEST_CASE("selfie overlay shows camera errors instead of vanishing", "[camera_widget]")
{
    FakeFactory factory;
    Stage stage;

    SECTION("no camera")
    {
        factory.no_device = true;
        CameraWidget w;
        int dismissed = 0;
        w.on_dismissed = [&] { ++dismissed; };
        w.open();
        stage.layout_and_paint(w);
        REQUIRE(w.error() == Error::NoDevice);
        REQUIRE(dismissed == 0); // stays up so the reason is readable
        w.on_pointer_down({10, 10});
        REQUIRE(dismissed == 1);
    }

    SECTION("camera fails after starting")
    {
        CameraWidget w;
        w.open();
        REQUIRE(factory.created.size() == 1);
        REQUIRE(factory.created[0]->running);
        factory.created[0]->fail(Error::PermissionDenied);
        stage.layout_and_paint(w);
        REQUIRE(w.error() == Error::PermissionDenied);
    }
}
