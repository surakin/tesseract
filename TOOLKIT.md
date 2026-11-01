# The toolkit and views

Tesseract draws its own interface. It does not use Qt widgets, GTK widgets, Win32 controls or AppKit views for what you see. One shared widget tree is painted by a small per-platform backend, which is how the app looks the same everywhere. This page explains that layer: the toolkit (`ui/shared/tk/`) and the views built on it (`ui/shared/views/`).

For where this sits in the whole app see [ARCHITECTURE.md](ARCHITECTURE.md). For adding code see [DEVELOPING.md](DEVELOPING.md).

## The idea in one paragraph

A **widget tree** describes the screen. Each widget can measure itself, arrange its children, and paint itself onto a **Canvas**. The Canvas is an abstract 2D drawing surface with one implementation per platform (Direct2D, QPainter, Cairo+Pango, CoreGraphics+CoreText). A **Host** connects the tree to the operating system: it schedules repaints, runs code on the UI thread, and supplies the few things that must stay native. The toolkit knows nothing about Matrix.

```text
views (RoomView, ComposeBar, SettingsView, …)   what the app shows
        │ built from
tk widgets (Widget, VBox, Button, ListView, …)    layout, hit-testing, focus, keys
        │ paint into                │ talks to
     Canvas  ◄── backend ──►      Host  ◄── backend
 (draw rects, text, images)   (repaint, post-to-UI, native text boxes, audio…)
```

## Layers and the include rule

`ui/shared` is four static libraries, each linking only the one below:

| Library | Folder | Holds |
| --- | --- | --- |
| `tesseract_tk` | `tk/` | Widget, Canvas, Host, layout, theme, caches |
| `tesseract_platform` | `platform/` | Non-UI OS services: capture, location, single instance |
| `tesseract_views` | `views/` | Rooms, timeline, composer, settings, calls |
| `tesseract_app` | `app/` | `ShellBase` and the controllers (see [SHELLS.md](SHELLS.md)) |

A layer may not include a higher one. The `layering_check` ctest fails the test run if it does. Put new code in the lowest layer that makes sense.

## Widgets

`tk::Widget` (`tk/widget.h`) is the base class. A widget does four jobs:

1. **Measure:** `measure(ctx, constraints)` returns the size it wants.
2. **Arrange:** `arrange(ctx, bounds)` fixes its final rectangle and arranges its children. Layout is the classic two-pass measure then arrange.
3. **Paint:** `paint(ctx)` draws into the Canvas. Children are painted after their parent. `paint_overlay` draws on top of everything, for popups and tooltips.
4. **Handle input:** pointer (`on_pointer_down/up/move`, `on_wheel`, `on_right_click`), keys (`on_key_down`), drag and drop, and focus.

Widgets are created through `tk::create_widget<T>(parent, args…)`, never with `new` or `make_unique`. The factory makes the Host available from the first line of the constructor. Children are owned by their parent.

### Layout containers

`tk/layout.h` has the usual boxes. `VBox` stacks top to bottom and `HBox` left to right, with padding, spacing and alignment. `Stack` layers children in the same bounds, which is how modal overlays sit on top of content. `Spacer` and `FixedBox` fill gaps. A child asks to stretch with `LayoutHints::fill_main` or `fill_cross`.

Do not call `arrange()` on a widget twice in the same pass after its children are arranged. This corrupts scroll limits.

### Ready-made controls

Look in `tk/` before writing a control: `Button`, `CheckButton`, `Label`, radio buttons, `Slider`, `ComboBox`, `TabView`, `ListView`, `ScrollView`, `TextField` and `TextArea`, `Toast`, `Tooltip`, `GroupBox`, and others. Use these instead of painting your own buttons inside `paint()`. A region that reacts to clicks should be a widget that sets `set_hoverable()` and `set_on_click()`, or a `Button` or `HoverArea`, so it gets the same hover and cursor behaviour as everything else.

### Visibility and enabled state

`visible()` is true only if the widget and all its ancestors are visible. Use `own_visible()` for the widget's own flag. Be careful: an unselected tab's content reports `visible() == false`. A disabled widget swallows all pointer and key input, except mouse-wheel scrolling.

## Drawing

`tk::Canvas` (`tk/canvas.h`) is deliberately small: fill and stroke rectangles, lines, images, text layouts, clipping, and opacity. All coordinates are logical pixels, and the backend applies the display scale.

Text is not drawn from strings. You ask the `CanvasFactory` to build a `TextLayout` (or rich text with several styles, such as emoji and links), measure it, and draw it. Layouts are expensive, so views build them when content changes and keep them.

Images are decoded and cached in the shared caches (`AnimImageCache`, `CompressedImageCache`, `MediaDiskCache`). Where the backend supports `Host::request_repaint_rect` (Qt6, Win32, macOS), animated images repaint only their own region. GTK4 repaints the whole surface.

## Theme and fonts

`tk::Theme` (`tk/theme.h`) holds the palette for light and dark, with the accent variants (Blue, Forest, Sunset, Violet, or the system accent). Widgets read colours from the theme in `paint()`, never hard-code them. When the theme or the display scale changes, widgets get `on_theme_changed` and `on_scale_changed`. Fonts go through `FontConfig` (Settings → Appearance → Fonts). `docs/UI-PARITY.md` records the colour and size values the platforms must share.

## Text input stays native

Real text editing needs IME, spell-check, selection and accessibility that are hard to reproduce. So `NativeTextField` and `NativeTextArea` wrap a real platform control: QLineEdit and QTextEdit on Qt6, GtkEntry and GtkTextView on GTK4, NSTextField and NSTextView on macOS, and [BetterText](../third_party/bettertext/README.md) on Windows. The control is overlaid on the widget's rectangle. Its pixels are rendered into the Canvas so it blends with everything else. The toolkit draws the hover and focus ring. It also draws the caret on Qt6 and Win32 (fields and areas) and for multi-line areas on GTK4 and macOS (`caret_owned_by_canvas()` in `host.h`). Single-line fields on GTK4 and macOS keep the native caret in the captured image.

## The Host

`tk::Host` (`tk/host.h`) is the per-platform integration point for one window or surface. The parts you will meet:

- `request_repaint()` and `request_relayout()`: ask for a redraw. Widgets change state and call these; they don't draw immediately.
- `post_to_ui(fn)` and `post_delayed(ms, fn)`: run code on the UI thread. This is how work from other threads reaches the widgets.
- `theme()`: the current theme.
- Factories for native pieces: text fields, popup windows, audio and video players, audio capture.
- Clipboard and cursor.

The platform backend subclasses it, along with a `Surface` that owns a Host (the shared base is `tk::SurfaceBase` in `tk/surface.h`; each backend has its own, such as `tk::qt6::Surface`), paints one widget tree and reacts to theme and scale changes. Backends live in `ui/<platform>/tk/`.

Tests use `TestHost` (`tests/cpp/tk_test_host.h`), a stub Host, so widgets can be driven and checked without a window.

## Accessibility

Every widget describes itself to screen readers through virtual methods: `access_role()`, `access_name()`, `access_state()`, `access_value()`, and a default action. The toolkit builds an accessibility tree from them (`tk/access_tree.h`), and each backend exposes it through UIA, AT-SPI, NSAccessibility or the Qt/GTK equivalents. A new view is not finished until it sets its role and name. Tests check this (`*_accessibility.cpp`).

## Keyboard and focus

Focus moves with Tab through widgets whose `focusable()` returns true. `focus_on_click()` is separate: some widgets are focusable by keyboard but don't grab focus on click. Global shortcuts come from one table, `views/shortcut_registry.h`; see [DEVELOPING.md](DEVELOPING.md). Native text controls hold real focus, which is a source of subtle bugs, so check `holds_native_focus()` when changing focus code.

## Views

A **view** is a widget (or a small group) that shows one piece of the app: `RoomListView`, `RoomView`, `MessageListView`, `ComposeBar`, `ThreadView`, `SettingsView`, `CallOverlayWidget`, the pickers and dialogs. `MainAppWidget` is the root that assembles them.

Views follow a few conventions:

- **No networking or app state.** A view gets data through setters (`set_pins(...)`) and tells the outside world what the user did through `std::function` callbacks (`on_jump_to`). The `ShellBase` layer wires the two together.
- **Types come from `client/`.** Views use `tesseract::` data types such as the `Event` hierarchy (`client/include/tesseract/types.h`) and `RoomInfo`, never SDK or generated bridge types.
- **Every visible string uses `tk::tr`.** See [DEVELOPING.md](DEVELOPING.md).
- **Expose test hooks** for the parts a test must reach, and layout constants as `static constexpr`.

A small view such as `PinnedBanner` is a good template. It is a `tk::Widget` subclass with setters, one callback, `measure`/`arrange`/`paint`, and accessibility methods.

Settings pages are built from `SettingsGroup`s and a `SettingBinder` that ties a control to a field in `Settings`. See the setting recipe in [DEVELOPING.md](DEVELOPING.md).

## Backends

Each platform provides the same set of pieces in `ui/<platform>/tk/`:

| Piece | Qt6 | GTK4 | Win32 | macOS |
| --- | --- | --- | --- | --- |
| Canvas | QPainter | Cairo + Pango | Direct2D | CoreGraphics + CoreText |
| Host / Surface | host_qt | host_gtk | host_win32 | host_macos |
| Native text, audio, video | Qt | GStreamer / GTK | Media Foundation / WASAPI | AVFoundation |

If you change the Canvas or Host interface, every backend must change with it. Prefer adding to the shared layer so the backends stay small.
