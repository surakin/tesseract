#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/theme.h"
#include "views/EncryptionSetupOverlay.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

using namespace tk;
using tesseract::views::EncryptionSetupOverlay;

namespace
{

struct EncryptionSetupOverlayStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 600);
    LayoutCtx layout_ctx() { return {surface->factory(), Theme::light()}; }
    PaintCtx  paint_ctx()  { return {surface->canvas(), surface->factory(), Theme::light()}; }
    void run(Widget& root, Rect bounds)
    {
        auto lc = layout_ctx();
        root.measure(lc, {bounds.w, bounds.h});
        root.arrange(lc, bounds);
        auto pc = paint_ctx();
        root.paint(pc);
    }
};

// Mimics Qt's real QWidget::setVisible(false)-clears-focus-of-an-already-
// focused-widget semantics that StubTextField (tk_test_host.h) deliberately
// does not model, so the regression test below can reproduce the actual
// bug class: EncryptionSetupOverlay::paint() used to unconditionally hide
// both native fields at the top of every paint pass and reshow only the
// active one, which — on a real native backend — silently dropped
// keyboard focus on the hide and never got it back on the reshow.
struct EncSetupFocusClearingNative : public tk::NativeTextField
{
    void set_rect(tk::Rect) override {}
    void set_text(std::string t) override { text_ = std::move(t); }
    std::string text() const override { return text_; }
    void set_placeholder(std::string) override {}
    void set_enabled(bool) override {}
    void set_password(bool) override {}
    void set_on_changed(std::function<void(const std::string&)>) override {}
    void set_on_submit(std::function<void()>) override {}
    void set_focused(bool f) override { focused_ = f; }
    void set_visible(bool v) override
    {
        if (!v && focused_ && on_focus_changed)
        {
            focused_ = false;
            on_focus_changed(false); // synchronous, like Qt's clearFocus()
        }
        visible_ = v;
    }
    void set_on_focus_changed(std::function<void(bool)> f) override
    {
        on_focus_changed = std::move(f);
    }

    std::string text_;
    bool visible_ = true;
    bool focused_ = false;
    std::function<void(bool)> on_focus_changed;
};

struct EncSetupFocusClearingHost : public TestHost
{
    EncSetupFocusClearingHost() : TestHost(nullptr) {}

    std::unique_ptr<tk::NativeTextField> make_text_field() override
    {
        auto f = std::make_unique<EncSetupFocusClearingNative>();
        fields_created.push_back(f.get()); // borrowed, owned by the TextField
        return f;
    }

    std::vector<EncSetupFocusClearingNative*> fields_created;
};

} // namespace

// ── Fresh mode ──────────────────────────────────────────────────────────────

TEST_CASE("Fresh: starts in Intro step", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Intro);
}

TEST_CASE("Fresh: Intro → ChooseMethod on primary action", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::ChooseMethod);
}

TEST_CASE("Fresh: Intro Skip fires on_close", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    bool closed = false;
    ov->on_close = [&]() { closed = true; };
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_skip();
    CHECK(closed);
}

TEST_CASE("Fresh: ChooseMethod Continue fires on_enable_recovery (key mode)",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action(); // → ChooseMethod
    std::string fired_passphrase = "sentinel";
    ov->on_enable_recovery = [&](std::string p) { fired_passphrase = p; };
    ov->simulate_primary_action(); // Continue in key mode
    CHECK(fired_passphrase.empty()); // empty passphrase → key mode
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Progress);
}

TEST_CASE("Fresh: ChooseMethod Continue fires on_enable_recovery (passphrase mode)",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    StubHost host;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(&host, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action(); // → ChooseMethod
    ov->simulate_select_passphrase_mode();
    REQUIRE(ov->passphrase_field() != nullptr);
    ov->passphrase_field()->set_text("s3cr3t");
    std::string fired_passphrase;
    ov->on_enable_recovery = [&](std::string p) { fired_passphrase = p; };
    ov->simulate_primary_action();
    CHECK(fired_passphrase == "s3cr3t");
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Progress);
}

TEST_CASE("Fresh: advance_progress Done → ShowKey with key string",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action(); // → ChooseMethod
    ov->simulate_primary_action(); // → Progress
    ov->advance_progress(4, "AAAA-BBBB-CCCC", 0, 0); // Done
    CHECK(ov->step() == EncryptionSetupOverlay::Step::ShowKey);
    CHECK(ov->recovery_key() == "AAAA-BBBB-CCCC");
}

TEST_CASE("Fresh: ShowKey Continue disabled until checkbox checked",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action();
    ov->simulate_primary_action();
    ov->advance_progress(4, "KEY", 0, 0);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::ShowKey);
    // Continue without checking box → stays on ShowKey
    ov->simulate_primary_action();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::ShowKey);
    // Check box → Continue enabled
    ov->simulate_check_key_saved();
    ov->simulate_primary_action();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Done);
}

TEST_CASE("Fresh: passphrase mode skips ShowKey → Done directly",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action();
    ov->simulate_select_passphrase_mode();
    ov->simulate_primary_action();
    ov->advance_progress(4, "", 0, 0); // Done, empty key → passphrase mode
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Done);
}

TEST_CASE("Fresh: Progress step==5 (error) returns to ChooseMethod with message",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action();
    ov->simulate_primary_action();
    ov->advance_progress(5, "network error", 0, 0);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::ChooseMethod);
    CHECK(!ov->error_msg().empty());
}

// ── Recover mode ─────────────────────────────────────────────────────────────

TEST_CASE("Recover: starts in Intro step", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Intro);
}

TEST_CASE("Recover: Intro → EnterKey on primary action", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::EnterKey);
}

TEST_CASE("Recover: EnterKey Verify fires on_recover with key",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    StubHost host;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(&host, EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action(); // → EnterKey
    REQUIRE(ov->key_field() != nullptr);
    ov->key_field()->set_text("my-recovery-key");
    std::string fired_key;
    ov->on_recover = [&](std::string k) { fired_key = k; };
    ov->simulate_primary_action(); // Verify
    CHECK(fired_key == "my-recovery-key");
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Progress);
}

TEST_CASE("Recover: key_field keeps host-level focus across a repeated "
          "relayout on the same step",
          "[encryption][overlay][focus]")
{
    // Regression test: EncryptionSetupOverlay::paint() used to unconditionally
    // hide() both native fields at the top of every paint pass and reshow only
    // the active one — a genuine hide-then-reshow round trip within a single
    // frame for whichever field stays active, which (on a real native
    // backend, e.g. Qt) silently drops keyboard focus on the hide and never
    // restores it. paint() must now only hide the field that ISN'T staying
    // active this step.
    EncryptionSetupOverlayStage st;
    EncSetupFocusClearingHost host;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(&host, EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action(); // → EnterKey
    st.run(*ov, {0, 0, 800, 600});

    REQUIRE(ov->key_field() != nullptr);
    REQUIRE(host.fields_created.size() == 2); // passphrase_field_, then key_field_
    auto* key_native = host.fields_created[1];

    // Simulate the native control gaining real OS focus directly (a click
    // bypasses canvas hit-testing entirely).
    key_native->focused_ = true;
    key_native->on_focus_changed(true);
    REQUIRE(host.focused_widget() == ov->key_field());

    // A second relayout/repaint on the same step (e.g. a resize, or any
    // unrelated repaint) must not disturb focus.
    st.run(*ov, {0, 0, 800, 600});
    CHECK(host.focused_widget() == ov->key_field());
}

TEST_CASE("Recover: 'use another device' fires on_request_sas",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action(); // → EnterKey
    bool sas_fired = false;
    ov->on_request_sas = [&]() { sas_fired = true; };
    ov->simulate_sas_link();
    CHECK(sas_fired);
}

TEST_CASE("Recover: Progress Done → Done step", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action();
    ov->simulate_primary_action();
    ov->advance_progress(4, "", 0, 0);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Done);
}

TEST_CASE("Recover: Progress error returns to EnterKey with message",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action();
    ov->simulate_primary_action();
    ov->advance_progress(5, "bad key", 0, 0);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::EnterKey);
    CHECK(!ov->error_msg().empty());
}

// ── Progress step labels ──────────────────────────────────────────────────────

TEST_CASE("advance_progress updates status label string", "[encryption][overlay]")
{
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    ov->simulate_primary_action();
    ov->simulate_primary_action();
    ov->advance_progress(1, "", 0, 0); // CreatingBackup
    {
        auto lbl = ov->progress_label();
        bool has_backup = lbl.find("backup") != std::string::npos
                       || lbl.find("Backup") != std::string::npos;
        CHECK(has_backup);
    }
    ov->advance_progress(3, "", 42, 100); // BackingUp
    CHECK(ov->progress_label().find("42") != std::string::npos);
}

// ── Done → close ─────────────────────────────────────────────────────────────

TEST_CASE("Done: close button fires on_close", "[encryption][overlay]")
{
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    ov->simulate_primary_action();
    ov->simulate_primary_action();
    ov->advance_progress(4, "KEY", 0, 0);
    ov->simulate_check_key_saved();
    ov->simulate_primary_action();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Done);
    bool closed = false;
    ov->on_close = [&]() { closed = true; };
    ov->simulate_primary_action(); // Close
    CHECK(closed);
}

// ── Cross-signing reset flow ─────────────────────────────────────────────────

TEST_CASE("Reset: begin_reset_wait enters ResetApproving", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->begin_reset_wait();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::ResetApproving);
}

TEST_CASE("Reset: approval hands off to the Fresh recovery-key flow",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->begin_reset_wait();
    ov->reset_approved();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::ChooseMethod);
}

TEST_CASE("Reset: Cancel on ResetApproving fires on_cancel_reset",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    bool cancelled = false;
    ov->on_cancel_reset = [&]() { cancelled = true; };
    st.run(*ov, {0, 0, 800, 600});
    ov->begin_reset_wait();
    ov->simulate_primary_action(); // Cancel
    CHECK(cancelled);
}

TEST_CASE("Reset: report_reset_error stays on ResetApproving with a message",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->begin_reset_wait();
    ov->report_reset_error("approval timed out");
    CHECK(ov->step() == EncryptionSetupOverlay::Step::ResetApproving);
    CHECK(ov->error_msg() == "approval timed out");
}
