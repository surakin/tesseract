#include <catch2/catch_test_macros.hpp>

#include "view_test_util.h"
#include "views/QRGrantView.h"

#include <tesseract/client.h>

#include <functional>
#include <string>

using tesseract::views::QRGrantView;

namespace
{

struct QrStage : vt::Stage
{
    std::unique_ptr<QRGrantView> view = tk::create_root_widget<QRGrantView>(&host);
    tesseract::Client client; // never logged in: every grant step fails
    int relayouts = 0;
    QrStage()
    {
        view->set_post_to_ui([](std::function<void()> f) { f(); });
        view->set_run_async([](std::function<void()> f) { f(); });
        view->set_relayout([this] { ++relayouts; });
        view->set_client(&client);
        mount(*view, {0, 0, 600, 600});
    }
    void run() { relayout_paint(*view, {0, 0, 600, 600}); }
};

} // namespace

TEST_CASE("QRGrantView starts in Loading and shows no action buttons",
          "[tk][view][qr_grant]")
{
    QrStage s;
    CHECK_FALSE(vt::has_name(*s.view, "Confirm"));
    CHECK_FALSE(vt::has_name(*s.view, "Try again"));
    CHECK_FALSE(vt::has_name(*s.view, "Close"));
    CHECK(s.view->visible());
}

TEST_CASE("QRGrantView start without a client is a no-op", "[tk][view][qr_grant]")
{
    auto view = tk::create_root_widget<QRGrantView>(nullptr);
    view->start();
    view->shutdown();
    SUCCEED();
}

TEST_CASE("QRGrantView a failed grant surfaces the error with retry/close",
          "[tk][view][qr_grant]")
{
    QrStage s;
    int cancelled = 0;
    s.view->set_on_cancel([&] { ++cancelled; });
    s.view->start();
    s.run();
    CHECK(s.relayouts >= 2); // Loading, then Error
    REQUIRE(vt::has_name(*s.view, "Try again"));
    REQUIRE(vt::has_name(*s.view, "Close")); // Cancel relabelled to Close

    const int before = s.relayouts;
    REQUIRE(vt::press(*s.view, "Try again"));
    CHECK(s.relayouts > before);
    s.run();

    REQUIRE(vt::press(*s.view, "Close"));
    CHECK(cancelled == 1);
}

TEST_CASE("QRGrantView shutdown, theme change and visibility are safe",
          "[tk][view][qr_grant]")
{
    QrStage s;
    s.view->start();
    auto token = s.view->generation_token();
    s.view->on_theme_changed(tk::Theme::dark());
    s.view->set_visible(false);
    s.view->set_visible(true);
    s.view->shutdown();
    CHECK_FALSE(token.expired());
    s.run();
}
