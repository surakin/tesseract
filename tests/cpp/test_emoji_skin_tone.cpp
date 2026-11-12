#include <catch2/catch_test_macros.hpp>

#include "tk/list_view.h"
#include "tk/theme.h"
#include "tk_test_host.h"
#include "tk_test_surface.h"
#include "views/EmojiPicker.h"
#include "views/ShortcodeEngine.h"
#include "views/SkinTonePopover.h"

#include <tesseract/emoji.h>
#include <tesseract/prefs.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

using tesseract::emoji::SkinTone;
namespace emoji = tesseract::emoji;

// ─────────────────────────────────────────────────────────────────────────
//  emoji:: tone table
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("Skin tone lookup: bases, variants and emoji without tones", "[emoji][skin_tone]")
{
    CHECK(emoji::supports_skin_tone("👍"));
    CHECK(emoji::supports_skin_tone("👍🏽"));
    CHECK_FALSE(emoji::supports_skin_tone("🍕"));

    CHECK(emoji::with_skin_tone("👍", SkinTone::Medium) == "👍🏽");
    CHECK(emoji::with_skin_tone("👍🏻", SkinTone::Dark) == "👍🏿"); // re-tone
    CHECK(emoji::with_skin_tone("👍🏻", SkinTone::None) == "👍");
    CHECK(emoji::with_skin_tone("🍕", SkinTone::Dark) == "🍕");

    CHECK(emoji::base_glyph("👍🏾") == "👍");
    CHECK(emoji::base_glyph("🍕") == "🍕");
}

TEST_CASE("Skin tone variants come from the data, not codepoint insertion", "[emoji][skin_tone]")
{
    // VS16 base: the toned form drops U+FE0F.
    CHECK(emoji::with_skin_tone("☝️", SkinTone::Medium) == "☝🏽");
    // ZWJ multi-person sequence: both people get the tone.
    CHECK(emoji::with_skin_tone("🧑‍🤝‍🧑", SkinTone::Light) ==
          "🧑🏻‍🤝‍🧑🏻");
    // Qualified base ("person: blond hair" → "person: light skin tone, blond hair").
    CHECK(emoji::with_skin_tone("👱", SkinTone::Light) == "👱🏻");
}

TEST_CASE("Every tone-capable picker entry round-trips through base_glyph", "[emoji][skin_tone]")
{
    int bases = 0;
    for (const auto& e : emoji::all())
    {
        if (!emoji::supports_skin_tone(e.glyph))
        {
            continue;
        }
        ++bases;
        for (SkinTone t : emoji::kSkinTones)
        {
            const auto toned = emoji::with_skin_tone(e.glyph, t);
            CHECK(emoji::base_glyph(toned) == e.glyph);
            CHECK((t == SkinTone::None) == (toned == e.glyph));
        }
    }
    CHECK(bases >= 300);
}

TEST_CASE("Skin tone keys round-trip; unknown keys mean no tone", "[emoji][skin_tone]")
{
    for (SkinTone t : emoji::kSkinTones)
    {
        CHECK(emoji::skin_tone_from_key(emoji::skin_tone_key(t)) == t);
    }
    CHECK(std::string(emoji::skin_tone_key(SkinTone::None)).empty());
    CHECK(emoji::skin_tone_from_key("medium_dark") == SkinTone::MediumDark);
    CHECK(emoji::skin_tone_from_key("purple") == SkinTone::None);
}

// ─────────────────────────────────────────────────────────────────────────
//  Prefs
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("Prefs emoji_skin_tone round-trips and is omitted when empty", "[prefs][skin_tone]")
{
    tesseract::PrefsData p;
    p.emoji_skin_tone = "medium";
    CHECK(tesseract::Prefs::parse(tesseract::Prefs::serialize(p)).emoji_skin_tone == "medium");

    p.emoji_skin_tone.clear();
    auto j = nlohmann::json::parse(tesseract::Prefs::serialize(p));
    CHECK_FALSE(j.contains("emoji_skin_tone"));
}

TEST_CASE("Prefs serialize overlays onto the stored event, keeping unknown keys",
          "[prefs][skin_tone]")
{
    tesseract::PrefsData p;
    p.last_room = "!new:x";
    p.open_rooms = {"!new:x"};
    const std::string stored =
        R"({"last_room":"!old:x","future_key":{"a":1},"emoji_skin_tone":"dark"})";

    auto j = nlohmann::json::parse(tesseract::Prefs::serialize(p, stored));
    CHECK(j["last_room"] == "!new:x");
    CHECK(j["future_key"]["a"] == 1);
    // Our field is authoritative: empty removes the stored value.
    CHECK_FALSE(j.contains("emoji_skin_tone"));

    // A non-object / unparsable base is ignored.
    auto j2 = nlohmann::json::parse(tesseract::Prefs::serialize(p, "not json"));
    CHECK(j2["last_room"] == "!new:x");
}

// ─────────────────────────────────────────────────────────────────────────
//  :shortcode: autocomplete
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("ShortcodeEngine returns tone-capable glyphs in the given tone", "[shortcode][skin_tone]")
{
    tesseract::views::ShortcodeEngine engine;
    auto has = [](const auto& results, const std::string& glyph)
    {
        return std::any_of(results.begin(), results.end(),
                           [&](const auto& s)
                           {
                               return s.glyph == glyph;
                           });
    };
    auto plain = engine.lookup("thumbs_up", {});
    CHECK(has(plain, "👍"));
    auto toned = engine.lookup("thumbs_up", {}, 8, SkinTone::Medium);
    CHECK(has(toned, "👍🏽"));
    CHECK_FALSE(has(toned, "👍"));
}

// ─────────────────────────────────────────────────────────────────────────
//  GridView secondary action (long press / right-click / Shift+Enter)
// ─────────────────────────────────────────────────────────────────────────

namespace
{

struct CountingGridAdapter : tk::GridAdapter
{
    std::size_t count() const override
    {
        return 20;
    }
    void paint_cell(std::size_t, tk::PaintCtx&, tk::Rect, bool, bool) override
    {
    }
};

struct GridStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(400, 300);
    TestHost host{nullptr};
    std::unique_ptr<tk::GridView> grid_owner;
    tk::GridView* grid = nullptr;
    CountingGridAdapter adapter;
    std::vector<int> clicks;
    std::vector<int> contexts;
    bool handled = true;

    GridStage()
    {
        grid_owner = tk::create_root_widget<tk::GridView>(&host);
        host.set_root(grid_owner.get());
        grid = grid_owner.get();
        grid->set_adapter(&adapter);
        grid->set_cell_size(32, 32);
        grid->set_spacing(2, 2);
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        grid->arrange(lc, {0, 0, 400, 300});
        grid->on_cell_clicked = [this](int i)
        {
            clicks.push_back(i);
        };
        grid->on_cell_context_requested = [this](int i)
        {
            contexts.push_back(i);
            return handled;
        };
    }
};

} // namespace

TEST_CASE("GridView long press fires the context request and eats the click",
          "[tk][gridview][skin_tone]")
{
    GridStage st;
    REQUIRE(st.grid->on_pointer_down({16, 16}));
    REQUIRE(st.host.pending_delays_.size() == 1);
    CHECK(st.host.pending_delays_[0].ms == tk::GridView::kLongPressMs);

    st.host.fire_all_delays();
    CHECK(st.contexts == std::vector<int>{0});

    st.grid->on_pointer_up({16, 16}, true);
    CHECK(st.clicks.empty());
}

TEST_CASE("GridView unhandled long press stays a normal click", "[tk][gridview][skin_tone]")
{
    GridStage st;
    st.handled = false; // e.g. an emoji without tones
    st.grid->on_pointer_down({16, 16});
    st.host.fire_all_delays();
    st.grid->on_pointer_up({16, 16}, true);
    CHECK(st.contexts == std::vector<int>{0});
    CHECK(st.clicks == std::vector<int>{0});
}

TEST_CASE("GridView release or drag before the delay cancels the long press",
          "[tk][gridview][skin_tone]")
{
    GridStage st;
    st.grid->on_pointer_down({16, 16});
    st.grid->on_pointer_up({16, 16}, true);
    st.host.fire_all_delays();
    CHECK(st.contexts.empty());
    CHECK(st.clicks == std::vector<int>{0});

    st.grid->on_pointer_down({16, 16});
    st.grid->on_pointer_drag({16 + tk::GridView::kLongPressSlop + 1, 16});
    st.host.fire_all_delays();
    CHECK(st.contexts.empty());
}

TEST_CASE("GridView right-click and Shift+Enter request the context action",
          "[tk][gridview][skin_tone]")
{
    GridStage st;
    CHECK(st.grid->on_right_click({50, 16})); // cell 1
    CHECK(st.contexts == std::vector<int>{1});

    st.host.request_focus(st.grid);
    st.grid->set_selected_index(2);
    tk::KeyEvent e;
    e.key = tk::Key::Enter;
    e.shift = true;
    CHECK(st.grid->on_key_down(e));
    CHECK(st.contexts == std::vector<int>{1, 2});
    CHECK(st.clicks.empty());
}

// ─────────────────────────────────────────────────────────────────────────
//  EmojiPicker
// ─────────────────────────────────────────────────────────────────────────

namespace
{

using tesseract::views::EmojiPicker;

struct PickerStage
{
    std::unique_ptr<TestSurface> surface = TestSurface::create(320, 360);
    TestHost host{nullptr};
    std::unique_ptr<EmojiPicker> owner;
    EmojiPicker* picker = nullptr;
    tk::GridView* grid = nullptr;
    std::vector<std::string> selected;
    std::vector<SkinTone> tone_changes;
    int dismissed = 0;

    PickerStage()
    {
        owner = tk::create_root_widget<EmojiPicker>(&host);
        host.set_root(owner.get());
        picker = owner.get();
        picker->set_visible(true);
        tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
        picker->measure(lc, {EmojiPicker::kWidth, EmojiPicker::kHeight});
        picker->arrange(lc, {0, 0, EmojiPicker::kWidth, EmojiPicker::kHeight});
        for (const auto& ch : picker->children())
        {
            if (auto* g = dynamic_cast<tk::GridView*>(ch.get()))
            {
                grid = g;
            }
        }
        REQUIRE(grid);
        picker->on_selected = [this](const std::string& g)
        {
            selected.push_back(g);
        };
        picker->on_skin_tone_changed = [this](SkinTone t)
        {
            tone_changes.push_back(t);
        };
        picker->on_dismiss = [this]
        {
            ++dismissed;
        };
    }

    // Index of `base` on the default (Smileys & People) page.
    static int index_of(std::string_view base)
    {
        auto entries = emoji::by_category(emoji::Category::SmileysPeople);
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            if (entries[i]->glyph == base)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }
};

} // namespace

TEST_CASE("EmojiPicker set_skin_tone re-tones tone-capable cells only", "[view][emoji][skin_tone]")
{
    PickerStage st;
    const int thumbs = PickerStage::index_of("👍");
    const int grin = PickerStage::index_of("😀");
    REQUIRE(thumbs >= 0);
    REQUIRE(grin >= 0);

    st.picker->set_skin_tone(SkinTone::Medium);
    st.grid->on_cell_clicked(thumbs);
    st.grid->on_cell_clicked(grin);
    CHECK(st.selected == std::vector<std::string>{"👍🏽", "😀"});
    CHECK(st.tone_changes.empty()); // host-set, not user-picked
}

TEST_CASE("EmojiPicker tone menu opens only for tone-capable emoji", "[view][emoji][skin_tone]")
{
    PickerStage st;
    CHECK_FALSE(st.grid->on_cell_context_requested(PickerStage::index_of("😀")));
    CHECK_FALSE(st.picker->skin_tone_popover()->is_open());

    CHECK(st.grid->on_cell_context_requested(PickerStage::index_of("👍")));
    CHECK(st.picker->skin_tone_popover()->is_open());
}

TEST_CASE("EmojiPicker picking a tone sets the default and inserts the variant",
          "[view][emoji][skin_tone]")
{
    PickerStage st;
    const int thumbs = PickerStage::index_of("👍");
    REQUIRE(st.grid->on_cell_context_requested(thumbs));
    auto* menu = st.picker->skin_tone_popover();
    CHECK(menu->glyph_for(SkinTone::Dark) == "👍🏿");

    // Focus starts on the current tone (None); three steps right = Medium.
    tk::KeyEvent right;
    right.key = tk::Key::Right;
    for (int i = 0; i < 3; ++i)
    {
        CHECK(st.picker->on_key_down(right));
    }
    tk::KeyEvent enter;
    enter.key = tk::Key::Enter;
    CHECK(st.picker->on_key_down(enter));

    CHECK_FALSE(menu->is_open());
    CHECK(st.picker->skin_tone() == SkinTone::Medium);
    CHECK(st.tone_changes == std::vector<SkinTone>{SkinTone::Medium});
    CHECK(st.selected == std::vector<std::string>{"👍🏽"});

    // The rest of the grid follows the new default.
    st.grid->on_cell_clicked(PickerStage::index_of("👋"));
    CHECK(st.selected.back() == "👋🏽");
}

TEST_CASE("EmojiPicker Escape and outside clicks close only the tone menu",
          "[view][emoji][skin_tone]")
{
    PickerStage st;
    const int thumbs = PickerStage::index_of("👍");
    REQUIRE(st.grid->on_cell_context_requested(thumbs));

    tk::KeyEvent esc;
    esc.key = tk::Key::Escape;
    CHECK(st.picker->on_key_down(esc));
    CHECK_FALSE(st.picker->skin_tone_popover()->is_open());
    CHECK(st.dismissed == 0);

    REQUIRE(st.grid->on_cell_context_requested(thumbs));
    // A point in the picker's search row, clear of the menu.
    const tk::Point outside{5, 5};
    REQUIRE_FALSE(st.picker->skin_tone_popover()->contains_world(outside));
    CHECK(st.picker->dispatch_pointer_down(outside) == st.picker);
    CHECK_FALSE(st.picker->skin_tone_popover()->is_open());
    CHECK(st.selected.empty());
    CHECK(st.dismissed == 0);
}

TEST_CASE("Emoji 18.0 additions and renamed-flag shortcodes", "[emoji][skin_tone]")
{
    CHECK(emoji::with_skin_tone("🫹", SkinTone::Medium) == "🫹🏽"); // leftwards thumb sign
    CHECK(emoji::with_skin_tone("🫺", SkinTone::Dark) == "🫺🏿");   // rightwards thumb sign

    // Pre-18.0 canonical shortcode of a renamed flag still resolves.
    auto hits = emoji::by_shortcode_prefix("flag_st_helena");
    CHECK(std::any_of(hits.begin(), hits.end(),
                      [](const auto& h)
                      {
                          return h.first->glyph == "🇸🇭";
                      }));
}

// ─────────────────────────────────────────────────────────────────────────
//  Emoji version detection (tk/emoji_support.h)
// ─────────────────────────────────────────────────────────────────────────

#include "tk/emoji_support.h"

#include <set>
#include <utility>

namespace
{

// Probe-only factory: renders everything except `rejected`.
struct ProbeFactory : tk::CanvasFactory
{
    std::set<std::string, std::less<>> rejected;
    int probes = 0;

    std::unique_ptr<tk::Image> decode_image(std::span<const std::uint8_t>) override
    {
        return nullptr;
    }
    std::unique_ptr<tk::TextLayout> build_rich_text(std::span<const tk::TextSpan>,
                                                    const tk::TextStyle&) override
    {
        return nullptr;
    }
    bool can_render_emoji(std::string_view glyph) override
    {
        ++probes;
        return !rejected.contains(glyph);
    }
};

// A Host whose platform font is at `version`.
struct VersionHost : TestHost
{
    using TestHost::TestHost;
    std::uint16_t version = tk::kAllEmojiVersions;
    std::uint16_t supported_emoji_version() override
    {
        return version;
    }
};

} // namespace

TEST_CASE("Emoji versions are recorded per entry and per tone variant", "[emoji][emoji_version]")
{
    CHECK(emoji::emoji_version("🫹") == 180);
    CHECK(emoji::emoji_version("🫹🏽") == 180);
    CHECK(emoji::emoji_version("🤝") == 30);
    CHECK(emoji::emoji_version("🤝🏻") == 140); // tone newer than its base
    CHECK(emoji::emoji_version("not an emoji") == 0);

    const auto& versions = emoji::emoji_versions();
    REQUIRE(!versions.empty());
    CHECK(versions.front() == 180);
    CHECK(std::is_sorted(versions.begin(), versions.end(), std::greater<>()));

    const auto newest = emoji::glyphs_introduced_in(180);
    CHECK(std::find(newest.begin(), newest.end(), "🫹") != newest.end());
    CHECK(std::find(newest.begin(), newest.end(), "🫹🏽") != newest.end());
}

TEST_CASE("detect_emoji_version walks versions newest first and stops early",
          "[emoji][emoji_version]")
{
    ProbeFactory all;
    CHECK(tk::detect_emoji_version(all) == 180);
    CHECK(std::cmp_equal(all.probes, emoji::glyphs_introduced_in(180).size()));

    ProbeFactory no18;
    no18.rejected = {"🫫"}; // cracking face, E18.0
    CHECK(tk::detect_emoji_version(no18) == 170);

    ProbeFactory no17;
    no17.rejected = {"🫫", std::string(emoji::glyphs_introduced_in(170).back())};
    CHECK(tk::detect_emoji_version(no17) == 160);

    ProbeFactory none;
    for (auto v : emoji::emoji_versions())
    {
        for (auto g : emoji::glyphs_introduced_in(v))
        {
            none.rejected.emplace(g);
        }
    }
    // Nothing drawable at all: unfiltered rather than an empty picker.
    CHECK(tk::detect_emoji_version(none) == tk::kAllEmojiVersions);
}

TEST_CASE("offered_emoji falls back to the base, then hides", "[emoji][emoji_version]")
{
    // Handshake (E3.0) with tones from E14.0.
    CHECK(tk::offered_emoji("🤝", SkinTone::Medium, 180) == "🤝🏽");
    CHECK(tk::offered_emoji("🤝", SkinTone::Medium, 130) == "🤝");
    CHECK(tk::offered_emoji("🫹", SkinTone::None, 170) == std::nullopt);
    CHECK(tk::offered_emoji("x", SkinTone::None, 0) == "x"); // unknown: untouched
    CHECK(tk::all_skin_tones_supported("🤝", 140));
    CHECK_FALSE(tk::all_skin_tones_supported("🤝", 130));
}

TEST_CASE("EmojiPicker hides emoji newer than the platform font", "[view][emoji][emoji_version]")
{
    auto surface = TestSurface::create(320, 360);
    VersionHost host{nullptr};
    host.version = 170;
    auto owner = tk::create_root_widget<EmojiPicker>(&host);
    host.set_root(owner.get());
    EmojiPicker& picker = *owner;
    picker.set_visible(true);
    tk::LayoutCtx lc{surface->factory(), tk::Theme::light()};
    picker.arrange(lc, {0, 0, EmojiPicker::kWidth, EmojiPicker::kHeight});
    tk::GridView* grid = nullptr;
    for (const auto& ch : picker.children())
    {
        if (auto* g = dynamic_cast<tk::GridView*>(ch.get()))
        {
            grid = g;
        }
    }
    REQUIRE(grid);

    std::vector<std::string> shown;
    picker.on_selected = [&](const std::string& g)
    {
        shown.push_back(g);
    };
    auto page = [&]
    {
        shown.clear();
        const int n = static_cast<int>(grid->adapter()->count());
        for (int i = 0; i < n; ++i)
        {
            grid->on_cell_clicked(i);
        }
        return shown;
    };
    auto contains = [](const std::vector<std::string>& v, const std::string& g)
    {
        return std::find(v.begin(), v.end(), g) != v.end();
    };

    auto smileys = page();
    CHECK_FALSE(contains(smileys, "🫹"));
    CHECK(contains(smileys, "😀"));

    picker.set_search_query("thumb");
    auto search = page();
    CHECK_FALSE(contains(search, "🫹"));
    CHECK(contains(search, "👍"));

    // Tones newer than the font fall back to the base, with no tone menu.
    host.version = 130;
    picker.set_search_query("handshake");
    picker.set_skin_tone(SkinTone::Medium);
    auto hands = page();
    CHECK(contains(hands, "🤝"));
    CHECK_FALSE(contains(hands, "🤝🏽"));
    const auto it = std::find(hands.begin(), hands.end(), "🤝");
    CHECK_FALSE(grid->on_cell_context_requested(static_cast<int>(it - hands.begin())));
}

TEST_CASE("ShortcodeEngine leaves out emoji newer than the platform font",
          "[shortcode][emoji_version]")
{
    tesseract::views::ShortcodeEngine engine;
    auto glyphs = [&](std::uint16_t max_version)
    {
        std::vector<std::string> out;
        for (const auto& s : engine.lookup("leftwards_thumb", {}, 8, SkinTone::None, max_version))
        {
            out.push_back(s.glyph);
        }
        return out;
    };
    CHECK(glyphs(180) == std::vector<std::string>{"🫹"});
    CHECK(glyphs(170).empty());
}

TEST_CASE("Backend probe: a real emoji renders, a noncharacter doesn't",
          "[emoji][emoji_version][backend]")
{
    auto surface = TestSurface::create(64, 64);
    CHECK(surface->factory().can_render_emoji("😀"));
#if !defined(_WIN32)
    // U+1FFFE is a Unicode noncharacter: permanently unassigned, so no OS font
    // update can ever give it a glyph (a merely unassigned codepoint is
    // eventually filled in by a newer Unicode / emoji font). Skipped on Win32:
    // its backend keeps the default can_render_emoji(), which answers true for
    // everything because it bundles an emoji font covering the whole table.
    CHECK_FALSE(surface->factory().can_render_emoji("\xF0\x9F\xBF\xBE")); // U+1FFFE
#endif
    INFO(
        "detected Emoji version on this machine: " << tk::detect_emoji_version(surface->factory()));
    CHECK(tk::detect_emoji_version(surface->factory()) > 0);
}

TEST_CASE("Tone menu buttons are hit-testable right after opening, before paint",
          "[view][emoji][skin_tone]")
{
    PickerStage st;
    REQUIRE(st.grid->on_cell_context_requested(PickerStage::index_of("👍")));
    const tk::Rect menu = st.picker->skin_tone_popover()->bounds();
    using tesseract::views::SkinTonePopover;
    // Centre of the last (Dark) button.
    const tk::Point dark{menu.x + SkinTonePopover::kPadding +
                             5 * (SkinTonePopover::kButtonSize + SkinTonePopover::kGap) +
                             SkinTonePopover::kButtonSize / 2,
                         menu.y + SkinTonePopover::kPadding + SkinTonePopover::kButtonSize / 2};
    tk::Widget* hit = st.picker->dispatch_pointer_down(dark);
    REQUIRE(hit != nullptr);
    CHECK(hit != st.picker); // a button, not the "outside the menu" close path
    hit->on_pointer_up({dark.x - hit->bounds().x, dark.y - hit->bounds().y}, true);
    CHECK(st.selected == std::vector<std::string>{"👍🏿"});
    CHECK(st.picker->skin_tone() == SkinTone::Dark);
}

TEST_CASE("Keyboard-opened tone menu returns focus to the grid on Escape",
          "[view][emoji][skin_tone]")
{
    PickerStage st;
    st.host.request_focus(st.grid);
    REQUIRE(st.host.focused_widget() == st.grid);
    REQUIRE(st.grid->on_cell_context_requested(PickerStage::index_of("👍")));
    CHECK(st.host.focused_widget() != st.grid); // on a tone button now

    tk::KeyEvent esc;
    esc.key = tk::Key::Escape;
    CHECK(st.picker->on_key_down(esc));
    CHECK(st.host.focused_widget() == st.grid);
}

TEST_CASE("Tone menu opened without focus inside the picker leaves focus alone",
          "[view][emoji][skin_tone]")
{
    PickerStage st;
    st.host.clear_focus();
    REQUIRE(st.grid->on_cell_context_requested(PickerStage::index_of("👍")));
    CHECK(st.host.focused_widget() == nullptr);
    tk::KeyEvent esc;
    esc.key = tk::Key::Escape;
    CHECK(st.picker->on_key_down(esc));
    CHECK(st.host.focused_widget() == nullptr);
}

// ─────────────────────────────────────────────────────────────────────────
//  Synced skin tone vs. stale echoes (AccountSession helpers)
// ─────────────────────────────────────────────────────────────────────────

#include <tesseract/account_session.h>

TEST_CASE("A stale echo inside the window doesn't revert a local tone change", "[skin_tone][sync]")
{
    using namespace std::chrono_literals;
    tesseract::AccountSession a;
    const auto t0 = std::chrono::steady_clock::now();
    tesseract::set_local_emoji_skin_tone(a, SkinTone::Light, t0);

    tesseract::apply_synced_emoji_skin_tone(a, SkinTone::None, t0 + 1s); // stale
    CHECK(a.emoji_skin_tone == SkinTone::Light);
    tesseract::apply_synced_emoji_skin_tone(a, SkinTone::Light, t0 + 2s); // our echo
    CHECK(a.emoji_skin_tone == SkinTone::Light);
    tesseract::settle_emoji_skin_tone(a, t0 + 60s);
    CHECK(a.emoji_skin_tone == SkinTone::Light); // parked stale value dropped
}

TEST_CASE("Another device's newer tone wins once the window has passed", "[skin_tone][sync]")
{
    using namespace std::chrono_literals;
    tesseract::AccountSession a;
    const auto t0 = std::chrono::steady_clock::now();
    tesseract::set_local_emoji_skin_tone(a, SkinTone::Light, t0);

    // Device A wrote Dark after us; our own echo never shows up.
    tesseract::apply_synced_emoji_skin_tone(a, SkinTone::Dark, t0 + 3s);
    CHECK(a.emoji_skin_tone == SkinTone::Light); // parked for now
    tesseract::settle_emoji_skin_tone(a, t0 + 5s);
    CHECK(a.emoji_skin_tone == SkinTone::Light); // still inside the window
    tesseract::settle_emoji_skin_tone(a, t0 + tesseract::kSkinToneEchoWindow + 1s);
    CHECK(a.emoji_skin_tone == SkinTone::Dark);

    // Later updates apply directly — nothing stays frozen.
    tesseract::apply_synced_emoji_skin_tone(a, SkinTone::Medium, t0 + 120s);
    CHECK(a.emoji_skin_tone == SkinTone::Medium);
}
