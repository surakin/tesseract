# Async Image Cache for Pickers Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give EmojiPicker and StickerPicker a single shared, disk-backed, off-the-UI-thread image cache on all four shells, eliminating Qt6's private per-picker caches and the per-shell re-fetch / UI-thread-decode duplication.

**Architecture:** Add one shared async path in `ShellBase` — `ensure_picker_image_()` (dedup + `media_disk_cache_` + network fetch on a worker) and `finalize_picker_image_()` (UI-thread cache insert) — plus a new per-shell `decode_image_()` virtual that decodes bytes → `tk::Image` **on the worker thread** (every backend's decoder is already thread-safe; only Win32 wraps a device-independent `IWICBitmap` that uploads at paint). GTK4 is the reference: its pickers already share `ShellBase::tk_images_`/`anim_cache_`; this plan finishes that pattern (adds disk cache + off-thread decode) and brings Qt6/macOS/Win32 onto it. Qt6's private `image_cache_`/`animated_cache_`/`fetches_in_flight_`/`anim_timer_` are deleted.

**Tech Stack:** C++20, `tesseract_tk` shared toolkit, Catch2 (`tesseract_tests`), Qt6 (QImage/QImageReader), GTK4 (GdkPixbuf+cairo), AppKit (CGImageSource), Win32 (WIC/Direct2D).

---

## Background: current state (verified)

| Shell | Picker cache | Dedup set | Decode thread | Disk cache |
|-------|--------------|-----------|---------------|------------|
| **GTK4** (reference) | shared `tk_images_`/`anim_cache_` | ShellBase `emoji_/sticker_fetches_in_flight_` | UI (`g_idle_add`) | **no** |
| **macOS** | shared `tk_images_`/`anim_cache_` | ShellBase `emoji_/sticker_fetches_in_flight_` | UI (`dispatch_get_main_queue`) | **no** |
| **Win32** | shared `tk_images_`/`anim_cache_` | (own path `request_sticker_image`) | UI (`on_media_bytes_ready_`) | **no** |
| **Qt6** | **private** `image_cache_`/`animated_cache_` per picker | **private** `fetches_in_flight_` | UI (`onImageLoaded_` slot) | **no** |

`tk::Image` is a platform-neutral abstract type; the shared caches store `std::unique_ptr<tk::Image>`. Every backend's decoder is thread-safe off the UI thread:
- Qt6: `QImage`/`QImageReader` are usable on non-GUI threads; `tk::qt6::make_image(QImage)` just wraps.
- GTK4: `decode_image_to_cairo_surface()` / `decode_animation()` (MainWindow.cpp anon namespace) use GdkPixbuf+cairo; `tk::cairo_pango::make_image()` refcounts (thread-safe).
- macOS: `CGImageSource` decode is thread-safe; `tk::cg::make_image(CGImageRef)` retains.
- Win32: `tk::d2d::decode_animation()` / WIC decode are pure WIC (free-threaded); `D2DImage` holds an `IWICBitmap`, GPU upload deferred to paint. Precedent: `ui/shared/tk/video_win32.cpp` already calls `make_image_from_bgra(*backend_, …)` off-thread.

Each shell's per-`MediaKind::MediaImage` decode in `on_media_bytes_ready_` is the exact logic the new `decode_image_()` must perform. To stay DRY without changing message-list threading, each shell's `on_media_bytes_ready_` `MediaImage` branch is refactored to call the same `decode_image_()` (still synchronously on the UI thread there — behaviour unchanged), while the picker path calls it on the worker.

---

## File Inventory

| File | Change |
|------|--------|
| `ui/shared/app/ShellBase.h` | +`DecodedImage` struct; +`decode_image_`/`monotonic_ms_` pure virtuals; +`start_anim_tick_`/`repaint_pickers_` virtuals; +`ensure_picker_image_`/`finalize_picker_image_` decls |
| `ui/shared/app/ShellBase.cpp` | implement `ensure_picker_image_` + `finalize_picker_image_` |
| `tests/cpp/test_picker_image_cache.cpp` | **new** — unit tests for `finalize_picker_image_` cache routing/dedup |
| `tests/CMakeLists.txt` | register the new test |
| `ui/shared/tk/canvas_d2d.h` / `.cpp` | +free `tk::d2d::decode_image(Backend&, span)` (off-thread single-frame) |
| `ui/shared/tk/host_win32.h` | declare `tk::win32::backend_singleton()` for off-thread access |
| `ui/linux-gtk/src/MainWindow.h` / `.cpp` | implement 4 virtuals; route pickers through `ensure_picker_image_`; delete `ensure_emoji_/sticker_image_async` |
| `ui/linux-qt/src/MainWindow.h` / `.cpp` | implement 4 virtuals; install picker providers from MainWindow; refactor `on_media_bytes_ready_` MediaImage → `decode_image_` |
| `ui/linux-qt/src/EmojiPicker.h` / `.cpp` | delete private cache/anim/fetch members + slots/signals; add `setImageProvider` passthrough |
| `ui/linux-qt/src/StickerPicker.h` / `.cpp` | same as EmojiPicker (incl. `animated_cache_`, `anim_timer_`) |
| `ui/macos/src/MainWindowController.mm` | implement 4 virtuals; route `_ensure*ImageAsync` through `ensure_picker_image_` |
| `ui/windows/src/MainWindow.h` / `.cpp` | implement 4 virtuals; route picker providers through `ensure_picker_image_`; delete `request_sticker_image` |
| `CHANGES.md` / `STATUS.md` | document the change |

---

## Task 1: Shared cache primitives in ShellBase (header)

**Files:**
- Modify: `ui/shared/app/ShellBase.h`

- [ ] **Step 1: Add the `DecodedImage` struct**

In `ui/shared/app/ShellBase.h`, immediately **after** the `MediaKind` enum block (the `};` that closes `enum class MediaKind`, currently at line 193), add:

```cpp
    // Result of a worker-thread decode. Exactly one of `still` /
    // `frames` is populated (frames non-empty ⇒ animated).
    struct DecodedImage {
        std::unique_ptr<tk::Image>              still;
        std::vector<std::unique_ptr<tk::Image>> frames;
        std::vector<int>                        delays_ms;
        bool empty() const { return !still && frames.empty(); }
    };
```

- [ ] **Step 2: Add the new abstract / overridable hooks**

In `ui/shared/app/ShellBase.h`, immediately **after** the `cache_rgba_image_` virtual (the line ending `std::vector<uint8_t> /*rgba*/) {}` at line 264), add:

```cpp
    // Decode `bytes` into a tk::Image (or animated frames). Scaled so the
    // longest side is ≤ max(max_w, max_h). MUST be safe to call on a
    // worker thread (no UI/device context): every backend's decoder is
    // thread-safe; Win32 wraps a device-independent IWICBitmap. Used by
    // ensure_picker_image_ (worker) and on_media_bytes_ready_ (UI).
    virtual DecodedImage decode_image_(const std::vector<uint8_t>& bytes,
                                       int max_w, int max_h) = 0;

    // Monotonic clock in ms from the SAME epoch the shell's animation
    // timer / anim_cache_.advance() uses (Qt: QDateTime msecs; GTK:
    // g_get_monotonic_time/1000; macOS: NSDate*1000; Win32: GetTickCount64).
    virtual std::int64_t monotonic_ms_() = 0;

    // Start the shell's shared animation frame-tick timer if it is not
    // already running. Default no-op (shells with no animated content).
    virtual void start_anim_tick_() {}

    // Repaint whichever picker surfaces are visible (relayout + invalidate).
    // Default no-op.
    virtual void repaint_pickers_() {}
```

- [ ] **Step 3: Declare the two concrete helpers**

In `ui/shared/app/ShellBase.h`, immediately **after** the `ensure_media_image_` declaration (line 394, `void ensure_media_image_(const std::string& url, int max_w, int max_h);`), add:

```cpp
    // Shared async picker-image path. Idempotent: no-op if already in
    // tk_images_ / anim_cache_ / in-flight. Dedups via
    // emoji_fetches_in_flight_ (is_sticker == false) or
    // sticker_fetches_in_flight_ (true). Worker: media_disk_cache_ →
    // client_->fetch_source_bytes → media_disk_cache_.store →
    // decode_image_ (OFF the UI thread) → post finalize_picker_image_.
    void ensure_picker_image_(const std::string& url, bool is_sticker);

    // UI-thread tail of ensure_picker_image_. Erases the in-flight key,
    // routes `d` into anim_cache_ (animated) or tk_images_ (still),
    // calls start_anim_tick_() / repaint_pickers_(). Public-testable
    // logic (see test_picker_image_cache.cpp). Safe if `d.empty()`.
    void finalize_picker_image_(std::string url, bool is_sticker,
                                DecodedImage d);
```

- [ ] **Step 4: Build the toolkit to verify the header compiles**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tk 2>&1 | grep -E "error:" | head -20
```
Expected: errors only of the form "abstract class ... `decode_image_` ... `monotonic_ms_`" from the four shell `MainWindow`s NOT yet implementing the new pure virtuals — that is expected and fixed in Tasks 4–8. `tesseract_tk` itself (no ShellBase subclass) must compile cleanly with **no** errors. If `tesseract_tk` reports errors, fix them before continuing.

- [ ] **Step 5: Commit**

```bash
git add ui/shared/app/ShellBase.h
git commit -m "feat(shell): DecodedImage + picker-cache hooks on ShellBase"
```

---

## Task 2: Implement `ensure_picker_image_` / `finalize_picker_image_`

**Files:**
- Modify: `ui/shared/app/ShellBase.cpp`

- [ ] **Step 1: Implement both methods**

In `ui/shared/app/ShellBase.cpp`, immediately **after** the closing `}` of `ShellBase::ensure_media_image_` (the function ending at line 81), add:

```cpp
void ShellBase::ensure_picker_image_(const std::string& url, bool is_sticker) {
    if (url.empty() || tk_images_.count(url) || anim_cache_.has(url)) return;
    auto& inflight = is_sticker ? sticker_fetches_in_flight_
                                : emoji_fetches_in_flight_;
    if (!inflight.insert(url).second) return;
    run_async_([this, url, is_sticker]() {
        auto bytes = media_disk_cache_.load(url);
        if (bytes.empty()) {
            bytes = client_->fetch_source_bytes(url);
            if (!bytes.empty()) media_disk_cache_.store(url, bytes);
        }
        if (bytes.empty()) {
            post_to_ui_([this, url, is_sticker]() {
                (is_sticker ? sticker_fetches_in_flight_
                            : emoji_fetches_in_flight_).erase(url);
            });
            return;
        }
        // Decode OFF the UI thread. Picker cells are bounded; reuse the
        // inline-image bound so picker bitmaps are reusable by the
        // message list (same shared tk_images_ key = the mxc url).
        DecodedImage d = decode_image_(bytes,
                                       visual::kMaxInlineImageWidth,
                                       visual::kMaxInlineImageHeight);
        post_to_ui_([this, url, is_sticker, d = std::move(d)]() mutable {
            finalize_picker_image_(url, is_sticker, std::move(d));
        });
    });
}

void ShellBase::finalize_picker_image_(std::string url, bool is_sticker,
                                       DecodedImage d) {
    (is_sticker ? sticker_fetches_in_flight_
                : emoji_fetches_in_flight_).erase(url);
    if (tk_images_.count(url) || anim_cache_.has(url)) return;
    if (!d.frames.empty()) {
        anim_cache_.store(url, std::move(d.frames), std::move(d.delays_ms),
                          monotonic_ms_());
        start_anim_tick_();
    } else if (d.still) {
        tk_images_.emplace(std::move(url), std::move(d.still));
    } else {
        return;  // decode failed — leave uncached so a later paint retries
    }
    repaint_pickers_();
}
```

`visual::kMaxInlineImageWidth/Height` come from `<tesseract/visual.h>`, already `#include`d in ShellBase.cpp (line 9).

- [ ] **Step 2: Build the toolkit**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tk 2>&1 | grep -E "error:" | head -20
```
Expected: no errors in `tesseract_tk` (still-abstract shell `MainWindow`s are not part of this target).

- [ ] **Step 3: Commit**

```bash
git add ui/shared/app/ShellBase.cpp
git commit -m "feat(shell): implement ensure_picker_image_ + finalize_picker_image_"
```

---

## Task 3: Unit tests for `finalize_picker_image_`

**Files:**
- Create: `tests/cpp/test_picker_image_cache.cpp`
- Modify: `tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing tests**

Create `tests/cpp/test_picker_image_cache.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include "app/ShellBase.h"

#include <memory>
#include <string>
#include <vector>

using tesseract::ShellBase;

namespace {

struct FakeImage : tk::Image {
    int width()  const override { return 1; }
    int height() const override { return 1; }
};

// Minimal concrete ShellBase. Implements every pure virtual; re-exposes
// the protected members the tests assert on.
struct TestShell : ShellBase {
    void post_to_ui_(std::function<void()> fn) override { fn(); }
    void on_rooms_updated_() override {}
    void on_media_bytes_ready_(const std::string&, MediaKind,
                               std::vector<uint8_t>) override {}
    void on_tab_state_changed_ui_() override {}
    DecodedImage decode_image_(const std::vector<uint8_t>&,
                               int, int) override { return {}; }
    std::int64_t monotonic_ms_() override { return 1000; }
    void start_anim_tick_() override { ++anim_tick_starts; }
    void repaint_pickers_() override { ++repaints; }

    int anim_tick_starts = 0;
    int repaints         = 0;

    using ShellBase::finalize_picker_image_;
    using ShellBase::DecodedImage;
    using ShellBase::tk_images_;
    using ShellBase::anim_cache_;
    using ShellBase::emoji_fetches_in_flight_;
    using ShellBase::sticker_fetches_in_flight_;
};

ShellBase::DecodedImage make_still() {
    ShellBase::DecodedImage d;
    d.still = std::make_unique<FakeImage>();
    return d;
}

ShellBase::DecodedImage make_anim(int n) {
    ShellBase::DecodedImage d;
    for (int i = 0; i < n; ++i) {
        d.frames.push_back(std::make_unique<FakeImage>());
        d.delays_ms.push_back(50);
    }
    return d;
}

} // namespace

TEST_CASE("finalize routes a still image into tk_images_", "[picker-cache]") {
    TestShell s;
    s.emoji_fetches_in_flight_.insert("mxc://e/1");
    s.finalize_picker_image_("mxc://e/1", /*is_sticker=*/false, make_still());

    CHECK(s.tk_images_.count("mxc://e/1") == 1);
    CHECK(s.anim_cache_.has("mxc://e/1") == false);
    CHECK(s.emoji_fetches_in_flight_.count("mxc://e/1") == 0);
    CHECK(s.repaints == 1);
    CHECK(s.anim_tick_starts == 0);
}

TEST_CASE("finalize routes animated frames into anim_cache_", "[picker-cache]") {
    TestShell s;
    s.sticker_fetches_in_flight_.insert("mxc://s/1");
    s.finalize_picker_image_("mxc://s/1", /*is_sticker=*/true, make_anim(3));

    CHECK(s.anim_cache_.has("mxc://s/1") == true);
    CHECK(s.tk_images_.count("mxc://s/1") == 0);
    CHECK(s.sticker_fetches_in_flight_.count("mxc://s/1") == 0);
    CHECK(s.anim_tick_starts == 1);
    CHECK(s.repaints == 1);
}

TEST_CASE("finalize does not overwrite an existing cache entry",
          "[picker-cache]") {
    TestShell s;
    s.tk_images_.emplace("mxc://e/2", std::make_unique<FakeImage>());
    const tk::Image* original = s.tk_images_.at("mxc://e/2").get();
    s.emoji_fetches_in_flight_.insert("mxc://e/2");

    s.finalize_picker_image_("mxc://e/2", false, make_still());

    CHECK(s.tk_images_.at("mxc://e/2").get() == original);   // unchanged
    CHECK(s.emoji_fetches_in_flight_.count("mxc://e/2") == 0); // still cleared
    CHECK(s.repaints == 0);
}

TEST_CASE("finalize with an empty decode result caches nothing",
          "[picker-cache]") {
    TestShell s;
    s.emoji_fetches_in_flight_.insert("mxc://e/3");

    s.finalize_picker_image_("mxc://e/3", false,
                             ShellBase::DecodedImage{});

    CHECK(s.tk_images_.count("mxc://e/3") == 0);
    CHECK(s.anim_cache_.has("mxc://e/3") == false);
    CHECK(s.emoji_fetches_in_flight_.count("mxc://e/3") == 0);
    CHECK(s.repaints == 0);
}
```

- [ ] **Step 2: Register the test**

In `tests/CMakeLists.txt`, add the source to the `add_executable(tesseract_tests …)` list, immediately after `cpp/test_markdown.cpp` (currently line 46):

```cmake
    cpp/test_markdown.cpp
    cpp/test_picker_image_cache.cpp
)
```
(Replace the existing `cpp/test_markdown.cpp\n)` with the two-line block above.)

- [ ] **Step 3: Run the tests**

```bash
cmake --build build/linux-qt6-debug --target tesseract_tests 2>&1 | tail -10
ctest --test-dir build/linux-qt6-debug -R picker-cache --output-on-failure
```
Expected: all 4 `[picker-cache]` tests pass. (The Qt6 `MainWindow` is not linked by `tesseract_tests`; `TestShell` provides the pure-virtual impls, so the abstract-class state from Task 1 does not block this build.)

- [ ] **Step 4: Commit**

```bash
git add tests/cpp/test_picker_image_cache.cpp tests/CMakeLists.txt
git commit -m "test(shell): finalize_picker_image_ cache routing + dedup"
```

---

## Task 4: GTK4 — implement the four virtuals (reference shell)

**Files:**
- Modify: `ui/linux-gtk/src/MainWindow.h`
- Modify: `ui/linux-gtk/src/MainWindow.cpp`

GTK4 already shares the caches; here it gains the off-thread + disk-cache path via the shared helper, and its `decode_image_`/`monotonic_ms_`/`start_anim_tick_`/`repaint_pickers_` overrides reuse the existing thread-safe helpers.

- [ ] **Step 1: Declare the overrides**

In `ui/linux-gtk/src/MainWindow.h`, in the `private:` section where `on_media_bytes_ready_` is declared, add:

```cpp
    DecodedImage decode_image_(const std::vector<uint8_t>& bytes,
                               int max_w, int max_h) override;
    std::int64_t monotonic_ms_() override;
    void         start_anim_tick_() override;
    void         repaint_pickers_() override;
```

- [ ] **Step 2: Implement them**

In `ui/linux-gtk/src/MainWindow.cpp`, immediately **after** the `MainWindow::on_media_bytes_ready_` definition (ends line 2268), add:

```cpp
ShellBase::DecodedImage
MainWindow::decode_image_(const std::vector<uint8_t>& bytes,
                          int /*max_w*/, int /*max_h*/) {
    // decode_image_to_cairo_surface / decode_animation are in this
    // file's anonymous namespace and are thread-safe (GdkPixbuf + cairo).
    // tk::cairo_pango::make_image refcounts the surface (thread-safe).
    DecodedImage d;
    if (auto anim = decode_animation(bytes)) {
        d.frames.reserve(anim->frames.size());
        for (cairo_surface_t* s : anim->frames) {
            d.frames.push_back(tk::cairo_pango::make_image(s));
            cairo_surface_destroy(s);
        }
        d.delays_ms = std::move(anim->delays_ms);
        if (!d.frames.empty()) return d;
        d.delays_ms.clear();
    }
    if (cairo_surface_t* surf = decode_image_to_cairo_surface(bytes)) {
        d.still = tk::cairo_pango::make_image(surf);
        cairo_surface_destroy(surf);
    }
    return d;
}

std::int64_t MainWindow::monotonic_ms_() {
    return g_get_monotonic_time() / 1000;
}

void MainWindow::start_anim_tick_() {
    start_anim_tick_if_needed_();
}

void MainWindow::repaint_pickers_() {
    if (emoji_picker_shared_)   emoji_picker_shared_->invalidate_image_cache();
    if (emoji_picker_surface_)  emoji_picker_surface_->relayout();
    if (sticker_picker_shared_) sticker_picker_shared_->invalidate_image_cache();
    if (sticker_picker_surface_) sticker_picker_surface_->relayout();
    invalidate_anim_consumers_();
}
```

- [ ] **Step 3: Repoint the picker providers at the shared helper**

In `ui/linux-gtk/src/MainWindow.cpp`, replace the EmojiPicker provider lambda (lines 2874-2882) with:

```cpp
    emoji_picker_shared_->set_image_provider(
        [this](const std::string& cache_key,
                const std::string& /*source_token*/) -> const tk::Image* {
            if (const auto* f = anim_cache_.current_frame(cache_key)) return f;
            auto it = tk_images_.find(cache_key);
            if (it != tk_images_.end()) return it->second.get();
            ensure_picker_image_(cache_key, /*is_sticker=*/false);
            return nullptr;
        });
```

And the StickerPicker provider lambda (lines 2938-2946) with:

```cpp
    sticker_picker_shared_->set_image_provider(
        [this](const std::string& cache_key,
                const std::string& /*source_token*/) -> const tk::Image* {
            if (const auto* f = anim_cache_.current_frame(cache_key)) return f;
            auto it = tk_images_.find(cache_key);
            if (it != tk_images_.end()) return it->second.get();
            ensure_picker_image_(cache_key, /*is_sticker=*/true);
            return nullptr;
        });
```

- [ ] **Step 4: Delete the now-dead async fetchers**

In `ui/linux-gtk/src/MainWindow.cpp`, delete the entire bodies of `MainWindow::ensure_sticker_image_async` (lines 2028-2085) and `MainWindow::ensure_emoji_image_async` (lines 2087-2139). Remove their declarations from `ui/linux-gtk/src/MainWindow.h`. Grep to confirm no other callers:

```bash
grep -rn "ensure_emoji_image_async\|ensure_sticker_image_async" ui/linux-gtk/
```
Expected: no matches after deletion. (Their only callers were the two provider lambdas just rewritten.)

- [ ] **Step 5: Build and smoke-test**

```bash
cmake --build build/linux-gtk-debug --target tesseract 2>&1 | grep -E "error:" | head -20
```
Expected: no errors. (If `build/linux-gtk-debug` is not configured: `cmake --preset linux-gtk-debug` first.)

- [ ] **Step 6: Commit**

```bash
git add ui/linux-gtk/src/MainWindow.h ui/linux-gtk/src/MainWindow.cpp
git commit -m "feat(gtk4): pickers use shared async+disk-cached image path"
```

---

## Task 5: Qt6 — strip private picker caches, install providers from MainWindow

**Files:**
- Modify: `ui/linux-qt/src/EmojiPicker.h` / `ui/linux-qt/src/EmojiPicker.cpp`
- Modify: `ui/linux-qt/src/StickerPicker.h` / `ui/linux-qt/src/StickerPicker.cpp`

- [ ] **Step 1: Add an `setImageProvider` passthrough to the EmojiPicker wrapper**

In `ui/linux-qt/src/EmojiPicker.h`, in the `public:` section (next to `invalidateImages()`), add:

```cpp
    /// Install the image provider on the wrapped shared picker. Called by
    /// MainWindow so the provider can capture the ShellBase shared caches.
    void setImageProvider(tesseract::views::EmojiPicker::ImageProvider p);
```

Delete these private members from `ui/linux-qt/src/EmojiPicker.h`:

```cpp
    std::unordered_map<std::string, std::unique_ptr<tk::Image>> image_cache_;
    std::unordered_set<std::string>                              fetches_in_flight_;
```

Delete the slot/signal/method declarations: the `signals:` block (`void imageLoadedSignal_(...)`), `private slots: void onImageLoaded_(...)`, and `void request_image_(const std::string&);`. Remove now-unused includes (`<QByteArray>` only if unused elsewhere — keep if `popupAt` uses it; otherwise drop `<unordered_map>`/`<unordered_set>`).

- [ ] **Step 2: Gut the EmojiPicker wrapper implementation**

In `ui/linux-qt/src/EmojiPicker.cpp`, in the constructor, **delete** the `shared_->set_image_provider([...])` block (lines 60-69) and the `connect(this, &EmojiPicker::imageLoadedSignal_, …)` call (lines 81-83). Delete the entire `EmojiPicker::request_image_` (lines 124-150) and `EmojiPicker::onImageLoaded_` (lines 152-179) definitions. Add the passthrough:

```cpp
void EmojiPicker::setImageProvider(
        tesseract::views::EmojiPicker::ImageProvider p) {
    if (shared_) shared_->set_image_provider(std::move(p));
}
```

`invalidateImages()` keeps its existing body (it forwards to `shared_->invalidate_image_cache()` + `surface_->update()`); the shared `repaint_pickers_()` will call it.

- [ ] **Step 3: Do the same for the StickerPicker wrapper (incl. animation)**

In `ui/linux-qt/src/StickerPicker.h`: add the passthrough decl:

```cpp
    void setImageProvider(tesseract::views::StickerPicker::ImageProvider p);
```

Delete these private members: `image_cache_`, the `AnimatedEntry` struct, `animated_cache_`, `anim_timer_`, `fetches_in_flight_`. Delete decls: `signals: void imageLoadedSignal_(...)`, `private slots: void onImageLoaded_(...)`, `void onAnimTick_();`, `void request_image_(const std::string&);`.

In `ui/linux-qt/src/StickerPicker.cpp`: in the constructor delete the `shared_->set_image_provider([...])` block (lines 61-80), the `connect(this, &StickerPicker::imageLoadedSignal_, …)` call (lines 96-98), and the `anim_timer_` creation block (lines 100-106 — `anim_timer_ = new QTimer(this); … connect(…, &StickerPicker::onAnimTick_);`). Delete the entire `StickerPicker::request_image_`, `StickerPicker::onImageLoaded_`, and `StickerPicker::onAnimTick_` definitions. In `showEvent`/`hideEvent`, delete any `anim_timer_->start()/stop()` lines (the shared `tk_anim_timer_` in MainWindow now drives animation). Add the passthrough:

```cpp
void StickerPicker::setImageProvider(
        tesseract::views::StickerPicker::ImageProvider p) {
    if (shared_) shared_->set_image_provider(std::move(p));
}
```

- [ ] **Step 4: Build the toolkit-linked picker objects only (header sanity)**

```bash
cmake --build build/linux-qt6-debug --target tesseract 2>&1 | grep -E "error:" | head -30
```
Expected: errors are limited to (a) `MainWindow` still abstract (missing the 4 virtuals — Task 6) and (b) `emojiPicker_`/`stickerPicker_` no longer installing a provider (wired in Task 6). No errors inside `EmojiPicker.cpp`/`StickerPicker.cpp` themselves. Do not try to fully link yet.

- [ ] **Step 5: Commit**

```bash
git add ui/linux-qt/src/EmojiPicker.h ui/linux-qt/src/EmojiPicker.cpp \
        ui/linux-qt/src/StickerPicker.h ui/linux-qt/src/StickerPicker.cpp
git commit -m "refactor(qt6): remove private picker caches; add provider passthrough"
```

---

## Task 6: Qt6 — implement the four virtuals + wire providers

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.h`
- Modify: `ui/linux-qt/src/MainWindow.cpp`

- [ ] **Step 1: Declare the overrides**

In `ui/linux-qt/src/MainWindow.h`, in the `private:` section near `on_media_bytes_ready_`, add:

```cpp
    DecodedImage decode_image_(const std::vector<uint8_t>& bytes,
                               int max_w, int max_h) override;
    std::int64_t monotonic_ms_() override;
    void         start_anim_tick_() override;
    void         repaint_pickers_() override;
```

- [ ] **Step 2: Implement `decode_image_` (QImageReader animation probe + still)**

In `ui/linux-qt/src/MainWindow.cpp`, immediately **after** the `MainWindow::on_media_bytes_ready_` definition (ends line 1989), add. This is exactly the message-list MediaImage decode logic, returning a `DecodedImage` instead of mutating caches; `QImage`/`QImageReader` are safe on a worker thread:

```cpp
ShellBase::DecodedImage
MainWindow::decode_image_(const std::vector<uint8_t>& bytes,
                          int max_w, int max_h) {
    DecodedImage d;
    if (bytes.empty()) return d;
    QByteArray qb(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<int>(bytes.size()));
    QBuffer buf(&qb);
    buf.open(QIODevice::ReadOnly);
    QImageReader reader(&buf);
    reader.setAutoTransform(true);

    if (reader.supportsAnimation() && reader.imageCount() > 1) {
        QImage frame;
        while (reader.read(&frame)) {
            int delay = reader.nextImageDelay();
            if (delay <= 0) delay = 100;
            if (delay < 20) delay = 20;
            QImage scaled = frame.scaled(max_w, max_h,
                                          Qt::KeepAspectRatio,
                                          Qt::SmoothTransformation);
            d.frames.push_back(tk::qt6::make_image(std::move(scaled)));
            d.delays_ms.push_back(delay);
        }
        if (!d.frames.empty()) return d;
        d.delays_ms.clear();
        buf.seek(0);
    }
    QImage img;
    if (!img.loadFromData(reinterpret_cast<const uchar*>(qb.constData()),
                          qb.size()))
        return d;
    QImage scaled = img.scaled(max_w, max_h,
                                Qt::KeepAspectRatio,
                                Qt::SmoothTransformation);
    d.still = tk::qt6::make_image(std::move(scaled));
    return d;
}

std::int64_t MainWindow::monotonic_ms_() {
    return QDateTime::currentMSecsSinceEpoch();
}

void MainWindow::start_anim_tick_() {
    if (tk_anim_timer_ && !tk_anim_timer_->isActive())
        tk_anim_timer_->start();
}

void MainWindow::repaint_pickers_() {
    if (emojiPicker_)   emojiPicker_->invalidateImages();
    if (stickerPicker_) stickerPicker_->invalidateImages();
    if (mainAppSurface_) { mainAppSurface_->relayout(); mainAppSurface_->update(); }
    if (shortcode_popup_visible_() && shortcode_popup_surface_)
        shortcode_popup_surface_->update();
}
```

Ensure `#include <QBuffer>`, `#include <QImageReader>`, `#include <QDateTime>` are present in `MainWindow.cpp` (they already are — used by `on_media_bytes_ready_`).

- [ ] **Step 3: Refactor `on_media_bytes_ready_` MediaImage branch to reuse `decode_image_` (DRY)**

In `ui/linux-qt/src/MainWindow.cpp`, in `MainWindow::on_media_bytes_ready_`, replace the entire `// MediaImage — animated probe first, then static fallback.` section (from `if (tk_images_.count(cache_key) || anim_cache_.has(cache_key))` through the final static `tk_images_.emplace(...)` + repaint, i.e. the block currently spanning roughly the QImageReader animation loop and static fallback) with:

```cpp
    // MediaImage — decode (same path as pickers) then store. Decode stays
    // on the UI thread here (unchanged behaviour); pickers decode on a
    // worker via ensure_picker_image_.
    if (tk_images_.count(cache_key) || anim_cache_.has(cache_key)) {
        mediaImageSizes_.erase(cache_key);
        return;
    }
    int max_w = kMaxImageWidth, max_h = kMaxImageHeight;
    if (auto sit = mediaImageSizes_.find(cache_key);
        sit != mediaImageSizes_.end()) {
        max_w = sit->second.first;
        max_h = sit->second.second;
        mediaImageSizes_.erase(sit);
    }
    DecodedImage d = decode_image_(bytes, max_w, max_h);
    if (!d.frames.empty()) {
        anim_cache_.store(cache_key, std::move(d.frames),
                          std::move(d.delays_ms),
                          QDateTime::currentMSecsSinceEpoch());
        if (tk_anim_timer_ && !tk_anim_timer_->isActive())
            tk_anim_timer_->start();
    } else if (d.still) {
        tk_images_.emplace(cache_key, std::move(d.still));
    } else {
        return;
    }
    if (mainApp_) mainApp_->room_view()->notify_image_ready(cache_key);
    if (mainAppSurface_) { mainAppSurface_->relayout(); mainAppSurface_->update(); }
    if (shortcode_popup_visible_() && shortcode_popup_surface_)
        shortcode_popup_surface_->update();
    return;
```

Leave the `RoomAvatar`/`UserAvatar`/`Tile` branches untouched.

- [ ] **Step 4: Install picker providers from MainWindow**

In `ui/linux-qt/src/MainWindow.cpp`, in the EmojiPicker construction block (after `emojiPicker_->setClient(client_);`, line ~916), add:

```cpp
    emojiPicker_->setImageProvider(
        [this](const std::string& cache_key,
                const std::string& /*source_token*/) -> const tk::Image* {
            if (const auto* f = anim_cache_.current_frame(cache_key)) return f;
            auto it = tk_images_.find(cache_key);
            if (it != tk_images_.end()) return it->second.get();
            ensure_picker_image_(cache_key, /*is_sticker=*/false);
            return nullptr;
        });
```

In the StickerPicker construction block (after `stickerPicker_->setClient(client_);`, line ~966), add:

```cpp
    stickerPicker_->setImageProvider(
        [this](const std::string& cache_key,
                const std::string& /*source_token*/) -> const tk::Image* {
            if (const auto* f = anim_cache_.current_frame(cache_key)) return f;
            auto it = tk_images_.find(cache_key);
            if (it != tk_images_.end()) return it->second.get();
            ensure_picker_image_(cache_key, /*is_sticker=*/true);
            return nullptr;
        });
```

- [ ] **Step 5: Build + run full Qt6 app**

```bash
cmake --build build/linux-qt6-debug 2>&1 | grep -E "error:" | head -20
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -15
```
Expected: clean build, all tests pass (including `[picker-cache]`).

- [ ] **Step 6: Commit**

```bash
git add ui/linux-qt/src/MainWindow.h ui/linux-qt/src/MainWindow.cpp
git commit -m "feat(qt6): pickers share async+disk-cached image cache; off-thread decode"
```

---

## Task 7: macOS — implement the four virtuals + route async fetchers

**Files:**
- Modify: `ui/macos/src/MainWindowController.mm`

macOS already shares the caches; `_decodeMediaBytes:forKey:` is the existing CGImageSource decoder. Split its pure decode out so it can run on a worker.

- [ ] **Step 1: Declare the overrides on `MacShell`**

In `ui/macos/src/MainWindowController.mm`, in the `class MacShell : public tesseract::ShellBase` declaration (near the `on_media_bytes_ready_` / `cache_rgba_image_` overrides), add:

```cpp
    DecodedImage decode_image_(const std::vector<uint8_t>& bytes,
                               int max_w, int max_h) override;
    std::int64_t monotonic_ms_() override;
    void         start_anim_tick_() override;
    void         repaint_pickers_() override;
```

- [ ] **Step 2: Implement `decode_image_` (CGImageSource — thread-safe)**

In `ui/macos/src/MainWindowController.mm`, add (free of `tk_images_`/`anim_cache_` mutation — pure decode, callable off the main queue; CGImageSource is thread-safe):

```cpp
ShellBase::DecodedImage
MacShell::decode_image_(const std::vector<uint8_t>& bytes,
                        int /*max_w*/, int /*max_h*/) {
    DecodedImage d;
    if (bytes.empty()) return d;
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, bytes.data(),
                                  static_cast<CFIndex>(bytes.size()));
    if (!data) return d;
    CGImageSourceRef src = CGImageSourceCreateWithData(data, nullptr);
    CFRelease(data);
    if (!src) return d;

    std::size_t count = CGImageSourceGetCount(src);
    if (count > 1) {
        for (std::size_t i = 0; i < count; ++i) {
            CGImageRef frame = CGImageSourceCreateImageAtIndex(src, i, nullptr);
            if (!frame) continue;
            d.frames.push_back(tk::cg::make_image(frame));
            CGImageRelease(frame);
            int delay_ms = 100;
            CFDictionaryRef props =
                CGImageSourceCopyPropertiesAtIndex(src, i, nullptr);
            if (props) {
                auto try_delay = [&](CFStringRef dk, CFStringRef uk,
                                     CFStringRef ck) {
                    auto* dd = (CFDictionaryRef)CFDictionaryGetValue(props, dk);
                    if (!dd) return;
                    auto* v = (CFNumberRef)CFDictionaryGetValue(dd, uk);
                    if (!v) v = (CFNumberRef)CFDictionaryGetValue(dd, ck);
                    if (!v) return;
                    double secs = 0;
                    CFNumberGetValue(v, kCFNumberDoubleType, &secs);
                    if (secs > 0) delay_ms = static_cast<int>(secs * 1000.0);
                };
                try_delay(kCGImagePropertyGIFDictionary,
                          kCGImagePropertyGIFUnclampedDelayTime,
                          kCGImagePropertyGIFDelayTime);
                try_delay(kCGImagePropertyPNGDictionary,
                          kCGImagePropertyAPNGUnclampedDelayTime,
                          kCGImagePropertyAPNGDelayTime);
                if (@available(macOS 11.0, *)) {
                    try_delay(kCGImagePropertyWebPDictionary,
                              kCGImagePropertyWebPDelayTime,
                              kCGImagePropertyWebPDelayTime);
                }
                CFRelease(props);
            }
            d.delays_ms.push_back(std::max(delay_ms, 20));
        }
        if (!d.frames.empty()) { CFRelease(src); return d; }
        d.delays_ms.clear();
        CFRelease(src);
        CFDataRef data2 = CFDataCreate(kCFAllocatorDefault, bytes.data(),
                                       static_cast<CFIndex>(bytes.size()));
        if (!data2) return d;
        src = CGImageSourceCreateWithData(data2, nullptr);
        CFRelease(data2);
        if (!src) return d;
    }
    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nullptr);
    CFRelease(src);
    if (!img) return d;
    d.still = tk::cg::make_image(img);
    CGImageRelease(img);
    return d;
}

std::int64_t MacShell::monotonic_ms_() {
    return static_cast<std::int64_t>(
        [[NSDate date] timeIntervalSince1970] * 1000.0);
}

void MacShell::start_anim_tick_() {
    if (ctrl_) [ctrl_ _startAnimTickIfNeeded];
}

void MacShell::repaint_pickers_() {
    if (ctrl_) [ctrl_ _relayoutChatSurface];
    EmojiPickerPanel* ep = [EmojiPickerPanel sharedPanel];
    if (ep.isVisible) [ep invalidateImageCache];
    StickerPickerPanel* sp = [StickerPickerPanel sharedPanel];
    if (sp.isVisible) [sp invalidateImageCache];
}
```

- [ ] **Step 3: Rewrite `_decodeMediaBytes:forKey:` to delegate to `decode_image_` (DRY)**

In `ui/macos/src/MainWindowController.mm`, replace the body of `- (void)_decodeMediaBytes:(const std::vector<uint8_t>&)bytes forKey:(const std::string&)key` (lines 3114-3191) with:

```cpp
- (void)_decodeMediaBytes:(const std::vector<uint8_t>&)bytes
                   forKey:(const std::string&)key {
    if (bytes.empty() || _shell->tk_images_.count(key)
        || _shell->anim_cache_.has(key)) return;
    auto d = _shell->decode_image_(bytes, 0, 0);
    if (!d.frames.empty()) {
        const std::int64_t now = static_cast<std::int64_t>(
            [[NSDate date] timeIntervalSince1970] * 1000.0);
        _shell->anim_cache_.store(key, std::move(d.frames),
                                  std::move(d.delays_ms), now);
        [self _startAnimTickIfNeeded];
    } else if (d.still) {
        _shell->tk_images_.emplace(key, std::move(d.still));
    }
}
```

(`MacShell` exposes `decode_image_`/`tk_images_`/`anim_cache_` to ObjC++ via the existing `public:` `using` declarations in `MacShell` — if `decode_image_` is not already re-exposed there, add `using ShellBase::decode_image_;` alongside the existing `using ShellBase::tk_images_;`.)

- [ ] **Step 4: Route the async fetchers through the shared helper**

In `ui/macos/src/MainWindowController.mm`, replace the body of `- (void)_ensureEmojiImageAsync:(std::string)url` (lines 3244-3266) with:

```cpp
- (void)_ensureEmojiImageAsync:(std::string)url {
    _shell->ensure_picker_image_(url, /*is_sticker=*/false);
}
```

and `- (void)_ensureStickerImageAsync:(std::string)url` (lines 3220-3242) with:

```cpp
- (void)_ensureStickerImageAsync:(std::string)url {
    _shell->ensure_picker_image_(url, /*is_sticker=*/true);
}
```

(`ensure_picker_image_` must be reachable from ObjC++: add `using ShellBase::ensure_picker_image_;` to the `MacShell` `public:` `using` block if not already present.)

- [ ] **Step 5: Build (best-effort) and commit**

If a macOS host is available:
```bash
cmake --build build/macos-appkit-arm64-debug 2>&1 | grep -E "error:" | head -20
```
Expected: no errors. If no macOS host is available, do a careful read-through of the diff against the Qt6/GTK4 equivalents for parity, then commit.

```bash
git add ui/macos/src/MainWindowController.mm
git commit -m "feat(macos): pickers share async+disk-cached image cache; off-thread decode"
```

---

## Task 8: Win32 — free off-thread decoder + implement the four virtuals

**Files:**
- Modify: `ui/shared/tk/canvas_d2d.h` / `ui/shared/tk/canvas_d2d.cpp`
- Modify: `ui/shared/tk/host_win32.h`
- Modify: `ui/windows/src/MainWindow.h` / `ui/windows/src/MainWindow.cpp`

- [ ] **Step 1: Add a free single-frame WIC decoder symmetric with `decode_animation`**

In `ui/shared/tk/canvas_d2d.h`, immediately **before** the `make_image_from_bgra` declaration (line 117), add:

```cpp
// Decode a single-frame encoded image (PNG/JPEG/WebP/…) into a tk::Image.
// Pure WIC (free-threaded): callable off the UI thread; the resulting
// D2DImage holds a device-independent IWICBitmap and uploads at paint.
// Returns nullptr on failure / on a multi-frame image.
std::unique_ptr<Image> decode_image(Backend& backend,
                                     std::span<const std::uint8_t> bytes);
```

In `ui/shared/tk/canvas_d2d.cpp`, immediately **before** the `decode_animation(` definition (line 1103), add the body lifted verbatim from `CanvasFactory::decode_image` (lines 799-847), retargeted to a free function on `Backend&`:

```cpp
std::unique_ptr<Image> decode_image(Backend& b,
                                     std::span<const std::uint8_t> bytes) {
    if (bytes.empty()) return nullptr;
    Backend::Impl& impl = b.impl();

    ComPtr<IWICStream> stream;
    if (FAILED(impl.wic->CreateStream(stream.GetAddressOf())))
        return nullptr;
    if (FAILED(stream->InitializeFromMemory(
            const_cast<BYTE*>(bytes.data()),
            static_cast<DWORD>(bytes.size()))))
        return nullptr;

    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(impl.wic->CreateDecoderFromStream(
            stream.Get(), nullptr,
            WICDecodeMetadataCacheOnLoad,
            decoder.GetAddressOf())))
        return nullptr;

    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf())))
        return nullptr;

    ComPtr<IWICFormatConverter> converter;
    if (FAILED(impl.wic->CreateFormatConverter(converter.GetAddressOf())))
        return nullptr;
    if (FAILED(converter->Initialize(
            frame.Get(), GUID_WICPixelFormat32bppPBGRA,
            WICBitmapDitherTypeNone, nullptr, 0.0f,
            WICBitmapPaletteTypeMedianCut)))
        return nullptr;

    ComPtr<IWICBitmap> cached;
    if (FAILED(impl.wic->CreateBitmapFromSource(
            converter.Get(), WICBitmapCacheOnLoad,
            cached.GetAddressOf())))
        return nullptr;

    UINT w = 0, h = 0;
    cached->GetSize(&w, &h);
    return std::make_unique<D2DImage>(std::move(cached),
                                      static_cast<int>(w),
                                      static_cast<int>(h));
}
```

Have `CanvasFactory::decode_image` (the method, lines 799-847) delegate to the free function to remove the duplication: replace its body with `return tk::d2d::decode_image(backend_, bytes);`.

- [ ] **Step 2: Expose `backend_singleton()` for off-thread use**

In `ui/shared/tk/host_win32.h`, in `namespace tk::win32`, add a declaration (the definition is the existing file-local one in `host_win32.cpp:38`; promote it to external linkage by removing any `static`/anonymous-namespace qualifier on that definition so the header declaration links):

```cpp
// Process-wide D2D backend. WIC factory is free-threaded; safe to use
// from worker threads for decode_image / decode_animation.
tk::d2d::Backend& backend_singleton();
```

- [ ] **Step 3: Declare the overrides on the Win32 `MainWindow`**

In `ui/windows/src/MainWindow.h`, near the `on_media_bytes_ready_` / `cache_rgba_image_` overrides, add:

```cpp
    DecodedImage decode_image_(const std::vector<uint8_t>& bytes,
                               int max_w, int max_h) override;
    std::int64_t monotonic_ms_() override;
    void         start_anim_tick_() override;
    void         repaint_pickers_() override;
```

- [ ] **Step 4: Implement the overrides**

In `ui/windows/src/MainWindow.cpp`, add (uses `tk::win32::backend_singleton()` — pure WIC, off-thread-safe; mirrors the `video_win32.cpp` off-thread → IWICBitmap pattern):

```cpp
ShellBase::DecodedImage
MainWindow::decode_image_(const std::vector<uint8_t>& bytes,
                          int /*max_w*/, int /*max_h*/) {
    DecodedImage d;
    if (bytes.empty()) return d;
    auto& backend = tk::win32::backend_singleton();
    std::span<const std::uint8_t> span(bytes.data(), bytes.size());
    auto frames = tk::d2d::decode_animation(backend, span);
    if (frames.size() >= 2) {
        d.frames.reserve(frames.size());
        d.delays_ms.reserve(frames.size());
        for (auto& af : frames) {
            d.frames.push_back(std::move(af.image));
            d.delays_ms.push_back(af.delay_ms);
        }
        return d;
    }
    d.still = tk::d2d::decode_image(backend, span);
    return d;
}

std::int64_t MainWindow::monotonic_ms_() {
    return static_cast<std::int64_t>(GetTickCount64());
}

void MainWindow::start_anim_tick_() {
    if (!anim_timer_running_ && hwnd_) {
        SetTimer(hwnd_, kAnimTimerId, kAnimTimerHz, nullptr);
        anim_timer_running_ = true;
    }
}

void MainWindow::repaint_pickers_() {
    if (room_view_) room_view_->notify_image_ready({});
    if (main_app_surface_) {
        main_app_surface_->relayout();
        if (HWND h = main_app_surface_->hwnd())
            InvalidateRect(h, nullptr, FALSE);
    }
    if (emoji_picker_shared_)   emoji_picker_shared_->invalidate_image_cache();
    if (sticker_picker_shared_) sticker_picker_shared_->invalidate_image_cache();
    if (emoji_picker_surface_ && emoji_picker_surface_->hwnd())
        InvalidateRect(emoji_picker_surface_->hwnd(), nullptr, FALSE);
    if (sticker_picker_surface_ && sticker_picker_surface_->hwnd())
        InvalidateRect(sticker_picker_surface_->hwnd(), nullptr, FALSE);
}
```

If `notify_image_ready({})` is not a valid "repaint all" call on `room_view_`, drop that line — the `main_app_surface_->relayout()` + `InvalidateRect` already force a repaint; keep `repaint_pickers_` minimal.

- [ ] **Step 5: Route the picker providers + delete `request_sticker_image`**

In `ui/windows/src/MainWindow.cpp`, replace the EmojiPicker provider lambda (in `ensure_emoji_picker_created`, lines 3142-3149) and the StickerPicker provider lambda (in `ensure_sticker_picker_created`, lines 3450-3457) so the on-miss call becomes the shared helper:

```cpp
    emoji_picker_shared_->set_image_provider(
        [this](const std::string& cache_key,
                const std::string& /*source_token*/) -> const tk::Image* {
            if (auto* f = anim_cache_.current_frame(cache_key)) return f;
            auto sit = tk_images_.find(cache_key);
            if (sit != tk_images_.end()) return sit->second.get();
            ensure_picker_image_(cache_key, /*is_sticker=*/false);
            return nullptr;
        });
```

```cpp
    sticker_picker_shared_->set_image_provider(
        [this](const std::string& cache_key,
                const std::string& /*source_token*/) -> const tk::Image* {
            if (auto* f = anim_cache_.current_frame(cache_key)) return f;
            auto sit = tk_images_.find(cache_key);
            if (sit != tk_images_.end()) return sit->second.get();
            ensure_picker_image_(cache_key, /*is_sticker=*/true);
            return nullptr;
        });
```

Then delete the `MainWindow::request_sticker_image` method and its `WM_TESSERACT_STICKER_BYTES` message handler + the message-id `#define`/`constexpr` and its `case` in `wnd_proc`. Confirm no remaining references:

```bash
grep -rn "request_sticker_image\|WM_TESSERACT_STICKER_BYTES" ui/windows/
```
Expected: no matches after deletion.

- [ ] **Step 6: Build (best-effort) and commit**

If a Windows / MinGW cross toolchain is available:
```bash
cmake --build build/windows-debug 2>&1 | grep -E "error:" | head -20
```
Expected: no errors. Otherwise diff-review for parity with the Qt6/GTK4 tasks, then commit.

```bash
git add ui/shared/tk/canvas_d2d.h ui/shared/tk/canvas_d2d.cpp \
        ui/shared/tk/host_win32.h \
        ui/windows/src/MainWindow.h ui/windows/src/MainWindow.cpp
git commit -m "feat(win32): pickers share async+disk-cached image cache; off-thread WIC decode"
```

---

## Task 9: Full build, tests, manual verification, docs

- [ ] **Step 1: Full build + test on the primary preset**

```bash
cmake --build build/linux-qt6-debug 2>&1 | tail -5
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -15
cargo test -p tesseract-sdk-ffi 2>&1 | tail -5
```
Expected: build succeeds; all tests pass including `[picker-cache]`.

- [ ] **Step 2: Build the GTK4 preset too**

```bash
cmake --preset linux-gtk-debug >/dev/null 2>&1
cmake --build build/linux-gtk-debug 2>&1 | grep -E "error:" | tail -5
```
Expected: no errors.

- [ ] **Step 3: Manual verification (Qt6 + GTK4)**

Run `./build/linux-qt6-debug/ui/linux-qt/tesseract`:

1. Open the sticker picker in a room with a custom MSC2545 pack. Custom stickers load (no permanent grey placeholders); animated stickers animate.
2. Open the emoji picker, switch to a custom emoticon pack tab — emoticons load.
3. Send one of those stickers; it appears in the message list **without a second fetch** (shared `tk_images_`/`anim_cache_` — watch there is no second network hit; the disk cache or memory cache serves it).
4. Close and reopen the picker — images appear **instantly** (memory cache; no re-decode).
5. Quit and relaunch the app, open the same picker — images appear **without re-downloading** (disk cache `~/.cache/tesseract/media`). Verify by checking files exist: `ls ~/.cache/tesseract/media | head`.
6. While a large pack first paints, the UI stays responsive (decode is off the UI thread).

Repeat 1–6 with the GTK4 build (`./build/linux-gtk-debug/ui/linux-gtk/tesseract`).

- [ ] **Step 4: Update CHANGES.md**

In `CHANGES.md`, under `## Unreleased` → today's `### 2026-05-18` section (create the `## Unreleased` + date heading if absent, per the file's existing convention), add:

```markdown
- feat(pickers): unified async image cache — EmojiPicker/StickerPicker now share the message-list `tk_images_`/`anim_cache_` on all four shells (Qt6 dropped its private per-picker caches), images route through `media_disk_cache_` so custom emoticons/stickers survive an app restart, and decode runs off the UI thread (Qt6 QImageReader / GTK4 GdkPixbuf+cairo / macOS CGImageSource / Win32 WIC) so first paint of a large pack no longer stalls
```

- [ ] **Step 5: Update STATUS.md**

In `STATUS.md`: bump `Last updated` to `2026-05-18`, refresh the test count (+1 test file / +4 cases), and add a short note under the media/pickers area that picker images are now shared, disk-persisted, and decoded off-thread.

- [ ] **Step 6: Commit docs**

```bash
git add CHANGES.md STATUS.md
git commit -m "docs: record unified async picker image cache"
```

---

## Task 10: Full code-review pass

After all tasks are committed, review every changed file against the goal before declaring done.

- [ ] **Step 1: Shared core**

Read `ShellBase.h`/`ShellBase.cpp` changes. Verify: `ensure_picker_image_` releases the worker before `post_to_ui_` (it uses `run_async_`, which the existing `ensure_media_image_` mirrors); the in-flight key is erased on **every** exit path (empty bytes, decode-fail, success); `finalize_picker_image_` never overwrites an existing `tk_images_`/`anim_cache_` entry; `DecodedImage` invariant (still XOR frames) holds in every shell's `decode_image_`.

- [ ] **Step 2: Qt6**

Confirm `EmojiPicker.h`/`StickerPicker.h` have **no** remaining `image_cache_`/`animated_cache_`/`fetches_in_flight_`/`anim_timer_`/`imageLoadedSignal_`/`onImageLoaded_`/`onAnimTick_`/`request_image_`. Confirm `grep -rn "image_cache_\|imageLoadedSignal_" ui/linux-qt/src/EmojiPicker.* ui/linux-qt/src/StickerPicker.*` is empty. Confirm `on_media_bytes_ready_` MediaImage branch now calls `decode_image_` and the `RoomAvatar`/`UserAvatar`/`Tile` branches are unchanged. Confirm the shared `tk_anim_timer_` still drives sticker animation now that the per-picker `anim_timer_` is gone.

- [ ] **Step 3: GTK4 / macOS / Win32**

Confirm each shell's `decode_image_` performs **no** cache mutation and is genuinely thread-safe (no GTK/AppKit-main-only / D2D-device calls — only GdkPixbuf+cairo / CGImageSource / WIC). Confirm `monotonic_ms_()` returns the **same clock** that shell's anim timer/`anim_cache_.advance()` uses (GTK `g_get_monotonic_time/1000`; macOS `NSDate*1000`; Win32 `GetTickCount64`; Qt `QDateTime` msecs) — a mismatched epoch makes animation freeze or spin. Confirm GTK4's `ensure_emoji_/sticker_image_async` and Win32's `request_sticker_image` are fully deleted with no dangling references.

- [ ] **Step 4: Fix issues found and re-verify**

```bash
cmake --build build/linux-qt6-debug 2>&1 | grep "error:" | head
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -5
```
Expected: all pass. Commit any fixes.

---

## Self-Review (author)

- **Scope coverage:** unify (Tasks 4–8 repoint all four shells onto shared `tk_images_`/`anim_cache_`; Qt6 private caches deleted in Task 5) ✓; disk persistence (Task 2 `ensure_picker_image_` calls `media_disk_cache_.load/store` — every shell now inherits it) ✓; off-thread decode (Task 2 calls `decode_image_` inside `run_async_`; Tasks 4–8 implement thread-safe per-shell `decode_image_`) ✓.
- **Placeholder scan:** all code steps contain full bodies; the only conditional ("if no macOS/Windows host") gives an explicit alternative action (diff-review for parity) — not a TODO.
- **Type consistency:** `DecodedImage` (Task 1) fields `still`/`frames`/`delays_ms`/`empty()` used identically in Tasks 2, 3, 6, 7, 8. `decode_image_(bytes,max_w,max_h)`, `monotonic_ms_()`, `start_anim_tick_()`, `repaint_pickers_()`, `ensure_picker_image_(url,is_sticker)`, `finalize_picker_image_(url,is_sticker,d)` signatures match across all tasks.
- **Risk note:** `on_media_bytes_ready_` message-list threading is deliberately unchanged (decode stays on the UI thread there; only the *function* is shared for DRY) — bounded blast radius. Win32/macOS build steps are best-effort with a diff-review fallback when no host is available.
