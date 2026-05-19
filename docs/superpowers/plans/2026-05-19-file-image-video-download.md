# File / Image / Video Download Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let users save file attachments and download images/videos to disk across all four platform shells (Qt6, GTK4, Win32, macOS).

**Architecture:** Three entry points — clicking a file card fires `on_file_clicked` (new, in `MessageListView`); a new ⬇ save button painted in `ImageViewerOverlay` fires `on_save`; a parallel save button in `VideoViewerOverlay` fires `on_save`. Each shell shows a native save dialog, then fetches bytes on a worker thread via the existing `Client::fetch_media_bytes` / `fetch_source_bytes`, and writes to disk with `std::ofstream`. No Rust or SDK changes are required.

**Tech Stack:** C++17, `<fstream>`, `<filesystem>` (for path stem/extension helpers); Qt6 `QFileDialog`; GTK4 `gtk_file_chooser_native`; Win32 `GetSaveFileNameW` (OPENFILENAMEW); macOS `NSSavePanel`.

---

## File Map

| File | Change |
|---|---|
| `ui/shared/views/MessageListView.h` | Add `FileHit` struct, `file_geom_` map, `press_file_` state, `on_file_clicked`, `file_hit_at()` |
| `ui/shared/views/MessageListView.cpp` | Record `file_geom_` in paint; clear in `paint()`; hit-test in `on_pointer_down`/`up`/`move` |
| `ui/shared/views/ImageViewerOverlay.h` | Add `on_save` callback, `save_btn_` rect, `press_save_` flag |
| `ui/shared/views/ImageViewerOverlay.cpp` | Paint ⬇ save button; handle press/release |
| `ui/shared/views/VideoViewerOverlay.h` | Add `on_save` callback, `save_btn_` rect, `press_save_` flag |
| `ui/shared/views/VideoViewerOverlay.cpp` | Paint ⬇ save button; handle press/release |
| `ui/linux-qt/src/MainWindow.cpp` | Wire `on_file_clicked`, image `on_save`, video `on_save` |
| `ui/linux-gtk/src/MainWindow.cpp` | Same for GTK4 shell |
| `ui/windows/src/MainWindow.h` | Add `WM_TESSERACT_FILE_BYTES` message constant |
| `ui/windows/src/MainWindow.cpp` | Wire all three download paths via PostMessage pattern |
| `ui/macos/src/MainWindowController.mm` | Wire all three download paths via `NSSavePanel` |

---

## Background: existing patterns to replicate

**ImageHit / VideoHit pattern (in `MessageListView`):**
- A struct is defined in `MessageListView.h` with `event_id`, URL fields, size, and `world_rect`.
- `mutable std::unordered_map<std::string, XxxHit> xxx_geom_` is declared in the private section (~line 594-601).
- The geometry map is populated just after the paint call in `Adapter::paint_body_block`.
- `image_geom_`, `video_geom_` (and others) are **cleared at the top of `paint()`** (~line 4763).
- `on_pointer_down` iterates the map with AABB containment to set `press_video_ / press_image_` and the matching `eid`.
- `on_pointer_up` fires the callback if `inside_self` and the pointer is still over the saved rect.
- `on_pointer_move` uses `on_link_hovered` with a non-URL sentinel (`"map://"`) to trigger a pointing-hand cursor for non-link clickable areas. We reuse `"file://"` the same way.

**ImageViewerOverlay close button (the template for the save button):**
- `kCloseBtnS = 36.0f`; positioned at `{b.x + b.w - (kCloseBtnS + 8.0f), b.y + 8.0f, kCloseBtnS, kCloseBtnS}`.
- Painted with `fill_rounded_rect` + `"\xC3\x97"` (UTF-8 ×) label.
- `press_close_` flag set in `on_pointer_down`, cleared and action fired in `on_pointer_up`.
- Save button goes **to the left**: `{close_btn_.x - kCloseBtnS - 4.0f, b.y + 8.0f, kCloseBtnS, kCloseBtnS}`.
- Label: `"\xe2\xac\x87"` (UTF-8 ⬇, U+2B07 DOWNWARDS BLACK ARROW).

**Shell async dispatch:**
- **Qt6**: `runOnPool_(fn)` → `QMetaObject::invokeMethod(this, fn, Qt::QueuedConnection)` to post back.
- **GTK4**: `run_async_(fn)` (from `ShellBase`) → `g_idle_add(callback, ctx_ptr)` to post back.
- **Win32**: `run_async_(fn)` → `PostMessageW(hwnd_, WM_TESSERACT_FILE_BYTES, 0, (LPARAM)new_payload)`.
- **macOS**: `_shell->run_async_(fn)` → `dispatch_async(dispatch_get_main_queue(), ^{ ... })`.

---

### Task 1: FileHit + file_geom_ in MessageListView

**Files:**
- Modify: `ui/shared/views/MessageListView.h` (around lines 390-414 for struct, 594-601 for maps)
- Modify: `ui/shared/views/MessageListView.cpp` (lines 1790-1795, ~4263-4295, ~4608-4650, 4758-4765, ~3960)

> These are pure-rendering changes with no clean unit-test seam — correctness verified by compile + visual check.

- [ ] **Step 1: Add `FileHit` struct and callback to `MessageListView.h`**

Add after the `VideoHit` / `on_video_clicked` block (around line 414):

```cpp
// File card left-click hit — fires `on_file_clicked`.
// `media_url` is the plain mxc:// URI; pass to
// `Client::fetch_media_bytes`. `file_name` is the suggested save name.
struct FileHit
{
    std::string event_id;
    std::string media_url; // mxc:// URI — pass to fetch_media_bytes
    std::string file_name; // suggested save filename
    uint64_t    file_size = 0;
    tk::Rect    world_rect;
};
std::optional<FileHit> file_hit_at(tk::Point world) const;

/// Fires when the user left-clicks a file card.
std::function<void(const MessageListView::FileHit&)> on_file_clicked;
```

- [ ] **Step 2: Add `file_geom_` map and press state to private section of `MessageListView.h`**

Add after the `video_geom_` block (around line 601):

```cpp
// File card click-to-download press state.
mutable std::unordered_map<std::string, FileHit> file_geom_;
bool press_file_ = false;
std::string press_file_eid_;
```

- [ ] **Step 3: Record `file_geom_` in `Adapter::paint_body_block` in `MessageListView.cpp`**

The file card render is at lines 1792-1795. Change it to:

```cpp
case MessageRowData::Kind::File:
{
    float card_w = std::min(kFileCardW, col_w);
    tk::Rect r{x, y, card_w, kFileCardH};
    paint_file_card(m, ctx, r);
    if (!m.event_id.empty())
    {
        owner_.file_geom_[m.event_id] =
            MessageListView::FileHit{m.event_id, m.media_url,
                                      m.file_name, m.file_size, r};
    }
    return y + kFileCardH;
}
```

- [ ] **Step 4: Clear `file_geom_` in `paint()` alongside the other geometry maps**

In `MessageListView::paint()` (~line 4763), add:

```cpp
file_geom_.clear();
```

immediately after `video_geom_.clear();`.

- [ ] **Step 5: Add hit-test in `on_pointer_down` for file cards**

After the `press_image_` block (~line 4291), add:

```cpp
// File card click-to-download hit-test.
{
    tk::Point world{local.x + bounds().x, local.y + bounds().y};
    for (const auto& [eid, hit] : file_geom_)
    {
        if (rect_contains(hit.world_rect, world))
        {
            press_file_ = true;
            press_file_eid_ = eid;
            return true;
        }
    }
}
```

- [ ] **Step 6: Fire `on_file_clicked` in `on_pointer_up`**

After the `press_image_` block in `on_pointer_up` (~line 4642), add:

```cpp
if (press_file_)
{
    bool fire = inside_self && !press_file_eid_.empty();
    std::string eid = std::move(press_file_eid_);
    press_file_ = false;
    press_file_eid_.clear();
    if (fire)
    {
        tk::Point world{local.x + bounds().x, local.y + bounds().y};
        auto it = file_geom_.find(eid);
        if (it != file_geom_.end() &&
            rect_contains(it->second.world_rect, world) && on_file_clicked)
        {
            on_file_clicked(it->second);
        }
    }
    return;
}
```

- [ ] **Step 7: Implement `file_hit_at()` in `MessageListView.cpp`**

Add after `video_hit_at()` (~line 2980):

```cpp
std::optional<MessageListView::FileHit>
MessageListView::file_hit_at(tk::Point world) const
{
    for (const auto& [eid, hit] : file_geom_)
    {
        if (world.x >= hit.world_rect.x && world.y >= hit.world_rect.y &&
            world.x < hit.world_rect.x + hit.world_rect.w &&
            world.y < hit.world_rect.y + hit.world_rect.h)
        {
            return hit;
        }
    }
    return std::nullopt;
}
```

- [ ] **Step 8: Show pointing-hand cursor when hovering a file card**

In `on_pointer_move`, inside the block that builds `new_link_url` (after the inline hyperlink check, ~line 3940), add:

```cpp
// File card hover: reuse on_link_hovered with a "file://" sentinel
// so the shell switches to the pointing-hand cursor.
if (new_link_url.empty())
{
    tk::Point world{local.x + bounds().x, local.y + bounds().y};
    for (const auto& [eid, hit] : file_geom_)
    {
        if (rect_contains(hit.world_rect, world))
        {
            new_link_url = "file://";
            break;
        }
    }
}
```

- [ ] **Step 9: Build and verify compile**

```bash
cmake --build build/linux-qt6-debug --target tesseract 2>&1 | tail -5
```

Expected: 0 errors, a few "no tests" notes at most.

- [ ] **Step 10: Commit**

```bash
git add ui/shared/views/MessageListView.h ui/shared/views/MessageListView.cpp
git commit -m "feat(ui): FileHit + file_geom_ + on_file_clicked in MessageListView"
```

---

### Task 2: Save button in ImageViewerOverlay

**Files:**
- Modify: `ui/shared/views/ImageViewerOverlay.h`
- Modify: `ui/shared/views/ImageViewerOverlay.cpp`

- [ ] **Step 1: Add `on_save`, `save_btn_`, and `press_save_` to `ImageViewerOverlay.h`**

After `std::function<void()> on_close;` (line 54), add:

```cpp
/// Fires when the user clicks the ⬇ save button.
/// `source_url` is the media_url_ (mxc:// or source JSON).
/// `filename_hint` is the body_ (MSC2530 filename caption, or empty).
std::function<void(std::string source_url, std::string filename_hint)>
    on_save;
```

In the private section after `tk::Rect close_btn_{};` (line 102), add:

```cpp
tk::Rect save_btn_{};
bool press_save_ = false;
```

- [ ] **Step 2: Position `save_btn_` alongside `close_btn_` in `ImageViewerOverlay.cpp`**

Both places where `close_btn_` is set (lines 71-72 and 141-142), add a matching `save_btn_` immediately after. The save button sits 4 px to the left of the close button:

```cpp
// existing close button position
close_btn_ = {b.x + b.w - (kCloseBtnS + 8.0f), b.y + 8.0f, kCloseBtnS, kCloseBtnS};
// new save button — left of close button
save_btn_  = {close_btn_.x - kCloseBtnS - 4.0f, b.y + 8.0f, kCloseBtnS, kCloseBtnS};
```

- [ ] **Step 3: Paint the save button**

In `ImageViewerOverlay.cpp`'s paint method, after the `// × close button` block (~line 183), add:

```cpp
// ⬇ save button
cv.fill_rounded_rect(save_btn_, kCloseBtnS * 0.5f,
                     tk::Color{0, 0, 0, 160});
{
    tk::TextStyle st;
    st.font_size = 16.0f;
    st.color = tk::Color{255, 255, 255, 255};
    st.max_width = kCloseBtnS;
    auto lo = ctx.factory.build_text("\xe2\xac\x87", st); // UTF-8 ⬇
    auto sz = lo->measure();
    float tx = save_btn_.x + (save_btn_.w - sz.w) * 0.5f;
    float ty = save_btn_.y + (save_btn_.h - sz.h) * 0.5f;
    ctx.canvas.draw_text(*lo, {tx, ty}, st.color);
}
```

- [ ] **Step 4: Handle `press_save_` in `on_pointer_down`**

In `on_pointer_down`, after the `press_close_` detection (~line 212), add:

```cpp
if (rect_contains(save_btn_, w))
{
    press_save_ = true;
    return true;
}
```

- [ ] **Step 5: Fire `on_save` in `on_pointer_up`**

In `on_pointer_up`, after the `press_close_` block (~line 241), add:

```cpp
if (press_save_)
{
    press_save_ = false;
    if (inside_self && rect_contains(save_btn_, w) && on_save)
    {
        on_save(media_url_, body_);
    }
    return;
}
```

- [ ] **Step 6: Build and verify compile**

```bash
cmake --build build/linux-qt6-debug --target tesseract 2>&1 | tail -5
```

- [ ] **Step 7: Commit**

```bash
git add ui/shared/views/ImageViewerOverlay.h ui/shared/views/ImageViewerOverlay.cpp
git commit -m "feat(ui): add save button to ImageViewerOverlay"
```

---

### Task 3: Save button in VideoViewerOverlay

**Files:**
- Modify: `ui/shared/views/VideoViewerOverlay.h`
- Modify: `ui/shared/views/VideoViewerOverlay.cpp`

Pattern is identical to Task 2. The viewer stores `source_json_` and `mime_type_`.

- [ ] **Step 1: Add `on_save`, `save_btn_`, and `press_save_` to `VideoViewerOverlay.h`**

After `std::function<void()> on_close;` (line 61), add:

```cpp
/// Fires when the user clicks the ⬇ save button.
/// `source_json` is either a plain mxc:// URI or a JSON MediaSource blob;
/// pass to `Client::fetch_source_bytes`. `mime_type` is e.g. "video/mp4".
std::function<void(std::string source_json, std::string mime_type)> on_save;
```

In the private section after `tk::Rect close_btn_{};` (line 100), add:

```cpp
tk::Rect save_btn_{};
bool press_save_ = false;
```

- [ ] **Step 2: Position `save_btn_` in `VideoViewerOverlay.cpp`**

After the `close_btn_` assignment (~line 190), add:

```cpp
save_btn_ = {close_btn_.x - kCloseBtnS - 4.0f, b.y + 8.0f, kCloseBtnS, kCloseBtnS};
```

- [ ] **Step 3: Paint the save button**

After the `// × close button` paint block (~line 365), add (identical to Task 2 Step 3):

```cpp
// ⬇ save button
cv.fill_rounded_rect(save_btn_, kCloseBtnS * 0.5f,
                     tk::Color{0, 0, 0, 160});
{
    tk::TextStyle st;
    st.font_size = 16.0f;
    st.color = tk::Color{255, 255, 255, 255};
    st.max_width = kCloseBtnS;
    auto lo = ctx.factory.build_text("\xe2\xac\x87", st); // UTF-8 ⬇
    auto sz = lo->measure();
    float tx = save_btn_.x + (save_btn_.w - sz.w) * 0.5f;
    float ty = save_btn_.y + (save_btn_.h - sz.h) * 0.5f;
    ctx.canvas.draw_text(*lo, {tx, ty}, st.color);
}
```

- [ ] **Step 4: Handle `press_save_` in `on_pointer_down`**

After the `press_close_` detection in `on_pointer_down` (~line 395), add:

```cpp
if (rect_contains(save_btn_, w))
{
    press_save_ = true;
    return true;
}
```

- [ ] **Step 5: Fire `on_save` in `on_pointer_up`**

After the `press_close_` block in `on_pointer_up` (~line 444), add:

```cpp
if (press_save_)
{
    press_save_ = false;
    if (inside_self && rect_contains(save_btn_, w) && on_save)
    {
        on_save(source_json_, mime_type_);
    }
    return;
}
```

- [ ] **Step 6: Build and verify compile**

```bash
cmake --build build/linux-qt6-debug --target tesseract 2>&1 | tail -5
```

- [ ] **Step 7: Commit**

```bash
git add ui/shared/views/VideoViewerOverlay.h ui/shared/views/VideoViewerOverlay.cpp
git commit -m "feat(ui): add save button to VideoViewerOverlay"
```

---

### Task 4: Qt6 shell — wire file, image, and video download

**Files:**
- Modify: `ui/linux-qt/src/MainWindow.cpp`

The download helper lambda writes bytes to a chosen path. Pattern: `QFileDialog::getSaveFileName` on the UI thread → `runOnPool_` for the fetch → `QMetaObject::invokeMethod` to write on the UI thread.

Add `#include <QFileDialog>` and `#include <fstream>` near the top of `MainWindow.cpp` if not already present.

- [ ] **Step 1: Wire `on_file_clicked` in `MainWindow.cpp`**

Find where `on_image_clicked` and `on_video_clicked` are wired (~line 746). After the `on_video_clicked` block, add:

```cpp
mainApp_->room_view()->on_file_clicked =
    [this](const tesseract::views::MessageListView::FileHit& hit)
{
    std::string suggested = hit.file_name.empty() ? "download" : hit.file_name;
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save file"),
        QString::fromStdString(suggested),
        tr("All files (*.*)"));
    if (path.isEmpty())
        return;
    std::string url  = hit.media_url;
    std::string dest = path.toStdString();
    runOnPool_(
        [this, url, dest]()
        {
            auto bytes = client_->fetch_media_bytes(url);
            QMetaObject::invokeMethod(
                this,
                [dest, bytes = std::move(bytes)]() mutable
                {
                    if (bytes.empty())
                        return;
                    std::ofstream f(dest, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(bytes.data()),
                            static_cast<std::streamsize>(bytes.size()));
                },
                Qt::QueuedConnection);
        });
};
```

- [ ] **Step 2: Wire `image_viewer()->on_save`**

After the `on_image_clicked` wiring block (~line 804), add:

```cpp
mainApp_->image_viewer()->on_save =
    [this](std::string source_url, std::string filename_hint)
{
    std::string suggested =
        filename_hint.empty() ? "image" : filename_hint;
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save image"),
        QString::fromStdString(suggested),
        tr("Images (*.jpg *.jpeg *.png *.gif *.webp);;All files (*.*)"));
    if (path.isEmpty())
        return;
    std::string dest = path.toStdString();
    runOnPool_(
        [this, source_url = std::move(source_url), dest]()
        {
            auto bytes = client_->fetch_source_bytes(source_url);
            QMetaObject::invokeMethod(
                this,
                [dest, bytes = std::move(bytes)]() mutable
                {
                    if (bytes.empty())
                        return;
                    std::ofstream f(dest, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(bytes.data()),
                            static_cast<std::streamsize>(bytes.size()));
                },
                Qt::QueuedConnection);
        });
};
```

- [ ] **Step 3: Wire `video_viewer()->on_save`**

After the `on_video_clicked` wiring block (and before or after the `on_file_clicked` block you just added), add:

```cpp
mainApp_->video_viewer()->on_save =
    [this](std::string source_json, std::string mime_type)
{
    // Derive extension from mime_type (e.g. "video/mp4" → ".mp4")
    std::string ext = ".mp4";
    auto slash = mime_type.find('/');
    if (slash != std::string::npos)
        ext = "." + mime_type.substr(slash + 1);
    QString path = QFileDialog::getSaveFileName(
        this, tr("Save video"),
        QString::fromStdString("video" + ext),
        tr("Videos (*.mp4 *.webm *.mkv);;All files (*.*)"));
    if (path.isEmpty())
        return;
    std::string dest = path.toStdString();
    runOnPool_(
        [this, source_json = std::move(source_json), dest]()
        {
            auto bytes = client_->fetch_source_bytes(source_json);
            QMetaObject::invokeMethod(
                this,
                [dest, bytes = std::move(bytes)]() mutable
                {
                    if (bytes.empty())
                        return;
                    std::ofstream f(dest, std::ios::binary);
                    f.write(reinterpret_cast<const char*>(bytes.data()),
                            static_cast<std::streamsize>(bytes.size()));
                },
                Qt::QueuedConnection);
        });
};
```

- [ ] **Step 4: Build and verify compile**

```bash
cmake --build build/linux-qt6-debug --target tesseract 2>&1 | tail -5
```

- [ ] **Step 5: Visual smoke test**

Run `./build/linux-qt6-debug/ui/linux-qt/tesseract`. Click a file card in a room → save dialog appears → file is written. Open an image → click ⬇ → image is saved. Open a video → click ⬇ → video is saved.

- [ ] **Step 6: Commit**

```bash
git add ui/linux-qt/src/MainWindow.cpp
git commit -m "feat(qt6): wire file/image/video download to native save dialog"
```

---

### Task 5: GTK4 shell — wire file, image, and video download

**Files:**
- Modify: `ui/linux-gtk/src/MainWindow.cpp`

GTK4 uses `gtk_file_chooser_native_new(title, window, GTK_FILE_CHOOSER_ACTION_SAVE, "Save", "Cancel")` + `gtk_native_dialog_run()` (synchronous, blocks the UI briefly but is the simplest correct approach on GTK4) or async via `gtk_native_dialog_show` + signal. Use the synchronous form for simplicity.

Add `#include <fstream>` near top if missing.

The async helper is `run_async_` (from `ShellBase`) and `g_idle_add` for posting back.

Find where `on_image_clicked` and `on_video_clicked` are wired (~lines 1126, 1159).

- [ ] **Step 1: Wire `on_file_clicked`**

After the `on_video_clicked` block (~line 1195), add:

```cpp
room_view_->on_file_clicked =
    [this](const tesseract::views::MessageListView::FileHit& hit)
{
    GtkFileChooserNative* dlg = gtk_file_chooser_native_new(
        "Save file", GTK_WINDOW(gtk_widget_get_root(
                         main_app_surface_->widget())),
        GTK_FILE_CHOOSER_ACTION_SAVE, "Save", "Cancel");
    std::string suggested = hit.file_name.empty() ? "download" : hit.file_name;
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg),
                                      suggested.c_str());
    gint result = gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg));
    if (result != GTK_RESPONSE_ACCEPT)
    {
        g_object_unref(dlg);
        return;
    }
    GFile* gf = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dlg));
    char* cpath = g_file_get_path(gf);
    std::string dest(cpath);
    g_free(cpath);
    g_object_unref(gf);
    g_object_unref(dlg);
    std::string url = hit.media_url;
    run_async_(
        [this, url, dest]()
        {
            auto bytes = client_->fetch_media_bytes(url);
            struct Ctx { MainWindow* self; std::string dest;
                         std::vector<uint8_t> bytes; };
            auto* ctx = new Ctx{this, dest, std::move(bytes)};
            g_idle_add(
                [](gpointer p) -> gboolean
                {
                    auto* c = static_cast<Ctx*>(p);
                    if (!c->bytes.empty())
                    {
                        std::ofstream f(c->dest, std::ios::binary);
                        f.write(reinterpret_cast<const char*>(
                                    c->bytes.data()),
                                static_cast<std::streamsize>(
                                    c->bytes.size()));
                    }
                    delete c;
                    return G_SOURCE_REMOVE;
                },
                ctx);
        });
};
```

- [ ] **Step 2: Wire `vid_viewer_->on_save`**

After the video viewer setup block (~line 1195 area), add:

```cpp
vid_viewer_->on_save =
    [this](std::string source_json, std::string mime_type)
{
    std::string ext = ".mp4";
    auto slash = mime_type.find('/');
    if (slash != std::string::npos)
        ext = "." + mime_type.substr(slash + 1);
    GtkFileChooserNative* dlg = gtk_file_chooser_native_new(
        "Save video", GTK_WINDOW(gtk_widget_get_root(
                          main_app_surface_->widget())),
        GTK_FILE_CHOOSER_ACTION_SAVE, "Save", "Cancel");
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg),
                                      ("video" + ext).c_str());
    gint result = gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg));
    if (result != GTK_RESPONSE_ACCEPT)
    {
        g_object_unref(dlg);
        return;
    }
    GFile* gf = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dlg));
    char* cpath = g_file_get_path(gf);
    std::string dest(cpath);
    g_free(cpath);
    g_object_unref(gf);
    g_object_unref(dlg);
    run_async_(
        [this, source_json = std::move(source_json), dest]()
        {
            auto bytes = client_->fetch_source_bytes(source_json);
            struct Ctx { MainWindow* self; std::string dest;
                         std::vector<uint8_t> bytes; };
            auto* ctx = new Ctx{this, dest, std::move(bytes)};
            g_idle_add(
                [](gpointer p) -> gboolean
                {
                    auto* c = static_cast<Ctx*>(p);
                    if (!c->bytes.empty())
                    {
                        std::ofstream f(c->dest, std::ios::binary);
                        f.write(reinterpret_cast<const char*>(
                                    c->bytes.data()),
                                static_cast<std::streamsize>(
                                    c->bytes.size()));
                    }
                    delete c;
                    return G_SOURCE_REMOVE;
                },
                ctx);
        });
};
```

- [ ] **Step 3: Wire `img_viewer` / `image_viewer` `on_save`**

Find where the image viewer is set up (near line 1126). Add after the `on_image_clicked` block:

```cpp
main_app_->image_viewer()->on_save =
    [this](std::string source_url, std::string filename_hint)
{
    std::string suggested = filename_hint.empty() ? "image" : filename_hint;
    GtkFileChooserNative* dlg = gtk_file_chooser_native_new(
        "Save image", GTK_WINDOW(gtk_widget_get_root(
                          main_app_surface_->widget())),
        GTK_FILE_CHOOSER_ACTION_SAVE, "Save", "Cancel");
    gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dlg),
                                      suggested.c_str());
    gint result = gtk_native_dialog_run(GTK_NATIVE_DIALOG(dlg));
    if (result != GTK_RESPONSE_ACCEPT)
    {
        g_object_unref(dlg);
        return;
    }
    GFile* gf = gtk_file_chooser_get_file(GTK_FILE_CHOOSER(dlg));
    char* cpath = g_file_get_path(gf);
    std::string dest(cpath);
    g_free(cpath);
    g_object_unref(gf);
    g_object_unref(dlg);
    run_async_(
        [this, source_url = std::move(source_url), dest]()
        {
            auto bytes = client_->fetch_source_bytes(source_url);
            struct Ctx { MainWindow* self; std::string dest;
                         std::vector<uint8_t> bytes; };
            auto* ctx = new Ctx{this, dest, std::move(bytes)};
            g_idle_add(
                [](gpointer p) -> gboolean
                {
                    auto* c = static_cast<Ctx*>(p);
                    if (!c->bytes.empty())
                    {
                        std::ofstream f(c->dest, std::ios::binary);
                        f.write(reinterpret_cast<const char*>(
                                    c->bytes.data()),
                                static_cast<std::streamsize>(
                                    c->bytes.size()));
                    }
                    delete c;
                    return G_SOURCE_REMOVE;
                },
                ctx);
        });
};
```

- [ ] **Step 4: Build GTK4 target**

```bash
cmake --preset linux-gtk-debug
cmake --build build/linux-gtk-debug --target tesseract 2>&1 | tail -5
```

Expected: 0 errors.

- [ ] **Step 5: Commit**

```bash
git add ui/linux-gtk/src/MainWindow.cpp
git commit -m "feat(gtk4): wire file/image/video download to native save dialog"
```

---

### Task 6: Win32 shell — wire file, image, and video download

**Files:**
- Modify: `ui/windows/src/MainWindow.h`
- Modify: `ui/windows/src/MainWindow.cpp`

Win32 uses `GetSaveFileNameW` (OPENFILENAMEW) for the dialog and the existing `PostMessageW` + struct-on-heap pattern for the async byte fetch.

- [ ] **Step 1: Add `WM_TESSERACT_FILE_BYTES` message constant in `MainWindow.h`**

After `WM_TESSERACT_JOIN_ROOM_DONE = WM_APP + 26;` (line 81), add:

```cpp
constexpr UINT WM_TESSERACT_FILE_BYTES = WM_APP + 27;
```

- [ ] **Step 2: Add `FileBytesPayload` struct alongside other payload structs in `MainWindow.h`**

Find the `VideoBytesPayload` struct (~line 163 area) and add next to it:

```cpp
struct FileBytesPayload
{
    std::string dest_path;
    std::vector<uint8_t> bytes;
};
```

- [ ] **Step 3: Add a `show_save_dialog_` helper to `MainWindow.h`**

In the private section, declare:

```cpp
// Returns the user-chosen path, or "" if cancelled.
std::wstring show_save_dialog_(const std::wstring& suggested,
                               const wchar_t* filter);
```

- [ ] **Step 4: Implement `show_save_dialog_` in `MainWindow.cpp`**

```cpp
std::wstring MainWindow::show_save_dialog_(const std::wstring& suggested,
                                           const wchar_t* filter)
{
    wchar_t buf[MAX_PATH]{};
    if (!suggested.empty())
    {
        wcsncpy_s(buf, suggested.c_str(), MAX_PATH - 1);
    }
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner   = hwnd_;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = MAX_PATH;
    ofn.lpstrFilter = filter;      // e.g. L"All files\0*.*\0\0"
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    if (GetSaveFileNameW(&ofn))
        return buf;
    return {};
}
```

- [ ] **Step 5: Wire `on_file_clicked` in the room-view setup block (~line 1959)**

After the `on_video_clicked` block, add:

```cpp
room_view_->on_file_clicked =
    [this](const tesseract::views::MessageListView::FileHit& hit)
{
    std::wstring suggested(hit.file_name.begin(), hit.file_name.end());
    if (suggested.empty()) suggested = L"download";
    std::wstring path = show_save_dialog_(suggested,
                                          L"All files\0*.*\0\0");
    if (path.empty())
        return;
    HWND target = hwnd_;
    std::string url = hit.media_url;
    run_async_(
        [this, target, url, path]()
        {
            auto bytes = client_->fetch_media_bytes(url);
            auto* p = new FileBytesPayload{
                std::string(path.begin(), path.end()), std::move(bytes)};
            if (!PostMessageW(target, WM_TESSERACT_FILE_BYTES, 0,
                              reinterpret_cast<LPARAM>(p)))
                delete p;
        });
};
```

- [ ] **Step 6: Handle `WM_TESSERACT_FILE_BYTES` in `WndProc` / message handler**

Find the `WM_TESSERACT_VIDEO_BYTES` handler (~line 813). Add next to it:

```cpp
case WM_TESSERACT_FILE_BYTES:
{
    auto* p = reinterpret_cast<FileBytesPayload*>(lParam);
    if (p && !p->bytes.empty())
    {
        std::ofstream f(p->dest_path, std::ios::binary);
        f.write(reinterpret_cast<const char*>(p->bytes.data()),
                static_cast<std::streamsize>(p->bytes.size()));
    }
    delete p;
    return 0;
}
```

- [ ] **Step 7: Wire `image_viewer()->on_save`**

After the `on_image_clicked` wiring block (~line 1959 area), add:

```cpp
main_app_->image_viewer()->on_save =
    [this](std::string source_url, std::string filename_hint)
{
    std::wstring suggested(filename_hint.begin(), filename_hint.end());
    if (suggested.empty()) suggested = L"image";
    std::wstring path = show_save_dialog_(
        suggested, L"Images\0*.jpg;*.jpeg;*.png;*.gif;*.webp\0All files\0*.*\0\0");
    if (path.empty())
        return;
    HWND target = hwnd_;
    run_async_(
        [this, target, source_url = std::move(source_url), path]()
        {
            auto bytes = client_->fetch_source_bytes(source_url);
            auto* p = new FileBytesPayload{
                std::string(path.begin(), path.end()), std::move(bytes)};
            if (!PostMessageW(target, WM_TESSERACT_FILE_BYTES, 0,
                              reinterpret_cast<LPARAM>(p)))
                delete p;
        });
};
```

- [ ] **Step 8: Wire `video_viewer()->on_save`**

```cpp
main_app_->video_viewer()->on_save =
    [this](std::string source_json, std::string mime_type)
{
    std::string ext = ".mp4";
    auto slash = mime_type.find('/');
    if (slash != std::string::npos)
        ext = "." + mime_type.substr(slash + 1);
    std::wstring suggested(("video" + ext).begin(), ("video" + ext).end());
    std::wstring path = show_save_dialog_(
        suggested, L"Videos\0*.mp4;*.webm;*.mkv\0All files\0*.*\0\0");
    if (path.empty())
        return;
    HWND target = hwnd_;
    run_async_(
        [this, target, source_json = std::move(source_json), path]()
        {
            auto bytes = client_->fetch_source_bytes(source_json);
            auto* p = new FileBytesPayload{
                std::string(path.begin(), path.end()), std::move(bytes)};
            if (!PostMessageW(target, WM_TESSERACT_FILE_BYTES, 0,
                              reinterpret_cast<LPARAM>(p)))
                delete p;
        });
};
```

- [ ] **Step 9: Build Win32 target (if available; skip if no Windows toolchain)**

```bash
cmake --preset windows-debug
cmake --build build/windows-debug --target tesseract 2>&1 | tail -10
```

Expected: 0 errors. (Cross-compile or skip on Linux-only CI.)

- [ ] **Step 10: Commit**

```bash
git add ui/windows/src/MainWindow.h ui/windows/src/MainWindow.cpp
git commit -m "feat(win32): wire file/image/video download via GetSaveFileNameW"
```

---

### Task 7: macOS shell — wire file, image, and video download

**Files:**
- Modify: `ui/macos/src/MainWindowController.mm`

macOS uses `NSSavePanel`. The panel must be shown on the main thread. Since `on_file_clicked` fires on the UI (main) thread already, we show the panel synchronously with `runModal`, then dispatch the fetch to a GCD global queue.

Find where `on_image_clicked` and `on_video_clicked` are wired (~lines 1900, 1915).

- [ ] **Step 1: Wire `on_file_clicked`**

After the `on_video_clicked` block (~line 1950), add:

```objc
_mainApp->room_view()->on_file_clicked =
    [weakSelf](const tesseract::views::MessageListView::FileHit& hit)
{
    MainWindowController* s = weakSelf;
    if (!s)
        return;
    NSSavePanel* panel = [NSSavePanel savePanel];
    NSString* suggested = hit.file_name.empty()
        ? @"download"
        : [NSString stringWithUTF8String:hit.file_name.c_str()];
    panel.nameFieldStringValue = suggested;
    NSModalResponse resp = [panel runModal];
    if (resp != NSModalResponseOK || !panel.URL)
        return;
    std::string dest = panel.URL.path.UTF8String;
    std::string url  = hit.media_url;
    s->_shell->run_async_(
        [weakSelf, url, dest, clientPtr = s->_shell->client_]()
        {
            auto bytes = clientPtr->fetch_media_bytes(url);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (bytes.empty())
                    return;
                std::ofstream f(dest, std::ios::binary);
                f.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
            });
        });
};
```

- [ ] **Step 2: Wire `image_viewer()->on_save`**

After the `on_image_clicked` block (~line 1914), add:

```objc
_mainApp->image_viewer()->on_save =
    [weakSelf](std::string source_url, std::string filename_hint)
{
    MainWindowController* s = weakSelf;
    if (!s)
        return;
    NSSavePanel* panel = [NSSavePanel savePanel];
    NSString* suggested = filename_hint.empty()
        ? @"image"
        : [NSString stringWithUTF8String:filename_hint.c_str()];
    panel.nameFieldStringValue = suggested;
    NSModalResponse resp = [panel runModal];
    if (resp != NSModalResponseOK || !panel.URL)
        return;
    std::string dest = panel.URL.path.UTF8String;
    s->_shell->run_async_(
        [weakSelf, source_url = std::move(source_url), dest,
         clientPtr = s->_shell->client_]()
        {
            auto bytes = clientPtr->fetch_source_bytes(source_url);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (bytes.empty())
                    return;
                std::ofstream f(dest, std::ios::binary);
                f.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
            });
        });
};
```

- [ ] **Step 3: Wire `video_viewer()->on_save`**

After the `on_video_clicked` block (~line 1950), add (alongside Step 1):

```objc
_mainApp->video_viewer()->on_save =
    [weakSelf](std::string source_json, std::string mime_type)
{
    MainWindowController* s = weakSelf;
    if (!s)
        return;
    std::string ext = ".mp4";
    auto slash = mime_type.find('/');
    if (slash != std::string::npos)
        ext = "." + mime_type.substr(slash + 1);
    NSSavePanel* panel = [NSSavePanel savePanel];
    panel.nameFieldStringValue =
        [NSString stringWithUTF8String:("video" + ext).c_str()];
    NSModalResponse resp = [panel runModal];
    if (resp != NSModalResponseOK || !panel.URL)
        return;
    std::string dest = panel.URL.path.UTF8String;
    s->_shell->run_async_(
        [weakSelf, source_json = std::move(source_json), dest,
         clientPtr = s->_shell->client_]()
        {
            auto bytes = clientPtr->fetch_source_bytes(source_json);
            dispatch_async(dispatch_get_main_queue(), ^{
                if (bytes.empty())
                    return;
                std::ofstream f(dest, std::ios::binary);
                f.write(reinterpret_cast<const char*>(bytes.data()),
                        static_cast<std::streamsize>(bytes.size()));
            });
        });
};
```

- [ ] **Step 4: Build macOS target (if available)**

```bash
cmake --preset macos-appkit-arm64-debug
cmake --build build/macos-appkit-arm64-debug --target Tesseract 2>&1 | tail -10
```

- [ ] **Step 5: Commit**

```bash
git add ui/macos/src/MainWindowController.mm
git commit -m "feat(macos): wire file/image/video download via NSSavePanel"
```

---

### Task 8: Final build verification

**Files:** None (verification only)

- [ ] **Step 1: Run Rust tests (no Rust changes, just regression check)**

```bash
cargo test -p tesseract-sdk-ffi 2>&1 | tail -5
```

Expected: all tests pass (count should match before this feature was added).

- [ ] **Step 2: Build Qt6 full stack**

```bash
cmake --build build/linux-qt6-debug 2>&1 | tail -5
```

Expected: 0 errors, 0 warnings introduced by this feature.

- [ ] **Step 3: Run C++ tests**

```bash
ctest --test-dir build/linux-qt6-debug --output-on-failure 2>&1 | tail -20
```

Expected: all tests pass.

- [ ] **Step 4: Visual smoke test checklist**

Run `./build/linux-qt6-debug/ui/linux-qt/tesseract` and verify:

1. **File card click**: hover a file bubble → cursor changes to pointing hand → click → system save dialog opens with the attachment's filename pre-filled → choose a path → file appears at that path with correct bytes.
2. **Image save**: click an image thumbnail → image viewer opens → click ⬇ → save dialog → image written to disk.
3. **Video save**: click a video thumbnail → video viewer opens → click ⬇ → save dialog → video written to disk.
4. **Cancel**: clicking Cancel in the save dialog does nothing (no crash, no empty file).
5. **Empty bytes**: if fetch fails (e.g. offline), no file is written (no crash).

- [ ] **Step 5: Commit (if any fixups were needed from smoke test)**

```bash
git add -p
git commit -m "fix(download): <describe fixup>"
```
