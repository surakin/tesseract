#include <catch2/catch_test_macros.hpp>

#include "tk/canvas.h"
#include "tk/controls.h"
#include "tk/theme.h"
#include "views/EncryptionSetupOverlay.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>
#include <vector>

using namespace tk;
using tesseract::views::EncryptionSetupOverlay;

namespace
{

struct EncryptionSetupOverlayStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 600);
    LayoutCtx layout_ctx() const
    {
        return {surface->factory(), Theme::light()};
    }
    PaintCtx paint_ctx() const
    {
        return {surface->canvas(), surface->factory(), Theme::light()};
    }
    void run(Widget& root, Rect bounds) const
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
    void set_rect(tk::Rect) override
    {
    }
    void set_text(std::string t) override
    {
        text_ = std::move(t);
    }
    std::string text() const override
    {
        return text_;
    }
    void set_placeholder(std::string) override
    {
    }
    void set_enabled(bool) override
    {
    }
    void set_password(bool) override
    {
    }
    void set_on_changed(std::function<void(const std::string&)>) override
    {
    }
    void set_on_submit(std::function<void()>) override
    {
    }
    void set_focused(bool f) override
    {
        focused_ = f;
    }
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
    EncSetupFocusClearingHost() : TestHost(nullptr)
    {
    }

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
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Intro);
}

TEST_CASE("Fresh: Intro primary generates a recovery key straight away", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    std::string fired_passphrase = "sentinel";
    ov->on_enable_recovery = [&](std::string p)
    {
        fired_passphrase = p;
    };
    ov->simulate_primary_action();
    CHECK(fired_passphrase.empty()); // empty passphrase → generated key
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Progress);
}

TEST_CASE("Fresh: Intro Skip fires on_close", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    bool closed = false;
    ov->on_close = [&]()
    {
        closed = true;
    };
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_skip();
    CHECK(closed);
}

TEST_CASE("Fresh: passphrase link → PassphraseEntry, Back → Intro", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_select_passphrase_mode();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::PassphraseEntry);
    ov->simulate_back();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Intro);
}

TEST_CASE("Fresh: PassphraseEntry Continue fires on_enable_recovery once both "
          "fields match",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    StubHost host;
    auto ov =
        tk::create_root_widget<EncryptionSetupOverlay>(&host, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_select_passphrase_mode();
    REQUIRE(ov->passphrase_field() != nullptr);
    REQUIRE(ov->passphrase_confirm_field() != nullptr);
    std::string fired_passphrase;
    bool fired = false;
    ov->on_enable_recovery = [&](std::string p)
    {
        fired = true;
        fired_passphrase = p;
    };

    // Mismatch: Continue is disabled and does nothing.
    ov->passphrase_field()->set_text("s3cr3t-passphrase");
    ov->passphrase_confirm_field()->set_text("s3cr3t-passphrasX");
    st.run(*ov, {0, 0, 800, 600});
    CHECK_FALSE(ov->primary_enabled());
    ov->simulate_primary_action();
    CHECK_FALSE(fired);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::PassphraseEntry);

    // Match: Continue fires with the passphrase.
    ov->passphrase_confirm_field()->set_text("s3cr3t-passphrase");
    st.run(*ov, {0, 0, 800, 600});
    CHECK(ov->primary_enabled());
    ov->simulate_primary_action();
    CHECK(fired_passphrase == "s3cr3t-passphrase");
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Progress);
}

TEST_CASE("Fresh: a short recovery passphrase is refused, not sent", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    StubHost host;
    auto ov =
        tk::create_root_widget<EncryptionSetupOverlay>(&host, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_select_passphrase_mode();
    REQUIRE(ov->passphrase_field() != nullptr);
    REQUIRE(ov->passphrase_confirm_field() != nullptr);
    bool fired = false;
    ov->on_enable_recovery = [&](std::string)
    {
        fired = true;
    };

    // Both fields match, but the passphrase is one character: it must not be sent.
    ov->passphrase_field()->set_text("x");
    ov->passphrase_confirm_field()->set_text("x");
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action();
    CHECK_FALSE(fired);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::PassphraseEntry);
    CHECK(ov->error_msg().find("12") != std::string::npos);
}

TEST_CASE("Fresh: advance_progress Done → ShowKey with key string", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action();                   // → Progress
    ov->advance_progress(4, "AAAA-BBBB-CCCC", 0, 0); // Done
    CHECK(ov->step() == EncryptionSetupOverlay::Step::ShowKey);
    CHECK(ov->recovery_key() == "AAAA-BBBB-CCCC");
}

TEST_CASE("Fresh: ShowKey Continue disabled until checkbox checked", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
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

TEST_CASE("Fresh: the saved-key checkbox is a real CheckButton driving Continue",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action();
    ov->advance_progress(4, "KEY", 0, 0);
    st.run(*ov, {0, 0, 800, 600}); // paint ShowKey so the checkbox is placed

    // The first CheckButton is "I've saved my recovery key" (the second,
    // SaveKey's "keep it on this device", stays hidden in Fresh).
    tk::CheckButton* cb = nullptr;
    for (auto& c : ov->children())
    {
        if (auto* b = dynamic_cast<tk::CheckButton*>(c.get()); b && !cb)
        {
            cb = b;
        }
    }
    REQUIRE(cb);
    CHECK(cb->visible());
    CHECK_FALSE(cb->checked());

    cb->access_default_action(); // same path as a click / Space
    CHECK(ov->key_saved_checked());
    ov->simulate_primary_action();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Done);
}

TEST_CASE("Fresh: ShowKey ignores backdrop clicks; Intro and Done honour them",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    int closes = 0;
    ov->on_close = [&]()
    {
        ++closes;
    };
    st.run(*ov, {0, 0, 800, 600});

    ov->simulate_backdrop_click(); // Intro: nothing to lose
    CHECK(closes == 1);

    ov->simulate_primary_action(); // → Progress
    ov->simulate_backdrop_click();
    CHECK(closes == 1);

    ov->advance_progress(4, "KEY", 0, 0); // → ShowKey
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_backdrop_click(); // would throw the only copy of the key away
    CHECK(closes == 1);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::ShowKey);

    ov->simulate_check_key_saved();
    ov->simulate_primary_action(); // → Done
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_backdrop_click();
    CHECK(closes == 2);
}

TEST_CASE("Fresh: PassphraseEntry ignores backdrop clicks", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    bool closed = false;
    ov->on_close = [&]()
    {
        closed = true;
    };
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_select_passphrase_mode();
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_backdrop_click();
    CHECK_FALSE(closed);
}

TEST_CASE("Fresh: Save to file is offered only when the shell wires it", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action();
    ov->advance_progress(4, "KEY", 0, 0);
    st.run(*ov, {0, 0, 800, 600});

    auto save_button_visible = [&]
    {
        for (const auto& ch : ov->children())
        {
            if (auto* b = dynamic_cast<tk::Button*>(ch.get()))
            {
                if (b->label().starts_with("Save to file"))
                {
                    return b->visible();
                }
            }
        }
        return false;
    };
    CHECK_FALSE(save_button_visible());

    std::string saved;
    ov->on_save_to_file = [&](std::string k)
    {
        saved = k;
    };
    st.run(*ov, {0, 0, 800, 600});
    CHECK(save_button_visible());
}

TEST_CASE("Fresh: a successful save ticks the saved checkbox", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action();
    ov->advance_progress(4, "KEY", 0, 0);

    ov->key_save_result(false, "disk full");
    CHECK_FALSE(ov->key_saved_checked());
    ov->key_save_result(true, "");
    CHECK(ov->key_saved_checked());
    ov->simulate_primary_action();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Done);
}

TEST_CASE("Fresh: passphrase mode skips ShowKey → Done directly", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    StubHost host;
    auto ov =
        tk::create_root_widget<EncryptionSetupOverlay>(&host, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_select_passphrase_mode();
    ov->passphrase_field()->set_text("pw");
    ov->passphrase_confirm_field()->set_text("pw");
    ov->simulate_primary_action();
    ov->advance_progress(4, "", 0, 0); // Done, empty key → passphrase mode
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Done);
}

TEST_CASE("Fresh: Progress error returns to Intro with message", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action();
    ov->advance_progress(5, "network error", 0, 0);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Intro);
    CHECK(ov->error_msg() == "network error");
}

TEST_CASE("Fresh: passphrase-mode Progress error returns to PassphraseEntry",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    StubHost host;
    auto ov =
        tk::create_root_widget<EncryptionSetupOverlay>(&host, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_select_passphrase_mode();
    ov->passphrase_field()->set_text("pw");
    ov->passphrase_confirm_field()->set_text("pw");
    ov->simulate_primary_action();
    ov->advance_progress(5, "network error", 0, 0);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::PassphraseEntry);
    CHECK(ov->error_msg() == "network error");
}

TEST_CASE("format_recovery_key groups in fours", "[encryption][overlay]")
{
    CHECK(EncryptionSetupOverlay::format_recovery_key("EsTcAbCdEfGh12") == "EsTc AbCd EfGh 12");
    // Already-grouped keys (matrix-sdk's own format) round-trip unchanged.
    CHECK(EncryptionSetupOverlay::format_recovery_key("EsTc AbCd EfGh") == "EsTc AbCd EfGh");
    CHECK(EncryptionSetupOverlay::format_recovery_key("").empty());
}

// ── Recover mode ─────────────────────────────────────────────────────────────

TEST_CASE("Recover: starts on the Choose step", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Choose);
}

TEST_CASE("Recover: Choose › Enter recovery key → EnterKey, Back → Choose", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_choose_recovery_key();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::EnterKey);
    ov->simulate_back();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Choose);
}

TEST_CASE("Recover: EnterKey Unlock fires on_recover with key", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    StubHost host;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(&host,
                                                             EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_choose_recovery_key();
    REQUIRE(ov->key_field() != nullptr);
    ov->key_field()->set_text("my-recovery-key");
    std::string fired_key;
    ov->on_recover = [&](std::string k)
    {
        fired_key = k;
    };
    ov->simulate_primary_action(); // Unlock
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
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(&host,
                                                             EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_choose_recovery_key(); // → EnterKey
    st.run(*ov, {0, 0, 800, 600});

    REQUIRE(ov->key_field() != nullptr);
    // Native creation is now deferred until a field is actually arranged
    // (see TextField::ensure_native_) — passphrase_field_ only ever gets
    // arranged on the PassphraseEntry step (EncryptionSetupOverlay.cpp's
    // paint()), which this Recover-mode scenario never reaches, so only
    // key_field_ is created here.
    REQUIRE(host.fields_created.size() == 1);
    auto* key_native = host.fields_created[0];

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

TEST_CASE("Recover: 'Use another device' is offered only with a verified peer",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Recover);
    int relayouts = 0;
    ov->on_layout_changed = [&]
    {
        ++relayouts;
    };
    st.run(*ov, {0, 0, 800, 600});
    CHECK_FALSE(ov->has_verified_other_device());
    ov->set_has_verified_other_device(true);
    CHECK(ov->has_verified_other_device());
    CHECK(relayouts == 1);
    ov->set_has_verified_other_device(true); // unchanged: no extra relayout
    CHECK(relayouts == 1);
}

TEST_CASE("Recover: 'Use another device' fires on_request_sas and waits", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->set_has_verified_other_device(true);
    bool sas_fired = false;
    ov->on_request_sas = [&]()
    {
        sas_fired = true;
    };
    ov->simulate_sas_link();
    CHECK(sas_fired);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::WaitingForOtherDevice);
    CHECK(ov->in_verification_step());
}

TEST_CASE("Recover: self-verification after verifying a user no longer names them",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->set_has_verified_other_device(true);
    ov->show_outgoing_user_request("Alice");
    CHECK(ov->peer() == "Alice");
    CHECK_FALSE(ov->peer_is_own_device());
    ov->verification_failed({}, true);
    ov->return_to_start();
    REQUIRE(ov->step() == EncryptionSetupOverlay::Step::Choose);
    ov->simulate_sas_link();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::WaitingForOtherDevice);
    CHECK(ov->peer().empty());
    CHECK(ov->peer_is_own_device());
}

TEST_CASE("Recover: Cancel while waiting cancels and returns to Choose", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_sas_link();
    bool cancelled = false;
    ov->on_cancel_verification = [&]
    {
        cancelled = true;
    };
    ov->simulate_primary_action(); // Cancel
    CHECK(cancelled);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Choose);
}

TEST_CASE("Recover: lost everything → LostAccess → reset callback", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_choose_lost_access();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::LostAccess);
    bool reset = false;
    ov->on_reset_encryption = [&]
    {
        reset = true;
    };
    ov->simulate_primary_action();
    CHECK(reset);
}

TEST_CASE("Recover: Progress Done → Done step", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_choose_recovery_key();
    ov->advance_progress(0, "", 0, 0);
    ov->advance_progress(4, "", 0, 0);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Done);
    CHECK(ov->done_kind() == EncryptionSetupOverlay::DoneKind::Unlocked);
}

TEST_CASE("Recover: Progress error returns to EnterKey with message", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_choose_recovery_key();
    ov->advance_progress(5, "bad key", 0, 0);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::EnterKey);
    CHECK(ov->error_msg() == "bad key");
}

// ── Interactive verification ─────────────────────────────────────────────────

namespace
{
// Both methods agreed: the decimals plus the 7 emoji.
tesseract::VerificationSas emoji_and_decimal_sas()
{
    return {{{"\xf0\x9f\x90\xb6", "Dog"},
             {"\xf0\x9f\x90\xb1", "Cat"},
             {"\xf0\x9f\xa6\x81", "Lion"},
             {"\xf0\x9f\x90\xbb", "Bear"},
             {"\xf0\x9f\x90\xbc", "Panda"},
             {"\xf0\x9f\x90\xa8", "Koala"},
             {"\xf0\x9f\x90\xaf", "Tiger"}},
            {1234, 5678, 9012}};
}

// The peer offered only `decimal` (MSC4405).
tesseract::VerificationSas decimal_only_sas()
{
    return {{}, {1000, 4242, 9191}};
}
} // namespace

TEST_CASE("Verify: incoming request → Continue accepts and waits", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Verify);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::IncomingRequest);
    ov->show_incoming_request("Phone", true);
    st.run(*ov, {0, 0, 800, 600});
    bool accepted = false;
    ov->on_accept_request = [&]
    {
        accepted = true;
    };
    ov->simulate_primary_action();
    CHECK(accepted);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::WaitingForOtherDevice);
}

TEST_CASE("Verify: 'Not me' fires on_decline_request", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Verify);
    ov->show_incoming_request("Phone", true);
    st.run(*ov, {0, 0, 800, 600});
    bool declined = false;
    ov->on_decline_request = [&]
    {
        declined = true;
    };
    ov->simulate_secondary();
    CHECK(declined);
}

TEST_CASE("Verify: emoji + decimal → They match → Confirming → Done", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Verify);
    ov->show_incoming_request("Phone", true);
    ov->simulate_primary_action(); // Continue
    ov->show_sas(emoji_and_decimal_sas());
    st.run(*ov, {0, 0, 800, 600}); // paints the numbers and the grid
    CHECK(ov->step() == EncryptionSetupOverlay::Step::CompareCodes);
    CHECK(ov->sas().emojis.size() == 7);
    CHECK(ov->sas().decimals[2] == 9012);

    bool matched = false;
    ov->on_sas_match = [&]
    {
        matched = true;
    };
    ov->simulate_primary_action();
    CHECK(matched);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Confirming);
    CHECK(ov->busy()); // an incoming request must not pre-empt this

    ov->verification_done(EncryptionSetupOverlay::DoneKind::OtherDeviceConfirmed);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Done);
    CHECK(ov->done_kind() == EncryptionSetupOverlay::DoneKind::OtherDeviceConfirmed);
}

TEST_CASE("Verify: decimal-only SAS → CompareCodes → They match → Confirming",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Verify);
    ov->show_incoming_request("Phone", true);
    ov->simulate_primary_action(); // Continue
    ov->show_sas(decimal_only_sas());
    st.run(*ov, {0, 0, 800, 600}); // paints the numbers, no emoji grid
    CHECK(ov->step() == EncryptionSetupOverlay::Step::CompareCodes);
    CHECK(ov->sas().emojis.empty());
    CHECK(ov->sas().decimals[0] == 1000);
    CHECK(ov->sas().decimals[2] == 9191);

    bool matched = false;
    ov->on_sas_match = [&]
    {
        matched = true;
    };
    ov->simulate_primary_action();
    CHECK(matched);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Confirming);
}

TEST_CASE("Verify: They don't match → VerifyFailed; the SDK echo keeps the "
          "local explanation",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Verify);
    ov->show_incoming_request("Phone", true);
    ov->simulate_primary_action();
    ov->show_sas(emoji_and_decimal_sas());
    bool mismatch = false;
    ov->on_sas_mismatch = [&]
    {
        mismatch = true;
    };
    ov->simulate_secondary();
    CHECK(mismatch);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::VerifyFailed);
    const std::string local = ov->error_msg();
    CHECK_FALSE(local.empty());
    ov->verification_failed("m.mismatched_sas", false);
    CHECK(ov->error_msg() == local);
}

TEST_CASE("Recover: a failed outgoing verification offers Try again", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_sas_link(); // we started it
    ov->verification_failed("timed out", true);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::VerifyFailed);
    bool retried = false;
    ov->on_retry_verification = [&]
    {
        retried = true;
    };
    ov->simulate_primary_action(); // Try again
    CHECK(retried);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::WaitingForOtherDevice);
}

TEST_CASE("ShowKey and Progress count as busy", "[encryption][overlay]")
{
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    CHECK_FALSE(ov->busy());
    ov->simulate_primary_action(); // → Progress
    CHECK(ov->busy());
    ov->advance_progress(4, "KEY", 0, 0); // → ShowKey
    CHECK(ov->busy());
}

// ── Progress step labels ──────────────────────────────────────────────────────

TEST_CASE("advance_progress shows one label plus a fraction while backing up",
          "[encryption][overlay]")
{
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    ov->simulate_primary_action();
    ov->advance_progress(1, "", 0, 0); // CreatingBackup
    const auto first = ov->progress_label();
    CHECK_FALSE(first.empty());
    CHECK(ov->progress_fraction() < 0.0f);
    ov->advance_progress(3, "", 42, 100); // BackingUp
    CHECK(ov->progress_label() == first); // no internal stage names
    CHECK(ov->progress_fraction() > 0.41f);
    CHECK(ov->progress_fraction() < 0.43f);
}

// ── Done → close ─────────────────────────────────────────────────────────────

TEST_CASE("Done: close button fires on_close", "[encryption][overlay]")
{
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    ov->simulate_primary_action();
    ov->advance_progress(4, "KEY", 0, 0);
    ov->simulate_check_key_saved();
    ov->simulate_primary_action();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Done);
    bool closed = false;
    ov->on_close = [&]()
    {
        closed = true;
    };
    ov->simulate_primary_action(); // Close
    CHECK(closed);
}

// ── Cross-signing reset flow ─────────────────────────────────────────────────

TEST_CASE("Reset: begin_reset_wait enters ResetApproving", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->begin_reset_wait();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::ResetApproving);
}

TEST_CASE("Reset: approval hands off to the Fresh recovery-key flow", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->begin_reset_wait();
    ov->reset_approved();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Intro);
}

TEST_CASE("Reset: Cancel on ResetApproving fires on_cancel_reset", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    bool cancelled = false;
    ov->on_cancel_reset = [&]()
    {
        cancelled = true;
    };
    st.run(*ov, {0, 0, 800, 600});
    ov->begin_reset_wait();
    ov->simulate_primary_action(); // Cancel
    CHECK(cancelled);
}

TEST_CASE("Reset: report_reset_error stays on ResetApproving with a message",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->begin_reset_wait();
    ov->report_reset_error("approval timed out");
    CHECK(ov->step() == EncryptionSetupOverlay::Step::ResetApproving);
    CHECK(ov->error_msg() == "approval timed out");
}

TEST_CASE("Verify: declining a request that interrupted Done returns to Done",
          "[encryption][overlay]")
{
    // Otherwise "Not me" would land on Fresh Intro, whose button would make
    // a new recovery key replacing the one just saved.
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action();
    ov->advance_progress(4, "KEY", 0, 0);
    ov->simulate_check_key_saved();
    ov->simulate_primary_action(); // → Done
    REQUIRE(ov->step() == EncryptionSetupOverlay::Step::Done);

    ov->show_incoming_request("Phone", true);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::IncomingRequest);
    ov->return_to_start(); // what ShellBase's on_decline_request does
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Done);
}

TEST_CASE("Verify: a failed incoming request never offers a retry, even after "
          "an earlier outgoing attempt",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_sas_link();       // outgoing: retry would be allowed
    ov->simulate_primary_action(); // Cancel → Choose

    ov->show_incoming_request("Phone", true);
    ov->simulate_primary_action(); // Continue
    ov->show_sas(emoji_and_decimal_sas());
    ov->simulate_secondary(); // They don't match
    REQUIRE(ov->step() == EncryptionSetupOverlay::Step::VerifyFailed);

    bool retried = false, closed = false;
    ov->on_retry_verification = [&]
    {
        retried = true;
    };
    ov->on_close = [&]
    {
        closed = true;
    };
    ov->simulate_primary_action(); // Close, not Try again
    CHECK_FALSE(retried);
    CHECK(closed);
}

TEST_CASE("Reset: 'Copy link' copies the approval URL, not a recovery key", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    std::string copied;
    ov->on_copy_to_clipboard = [&](std::string s)
    {
        copied = s;
    };
    ov->set_reset_account("@test:example.org");
    ov->begin_reset_wait();
    ov->set_reset_approval_url("https://auth.example.org/account/?action=reset");
    st.run(*ov, {0, 0, 800, 600});

    tk::Button* copy = nullptr;
    for (const auto& ch : ov->children())
    {
        if (auto* b = dynamic_cast<tk::Button*>(ch.get()); b && b->label() == "Copy link")
        {
            copy = b;
        }
    }
    REQUIRE(copy != nullptr);
    CHECK(copy->visible());
    copy->click();
    CHECK(copied == "https://auth.example.org/account/?action=reset");
}

// ── SaveKey mode ────────────────────────────────────────────────────────────

TEST_CASE("SaveKey: Intro explains, then shows the held key; ticking the box "
          "finishes and reports it saved",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::SaveKey);
    ov->set_recovery_key("EsTcabcd");
    bool saved = false, enabled = false, kept = true;
    ov->on_key_saved = [&](bool keep)
    {
        saved = true;
        kept = keep;
    };
    ov->on_enable_recovery = [&](std::string)
    {
        enabled = true;
    };
    st.run(*ov, {0, 0, 800, 600});
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Intro);

    ov->simulate_primary_action();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::ShowKey);
    CHECK(ov->recovery_key() == "EsTcabcd");
    CHECK_FALSE(enabled); // shows the existing key, never makes a new one

    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_primary_action(); // box unticked: stays put
    CHECK(ov->step() == EncryptionSetupOverlay::Step::ShowKey);
    CHECK_FALSE(saved);

    ov->simulate_check_key_saved();
    ov->simulate_primary_action();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Done);
    CHECK(ov->done_kind() == EncryptionSetupOverlay::DoneKind::Protected);
    CHECK(saved);
    CHECK_FALSE(kept); // keeping it on the device is opt-in
}

TEST_CASE("SaveKey: ticking 'keep it on this device' is reported", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::SaveKey);
    ov->set_recovery_key("KEY");
    bool kept = false;
    ov->on_key_saved = [&](bool keep)
    {
        kept = keep;
    };
    ov->show_key_now();
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_check_key_saved();
    ov->simulate_check_keep_on_device();
    ov->simulate_primary_action();
    CHECK(kept);
}

TEST_CASE("SaveKey before sign-out: skip signs out, Done signs out, closing cancels",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::SaveKey);
    ov->set_recovery_key("KEY");
    ov->set_before_sign_out(true);
    int sign_outs = 0, closes = 0;
    ov->on_sign_out = [&](bool)
    {
        ++sign_outs;
    };
    ov->on_close = [&]
    {
        ++closes;
    };
    st.run(*ov, {0, 0, 800, 600});

    ov->simulate_backdrop_click(); // cancel
    CHECK(closes == 1);
    CHECK(sign_outs == 0);
    ov->simulate_skip(); // "Sign out without saving"
    CHECK(sign_outs == 1);
    CHECK(closes == 1);

    ov->simulate_primary_action(); // → ShowKey
    ov->simulate_check_key_saved();
    ov->simulate_primary_action(); // → Done
    REQUIRE(ov->step() == EncryptionSetupOverlay::Step::Done);
    ov->simulate_primary_action(); // "Sign out"
    CHECK(sign_outs == 2);
}

TEST_CASE("SaveKey: Remind me later closes without marking it saved", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::SaveKey);
    bool closed = false, saved = false;
    ov->on_close = [&]
    {
        closed = true;
    };
    ov->on_key_saved = [&](bool)
    {
        saved = true;
    };
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_skip();
    CHECK(closed);
    CHECK_FALSE(saved);
}

TEST_CASE("SaveKey: show_key_now skips the explanation", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::SaveKey);
    ov->set_recovery_key("KEY");
    ov->show_key_now();
    st.run(*ov, {0, 0, 800, 600});
    CHECK(ov->step() == EncryptionSetupOverlay::Step::ShowKey);
    CHECK(ov->busy()); // an incoming request can't push the only copy away
}

TEST_CASE("SaveKey: a key kept in secure storage can be looked at and left",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::SaveKey);
    ov->set_recovery_key("KEY");
    int closes = 0;
    bool saved = false;
    ov->on_close = [&]
    {
        ++closes;
    };
    ov->on_key_saved = [&](bool)
    {
        saved = true;
    };
    ov->simulate_primary_action(); // → ShowKey
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_backdrop_click();
    CHECK(closes == 1);
    CHECK_FALSE(saved);
    ov->simulate_back();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Intro);
}

TEST_CASE("SaveKey: the only copy of a key can't be left unsaved", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::SaveKey);
    ov->set_recovery_key("KEY");
    int closes = 0;
    ov->on_close = [&]
    {
        ++closes;
    };
    ov->show_key_now();
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_backdrop_click();
    CHECK(closes == 0);
}

TEST_CASE("AutoSetupNotice: OK closes it", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(
        nullptr, EncryptionSetupOverlay::Mode::AutoSetupNotice);
    bool closed = false;
    ov->on_close = [&]
    {
        closed = true;
    };
    st.run(*ov, {0, 0, 800, 600});
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Intro);
    ov->simulate_primary_action();
    CHECK(closed);
}

TEST_CASE("SaveKey before sign-out: 'Sign out and delete the key' asks to delete it",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::SaveKey);
    ov->set_recovery_key("KEY");
    ov->set_before_sign_out(true);
    std::vector<bool> removes;
    ov->on_sign_out = [&](bool remove)
    {
        removes.push_back(remove);
    };
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_skip();                    // keep it
    ov->simulate_sign_out_and_remove_key(); // delete it
    REQUIRE(removes.size() == 2);
    CHECK_FALSE(removes[0]);
    CHECK(removes[1]);
}

TEST_CASE("AutoSetupNotice: Turn off backup shows progress, then Done", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(
        nullptr, EncryptionSetupOverlay::Mode::AutoSetupNotice);
    bool asked = false;
    ov->on_turn_off_backup = [&]
    {
        asked = true;
    };
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_turn_off_backup();
    CHECK(asked);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Progress);
    ov->backup_turned_off();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Done);
    CHECK(ov->done_kind() == EncryptionSetupOverlay::DoneKind::BackupOff);
}

TEST_CASE("AutoSetupNotice: a failed turn-off returns to the notice with the error",
          "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(
        nullptr, EncryptionSetupOverlay::Mode::AutoSetupNotice);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_turn_off_backup();
    ov->advance_progress(5, "boom", 0, 0);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Intro);
    CHECK(ov->error_msg() == "boom");
}

TEST_CASE("Verify: cancel codes map to plain wording", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Recover);
    st.run(*ov, {0, 0, 800, 600});
    ov->simulate_sas_link();
    ov->verification_cancelled("m.accepted", false);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::VerifyFailed);
    CHECK(ov->error_msg().find("m.") == std::string::npos);
    CHECK(ov->error_msg().find("Another of your devices") != std::string::npos);
}

TEST_CASE("Fresh: an existing server backup asks before it is replaced", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    int enable_calls = 0;
    std::vector<std::string> replace_calls;
    ov->on_enable_recovery = [&](std::string)
    {
        ++enable_calls;
    };
    ov->on_replace_backup = [&](std::string p)
    {
        replace_calls.push_back(p);
    };

    ov->simulate_primary_action(); // Create recovery key → Progress
    REQUIRE(ov->step() == EncryptionSetupOverlay::Step::Progress);
    REQUIRE(enable_calls == 1);

    // Setup stopped at a backup it must not delete on its own.
    ov->advance_progress(7, "", 0, 0);
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Intro);
    CHECK(ov->replace_backup_pending());
    CHECK(ov->error_msg().find("key storage") != std::string::npos);
    CHECK(replace_calls.empty());

    // Only the explicit confirmation replaces it, and it does not re-run the
    // plain enable.
    ov->simulate_primary_action();
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Progress);
    CHECK(enable_calls == 1);
    REQUIRE(replace_calls.size() == 1);
    CHECK(replace_calls[0].empty());

    // Done clears the pending state.
    ov->advance_progress(4, "KEY", 0, 0);
    CHECK_FALSE(ov->replace_backup_pending());
}

TEST_CASE("Fresh: dismissing the replace-backup prompt forgets it", "[encryption][overlay]")
{
    EncryptionSetupOverlayStage st;
    auto ov = tk::create_root_widget<EncryptionSetupOverlay>(nullptr,
                                                             EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov, {0, 0, 800, 600});
    ov->on_enable_recovery = [](std::string) {};
    ov->simulate_primary_action();
    ov->advance_progress(7, "", 0, 0);
    REQUIRE(ov->replace_backup_pending());
    ov->set_visible(false);
    CHECK_FALSE(ov->replace_backup_pending());
}
