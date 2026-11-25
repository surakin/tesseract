// The consent dialog (on ConfirmDialog): every way out is exactly one answer.

#include <catch2/catch_test_macros.hpp>

#include "tk/controls.h"
#include "tk/theme.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"
#include "views/ConfirmDialog.h"
#include "views/WorldConsentDialog.h"

#include <memory>
#include <vector>

using namespace tesseract::views;

namespace
{
struct Dlg
{
    StubHost host;
    std::unique_ptr<ConfirmDialog> dialog = tk::create_root_widget<ConfirmDialog>(&host);
    std::vector<ConsentChoice> answers;
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 600);

    void open()
    {
        open_world_consent_dialog(*dialog, {"Arena", "@alice:s", std::string(64, 'a')},
                                  [this](ConsentChoice c)
                                  {
                                      answers.push_back(c);
                                  });
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        dialog->measure(lc, {800, 600});
        dialog->arrange(lc, {0, 0, 800, 600});
    }
    tk::Button* button(const std::string& label)
    {
        for (const auto& c : dialog->children())
        {
            if (auto* b = dynamic_cast<tk::Button*>(c.get()); b && b->label() == label)
            {
                return b;
            }
        }
        return nullptr;
    }
};
} // namespace

TEST_CASE("world consent dialog: Run once answers RunOnce, exactly once", "[world][world consent]")
{
    Dlg d;
    d.open();
    d.dialog->confirm();
    CHECK(d.answers == std::vector<ConsentChoice>{ConsentChoice::RunOnce});
    d.dialog->close();
    CHECK(d.answers.size() == 1);
}

TEST_CASE("world consent dialog: the secondary button answers AlwaysRun", "[world][world consent]")
{
    Dlg d;
    d.open();
    auto* b = d.button("Always run this world");
    REQUIRE(b != nullptr);
    b->click();
    CHECK(d.answers == std::vector<ConsentChoice>{ConsentChoice::AlwaysRun});
    CHECK_FALSE(d.dialog->is_open());
}

TEST_CASE("world consent dialog: Cancel answers DontRun", "[world][world consent]")
{
    Dlg d;
    d.open();
    auto* b = d.button("Don't run");
    REQUIRE(b != nullptr);
    b->click();
    CHECK(d.answers == std::vector<ConsentChoice>{ConsentChoice::DontRun});
}

TEST_CASE("world consent dialog: Escape (close) and a backdrop click answer DontRun",
          "[world][world consent]")
{
    Dlg esc;
    esc.open();
    esc.dialog->close(); // what MainAppWidget does on Escape
    CHECK(esc.answers == std::vector<ConsentChoice>{ConsentChoice::DontRun});

    Dlg bd;
    bd.open();
    bd.dialog->on_pointer_down({5.0f, 5.0f});
    bd.dialog->on_pointer_up({5.0f, 5.0f}, true);
    CHECK(bd.answers == std::vector<ConsentChoice>{ConsentChoice::DontRun});
}

TEST_CASE("world consent dialog: a dialog that replaces it dismisses it", "[world][world consent]")
{
    Dlg d;
    d.open();
    ConfirmDialog::Options other;
    other.title = "Leave?";
    d.dialog->open(std::move(other), [] {});
    CHECK(d.answers == std::vector<ConsentChoice>{ConsentChoice::DontRun});
    d.dialog->confirm(); // the other dialog's confirm is not ours
    CHECK(d.answers.size() == 1);
}

TEST_CASE("ConfirmDialog: on_dismiss fires for cancel and close but not for confirm",
          "[world][world consent]")
{
    Dlg d;
    int dismissed = 0, confirmed = 0;
    ConfirmDialog::Options o;
    o.title = "T";
    o.on_dismiss = [&]
    {
        ++dismissed;
    };
    d.dialog->open(o,
                   [&]
                   {
                       ++confirmed;
                   });
    d.dialog->confirm();
    CHECK(confirmed == 1);
    CHECK(dismissed == 0);
    d.dialog->open(o,
                   [&]
                   {
                       ++confirmed;
                   });
    d.dialog->close();
    CHECK(dismissed == 1);
    d.dialog->close(); // already closed: no second call
    CHECK(dismissed == 1);
}
