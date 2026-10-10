// ComposerPopups (ui/shared/app/ComposerPopups.cpp): owns the four composer
// autocomplete popups of a pop-out window and routes the compose TextArea's
// change / submit / navigation events to them. Driven through a stub host that
// hands out recordable popup surfaces.

#include <catch2/catch_test_macros.hpp>

#include "app/ComposerPopups.h"
#include "app/RoomPane.h"
#include "app/ShellBase.h"
#include "shell_test_double.h"
#include "tk/text_area.h"
#include "tk/theme.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <tesseract/client.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace
{

struct PopupsShell : tesseract::test::TestShellBase
{
    ~PopupsShell() override
    {
        pool_.drain();
        mut_pool_.drain();
        media_prefetch_pool_.drain();
    }
    void apply_thread_messages_(
        const std::string&, std::vector<tesseract::views::MessageRowData>,
        bool) override {}
    void apply_thread_message_insert_(
        const std::string&, std::size_t,
        tesseract::views::MessageRowData) override {}
    void apply_thread_message_remove_(const std::string&,
                                      std::size_t) override {}
    using ShellBase::client_;
};

// Native composer stand-in whose caret follows the text.
struct PopupsArea : StubTextArea
{
    int cursor_byte_pos() const override
    {
        return static_cast<int>(text_.size());
    }
    void set_on_submit(std::function<void()> f) override
    {
        on_submit = std::move(f);
    }
    std::function<void()> on_submit;
};

struct PopupsHost : PopupCapableStubHost
{
    std::unique_ptr<tk::NativeTextArea> make_text_area() override
    {
        auto a = std::make_unique<PopupsArea>();
        native = a.get();
        return a;
    }
    PopupsArea* native = nullptr;
};

struct PopupsRig
{
    PopupsShell shell;
    tesseract::Client client;
    std::unique_ptr<TestSurface> surface = TestSurface::create(320, 200);
    PopupsHost host;
    std::unique_ptr<tk::TextArea> area =
        tk::create_root_widget<tk::TextArea>(&host, 40.0f);
    std::unique_ptr<tesseract::RoomPane> pane;
    std::unique_ptr<tesseract::ComposerPopups> popups;

    // Order in which ComposerPopups creates its surfaces.
    enum
    {
        Mention = 0,
        Slash = 1,
        Shortcode = 2,
        Gif = 3
    };

    PopupsRig()
    {
        shell.client_ = &client;
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        area->measure(lc, {200, 40});
        area->arrange(lc, {0, 0, 200, 40});
        REQUIRE(host.native != nullptr);
        pane = std::make_unique<tesseract::RoomPane>(
            tesseract::RoomPane::Deps{
                .shell = &shell, .repaint = [] {}, .relayout = [] {}},
            "!room:x");
        popups = std::make_unique<tesseract::ComposerPopups>(
            host, area.get(), pane.get(), nullptr);
        REQUIRE(host.popups_created.size() == 4);
    }

    // Simulate the user typing `t` (native change event).
    void type(const std::string& t)
    {
        host.native->text_ = t;
        host.native->on_changed(t);
    }
    bool popup_visible(int i) const { return host.popups_created[i]->visible(); }
};

} // namespace

TEST_CASE("ComposerPopups creates all four popups hidden", "[composer_popups]")
{
    PopupsRig r;
    for (int i = 0; i < 4; ++i)
    {
        CHECK_FALSE(r.popup_visible(i));
        CHECK(r.host.popups_created[i]->root() != nullptr);
    }
}

TEST_CASE("typing a slash command opens the command popup and hide_all closes it",
          "[composer_popups]")
{
    PopupsRig r;
    r.type("/po");
    CHECK(r.popup_visible(PopupsRig::Slash));
    CHECK_FALSE(r.popup_visible(PopupsRig::Mention));
    r.popups->hide_all();
    CHECK_FALSE(r.popup_visible(PopupsRig::Slash));
}

TEST_CASE("typing a :shortcode opens the emoji popup", "[composer_popups]")
{
    PopupsRig r;
    r.type(":smi");
    CHECK(r.popup_visible(PopupsRig::Shortcode));
    r.type("plain");
    CHECK_FALSE(r.popup_visible(PopupsRig::Shortcode));
}

TEST_CASE("Escape closes an open popup; with none open the key is not consumed",
          "[composer_popups]")
{
    PopupsRig r;
    REQUIRE(r.host.native->on_popup_nav);
    CHECK_FALSE(r.host.native->on_popup_nav(tk::NavKey::Escape));
    r.type("/po");
    REQUIRE(r.popup_visible(PopupsRig::Slash));
    CHECK(r.host.native->on_popup_nav(tk::NavKey::Escape));
    CHECK_FALSE(r.popup_visible(PopupsRig::Slash));
}

TEST_CASE("an outside click dismisses the slash popup", "[composer_popups]")
{
    PopupsRig r;
    r.type("/po");
    REQUIRE(r.popup_visible(PopupsRig::Slash));
    auto* surf = r.host.popups_created[PopupsRig::Slash];
    REQUIRE(surf->on_dismiss_requested);
    surf->on_dismiss_requested();
    CHECK_FALSE(r.popup_visible(PopupsRig::Slash));
}

TEST_CASE("submit with no popup open and no room view is a no-op",
          "[composer_popups]")
{
    PopupsRig r;
    REQUIRE(r.area != nullptr);
    CHECK_NOTHROW(r.host.native->on_submit
                      ? r.host.native->on_submit()
                      : void());
}

TEST_CASE("gif results and failures for unknown requests are ignored",
          "[composer_popups]")
{
    PopupsRig r;
    r.popups->on_gif_results(123456789, {});
    r.popups->on_gif_search_failed(123456789, "x");
    CHECK_FALSE(r.popup_visible(PopupsRig::Gif));
    // Hiding / showing the strip without a room view must not open it.
    r.popups->show_gif_popup();
    CHECK_FALSE(r.popup_visible(PopupsRig::Gif));
    r.popups->hide_gif_popup();
}

TEST_CASE("theme and animation refresh reach the popups safely",
          "[composer_popups]")
{
    PopupsRig r;
    CHECK_NOTHROW(r.popups->apply_theme(tk::Theme::dark()));
    CHECK_NOTHROW(r.popups->repaint_anim_frame());
}

TEST_CASE("destroying ComposerPopups unhooks the composer", "[composer_popups]")
{
    PopupsRig r;
    r.type("hello"); // typing notice goes out (no-op on a session-less client)
    r.popups.reset();
    // The text area's change / submit hooks and nav handler were removed.
    CHECK_FALSE(r.host.native->on_changed);
    CHECK_FALSE(r.host.native->on_submit);
}
