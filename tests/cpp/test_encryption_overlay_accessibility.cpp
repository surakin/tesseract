#include <catch2/catch_test_macros.hpp>

#include "tk/access_tree.h"
#include "access_test_util.h"
#include "tk/theme.h"
#include "views/EncryptionSetupOverlay.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"

#include <memory>
#include <string>

// EncryptionSetupOverlay as a screen reader sees it: a modal dialog named
// by the step title, real buttons for every link / option card, and text
// rows for the painted-only content (recovery key, SAS codes, progress).

using namespace tk;
using tesseract::views::EncryptionSetupOverlay;

namespace enc_overlay_a11y_test
{

struct EncOverlayA11yStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(800, 600);
    void run(Widget& root)
    {
        LayoutCtx lc{surface->factory(), Theme::light()};
        root.measure(lc, {800, 600});
        root.arrange(lc, {0, 0, 800, 600});
        PaintCtx pc{surface->canvas(), surface->factory(), Theme::light()};
        root.paint(pc);
    }
};

} // namespace enc_overlay_a11y_test

using enc_overlay_a11y_test::EncOverlayA11yStage;
using access_test::find_named;
using access_test::find_prefix;

TEST_CASE("Recover › Choose is a modal dialog with real option-card and link buttons",
          "[encryption][overlay][accessibility]")
{
    EncOverlayA11yStage st;
    auto ov =
        create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Recover);
    ov->set_has_verified_other_device(true);
    bool closed = false;
    ov->on_close = [&]
    {
        closed = true;
    };
    st.run(*ov);

    AccessNode tree = build_access_tree(ov.get());
    CHECK(tree.role == Role::Dialog);
    CHECK(tree.modal);
    CHECK(tree.name == "Confirm it's you");
    CHECK_FALSE(tree.description.empty());

    const AccessNode* device = find_named(tree, "Use another device");
    REQUIRE(device != nullptr);
    CHECK(device->role == Role::Button);
    CHECK(device->description == "Approve from a device where you're already signed in");

    const AccessNode* key = find_named(tree, "Enter recovery key");
    REQUIRE(key != nullptr);
    CHECK(invoke_default_action(*key));
    CHECK(ov->step() == EncryptionSetupOverlay::Step::EnterKey);

    st.run(*ov);
    tree = build_access_tree(ov.get());
    CHECK(tree.name == "Enter your recovery key");
    // The Choose-step controls are gone once the step changes.
    CHECK(find_named(tree, "Use another device") == nullptr);
    const AccessNode* back = find_named(tree, "Back");
    REQUIRE(back != nullptr);
    CHECK(invoke_default_action(*back));
    CHECK(ov->step() == EncryptionSetupOverlay::Step::Choose);

    st.run(*ov);
    tree = build_access_tree(ov.get());
    const AccessNode* skip = find_named(tree, "Skip for now");
    REQUIRE(skip != nullptr);
    CHECK(invoke_default_action(*skip));
    CHECK(closed);
}

TEST_CASE("ShowKey exposes the recovery key grouped in fours",
          "[encryption][overlay][accessibility]")
{
    EncOverlayA11yStage st;
    auto ov =
        create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    ov->advance_progress(4, "EsTcabcdEFGH", 0, 0);
    REQUIRE(ov->step() == EncryptionSetupOverlay::Step::ShowKey);
    st.run(*ov);

    AccessNode tree = build_access_tree(ov.get());
    CHECK(tree.name == "Save your recovery key");
    const AccessNode* key = find_named(tree, "Recovery key: EsTc abcd EFGH");
    REQUIRE(key != nullptr);
    CHECK(key->role == Role::StaticText);
    CHECK(find_named(tree, "I've saved my recovery key") != nullptr);
}

TEST_CASE("CompareCodes reads the numbers and each emoji by name",
          "[encryption][overlay][accessibility]")
{
    EncOverlayA11yStage st;
    auto ov =
        create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Verify);
    tesseract::VerificationSas sas;
    sas.decimals = {1234, 5678, 9012};
    sas.emojis = {{"\xF0\x9F\x90\xB6", "Dog"}, {"\xF0\x9F\x90\xB1", "Cat"}};
    ov->show_sas(sas);
    st.run(*ov);

    AccessNode tree = build_access_tree(ov.get());
    CHECK(tree.name == "Compare numbers");
    CHECK(find_named(tree, "Numbers: 1234, 5678, 9012") != nullptr);
    CHECK(find_named(tree, "1. Dog") != nullptr);
    CHECK(find_named(tree, "2. Cat") != nullptr);
    const AccessNode* mismatch = find_named(tree, "They don't match");
    REQUIRE(mismatch != nullptr);
    CHECK(mismatch->role == Role::Button);
}

TEST_CASE("Progress is exposed as a progress node with the percentage",
          "[encryption][overlay][accessibility]")
{
    EncOverlayA11yStage st;
    auto ov =
        create_root_widget<EncryptionSetupOverlay>(nullptr, EncryptionSetupOverlay::Mode::Fresh);
    ov->simulate_primary_action(); // Intro → Progress
    REQUIRE(ov->step() == EncryptionSetupOverlay::Step::Progress);
    ov->advance_progress(3, "", 1, 4);
    st.run(*ov);

    AccessNode tree = build_access_tree(ov.get());
    const AccessNode* bar = find_prefix(tree, "Securing your messages");
    REQUIRE(bar != nullptr);
    // The dialog itself is named after the progress label too; the row is
    // the one carrying the progress role.
    bool found_bar = false;
    for (const auto& ch : tree.children)
    {
        if (ch.role == Role::ProgressBar)
        {
            found_bar = true;
            CHECK(ch.name.find("25%") != std::string::npos);
        }
    }
    CHECK(found_bar);
}

TEST_CASE("passphrase fields are labelled and never expose the typed text",
          "[encryption][overlay][accessibility]")
{
    EncOverlayA11yStage st;
    StubHost host;
    auto ov =
        create_root_widget<EncryptionSetupOverlay>(&host, EncryptionSetupOverlay::Mode::Fresh);
    st.run(*ov);
    ov->simulate_select_passphrase_mode();
    REQUIRE(ov->passphrase_field() != nullptr);
    ov->passphrase_field()->set_text("hunter2");
    st.run(*ov);

    CHECK(ov->passphrase_field()->access_name() == "Recovery passphrase");
    CHECK(ov->passphrase_confirm_field()->access_name() == "Confirm recovery passphrase");
    AccessNode tree = build_access_tree(ov.get());
    CHECK(find_named(tree, "hunter2") == nullptr);
}

TEST_CASE("verification failure and completion are announced",
          "[encryption][overlay][accessibility]")
{
    struct RecordingHost : StubHost
    {
        std::vector<std::string> said;
        void on_announce_(const std::string& t, Politeness) override
        {
            said.push_back(t);
        }
    };
    RecordingHost host;
    auto ov =
        create_root_widget<EncryptionSetupOverlay>(&host, EncryptionSetupOverlay::Mode::Verify);
    ov->verification_failed("Timed out", true);
    ov->return_to_start();
    REQUIRE_FALSE(host.said.empty());
    CHECK(host.said.back() == "Timed out");

    auto ov2 =
        create_root_widget<EncryptionSetupOverlay>(&host, EncryptionSetupOverlay::Mode::Verify);
    ov2->verification_done(EncryptionSetupOverlay::DoneKind::UserVerified);
    CHECK(host.said.back().rfind("Verified.", 0) == 0);
}
