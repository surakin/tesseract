#import "RoomWindowController.h"
#import "tk_locale.h"
#include "app/ShellBase.h"
#include "app/ComposerPopups.h"
#include "app/RoomWindowBase.h"
#include <tesseract/visual.h>
#include "tk/host_macos.h"
#include "tk/i18n.h"
#include "views/ConfirmDialog.h"
#include "views/ForwardRoomPicker.h"
#include "views/MediaViewerOverlay.h"
#include "views/media_viewer_items.h"
#include "views/PopoutRoomWidget.h"
#include "views/RoomMediaView.h"
#include "views/MessageListView.h"

#include <tesseract/client.h>
#include <tesseract/image_pack.h>
#include <tesseract/settings.h>

#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declaration — lets @interface reference MacRoomWindow before the
// C++ class definition.
// ─────────────────────────────────────────────────────────────────────────────

class MacRoomWindow;

@interface RoomWindowController ()
@property(nonatomic, assign) MacRoomWindow* cppWindow;
- (void)_windowDidChangeBackingProperties:(NSNotification*)note;
@end

// ─────────────────────────────────────────────────────────────────────────────
// MacRoomWindow — C++ RoomWindowBase subclass for macOS pop-out windows
// ─────────────────────────────────────────────────────────────────────────────

class MacRoomWindow : public tesseract::RoomWindowBase
{
public:
    MacRoomWindow(tesseract::ShellBase* shell, const std::string& room_id);
    ~MacRoomWindow() override;

    void bring_to_front() override;
    void close_window() override;
    void request_relayout() override;
    void update_window_title_(const std::string& name) override;
    void apply_theme(const tk::Theme& t) override;
    void apply_scale_change(float scale) override;
    void repaint_anim_frame() override;

    // Called by -windowWillClose: delegate method.
    void on_window_will_close()
    {
        window_closed_ = true;
        schedule_self_close_();
    }

    // Expose protected RoomWindowBase members to ObjC++ callers.
    using tesseract::RoomWindowBase::save_popout_geometry_;

    // Called by -windowDidResize: delegate method. A popup's screen position
    // is captured once when it opens and never recomputed, so leaving one
    // open across a resize would strand it away from whatever control
    // anchored it. Close everything instead.
    void dismiss_popups_on_resize()
    {
        if (surface_) surface_->host().dismiss_active_popup();
        if (room_view_) room_view_->dismiss_popups();
        if (popups_)
            popups_->hide_all();
    }

    // Called by -keyDown: when Escape is pressed.
    bool on_escape_key()
    {
        if (room_view_ && room_view_->room_search_open())
        {
            room_view_->close_room_search();
            return true;
        }
        if (close_media_viewer_if_open_())
        {
            return true;
        }
        return false;
    }

protected:
    void surface_repaint_() override;
    // Fan-in for async GIF search results (forwarded by ShellBase to every
    // pop-out; only the controller that issued the search matches).
    void on_gif_results(std::uint64_t request_id,
                        std::vector<tesseract::GifResult> results) override;
    void on_gif_search_failed(std::uint64_t request_id,
                              const std::string& message) override;

private:
    __strong RoomWindowController* controller_ = nil;
    std::unique_ptr<tk::macos::Surface> surface_;
    // Borrowed from room_view_->compose_bar()->text_area() — see
    // compose_text_area_(). Search fields are self-owned too — see
    // RoomSearchBar::search_field() / ForwardRoomPicker::search_field().
    tk::TextArea* text_area_ = nullptr;
    tesseract::views::ForwardRoomPicker* forward_picker_widget_ = nullptr; // borrowed
    tesseract::views::RoomMediaView* room_media_view_widget_ = nullptr; // borrowed
    tesseract::views::ConfirmDialog* confirm_dialog_widget_ = nullptr; // borrowed

    // Composer popups (@mention, /command, :shortcode:, /gif). Declared after
    // surface_ so the popups are destroyed before the Host they came from.
    std::unique_ptr<tesseract::ComposerPopups> popups_;
    int link_hovered_ = 0;
    bool window_closed_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// MacRoomWindow implementation
// ─────────────────────────────────────────────────────────────────────────────

MacRoomWindow::MacRoomWindow(tesseract::ShellBase* shell,
                             const std::string&    room_id)
    : tesseract::RoomWindowBase(shell, room_id)
{
    NSRect frame = NSMakeRect(0, 0, 800, 600);
    NSWindowStyleMask style =
        NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
        NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSWindow* win = [[NSWindow alloc] initWithContentRect:frame
                                                styleMask:style
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    win.minSize = NSMakeSize(tesseract::visual::kMinWindowWidth,
                              tesseract::visual::kMinWindowHeight);
    NSString* title = [NSString stringWithUTF8String:room_id.c_str()] ?: @"";
    [win setTitle:title];

    // Apply saved geometry, or centre the default-sized window.
    {
        auto saved = get_saved_popout_geometry_(800, 600);
        if (saved.valid)
        {
            NSArray<NSScreen*>* screens = [NSScreen screens];
            const CGFloat primaryTop =
                screens.count > 0
                    ? ([[screens firstObject] frame].origin.y +
                       [[screens firstObject] frame].size.height)
                    : 768.0;
            NSRect f = NSMakeRect(saved.x,
                                  primaryTop - saved.y - saved.h,
                                  saved.w,
                                  saved.h);
            [win setFrame:f display:NO];
        }
        else
        {
            [win center];
        }
    }

    surface_ = std::make_unique<tk::macos::Surface>(tk::Theme::light());
    NSView* surfaceView = (__bridge NSView*)surface_->view_handle();
    [win setContentView:surfaceView];

    auto room_widget = tk::create_root_widget<tesseract::views::PopoutRoomWidget>(
        &surface_->host());
    room_view_             = room_widget->room_view();
    media_viewer_          = room_widget->media_viewer();
    forward_picker_widget_ = room_widget->forward_picker();
    room_media_view_widget_ = room_widget->room_media_view();
    confirm_dialog_widget_ = room_widget->confirm_dialog();
    room_widget->on_layout_changed = [this]
    {
        if (surface_)
        {
            surface_->relayout();
        }
    };
    surface_->set_root(std::move(room_widget));

    // ── Shared per-room wiring, via RoomPane ──────────────────────────────
    init_pane_(&surface_->host());
    pane_->attach({
        .room_view = room_view_,
        .media_viewer = media_viewer_,
        .forward_picker = forward_picker_widget_,
        .room_media_view = room_media_view_widget_,
        .focus_forward_picker_field = [this]
        {
            if (!forward_picker_widget_)
                return;
            if (auto* f = forward_picker_widget_->search_field())
            {
                f->set_text("");
                f->set_focused(true);
            }
        },
        .hide_forward_picker_field = [this]
        {
            if (forward_picker_widget_)
                if (auto* f = forward_picker_widget_->search_field())
                    f->set_visible(false);
        },
    });

    // ── Video player for this window's MediaViewerOverlay ────────────────────
    if (auto player = surface_->host().make_video_player())
    {
        media_viewer_->set_video_player(std::move(player));
    }

    // Inline autoplay video/GIF in the timeline (separate from the lightbox
    // player above — MessageListView falls back to a static thumbnail unless
    // both of these are set).
    room_view_->set_video_player_factory(
        [this]() { return surface_->host().make_video_player(); });
    room_view_->set_video_fetch_provider(
        [this](const std::string& src,
               std::function<void(std::vector<std::uint8_t>)> on_ready)
        {
            pane_->fetch_source_bytes_(src, std::move(on_ready));
        });

    // ── Media save dialog ─────────────────────────────────────────────────────
    // One handler for every viewer kind: RoomPane builds the spec and writes
    // the file; this window only shows the panel.
    pane_->install_media_viewer_save_(
        [](const tesseract::views::MediaSaveSpec& spec,
           std::function<void(std::string)> done)
        {
            NSSavePanel* panel = [NSSavePanel savePanel];
            panel.nameFieldStringValue =
                [NSString stringWithUTF8String:spec.suggested_name.c_str()];
            NSModalResponse resp = [panel runModal];
            if (resp != NSModalResponseOK || !panel.URL)
                return;
            done(std::string(panel.URL.path.UTF8String));
        });

    // Full-screen toggle acts on this pop-out window.
    const auto set_fs = [this](bool on)
    {
        if (window_closed_ || !controller_ || !controller_.window)
            return;
        const bool is_fs =
            (controller_.window.styleMask & NSWindowStyleMaskFullScreen) != 0;
        if (is_fs != on)
            [controller_.window toggleFullScreen:nil];
    };
    media_viewer_->on_request_fullscreen = set_fs;

    room_view_->on_file_clicked =
        [this](const tesseract::views::MessageListView::FileHit& hit)
    {
        NSSavePanel* panel = [NSSavePanel savePanel];
        NSString* suggested = hit.file_name.empty()
            ? @"download"
            : [NSString stringWithUTF8String:hit.file_name.c_str()];
        panel.nameFieldStringValue = suggested;
        NSModalResponse resp = [panel runModal];
        if (resp != NSModalResponseOK || !panel.URL)
            return;
        std::string url = hit.source ? hit.source->fetch_token() : std::string{};
        pane_->save_source_to_file_(std::move(url),
                              std::string(panel.URL.path.UTF8String));
    };

    // ── Surface-bound providers (need this shell's own surface_) ─────────────
    if (auto player = surface_->host().make_audio_player())
    {
        room_view_->set_audio_player(std::move(player));
    }

    // Drag-and-drop file ingest is now tree-dispatched automatically (see
    // Host::ingest_native_file_drop -> Host::dispatch_file_drop);
    // RoomView::on_file_drop routes into this window's compose bar via the
    // provider fields wired in wire_room_view_.
    surface_->set_on_file_drop_error(
        [this](std::string reason)
        {
            pane_->shell_show_status_message_(std::move(reason));
        });

    room_view_->set_post_delayed(
        [this](int ms, std::function<void()> fn)
        {
            if (surface_)
            {
                surface_->host().post_delayed(ms, std::move(fn));
            }
        });
    room_view_->on_layout_changed = [this]
    {
        if (surface_)
        {
            surface_->relayout();
        }
    };
    room_view_->on_set_clipboard = [this](std::string_view t)
    {
        if (surface_)
            surface_->host().set_clipboard_text(t);
    };
    room_view_->on_selection_started = [this]()
    {
        if (surface_)
            surface_->host().release_focus_to_canvas();
    };
    room_view_->on_show_copy_menu = [this]()
    {
        if (!surface_) return;
        NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
        NSMenuItem* item = [[NSMenuItem alloc]
            initWithTitle:TkTr("Copy")
                   action:@selector(copy:)
            keyEquivalent:@""];
        // Explicit target: with nil, copy: walks the responder chain and a
        // still-first-responder composer NSTextView would copy its own text.
        item.target = controller_;
        [menu addItem:item];
        NSEvent* event = [NSApp currentEvent];
        NSView* view = (__bridge NSView*)surface_->view_handle();
        if (event && view)
            [NSMenu popUpContextMenu:menu withEvent:event forView:view];
    };

    // ── Compose text area (self-owned — see ComposeBar::text_area()) ────────
    // + composer popups (@mention, /command, :shortcode:, /gif).
    text_area_ = room_view_->compose_bar()->text_area();
    popups_ = std::make_unique<tesseract::ComposerPopups>(
        surface_->host(), text_area_, pane_.get(), room_view_);
    surface_->set_on_layout(
        [this]
        {
            // Native child controls always paint over canvas-drawn overlays,
            // so hide them while the confirm dialog covers the window —
            // otherwise the compose box/search fields would poke through on
            // top of the modal backdrop. text_area_ self-positions via
            // ComposeBar::arrange() otherwise (reached via the relayout this
            // set_on_layout callback runs after), so only a force-hide is
            // needed here — mirrors the search fields' own gating below.
            const bool confirm_open =
                confirm_dialog_widget_ && confirm_dialog_widget_->is_open();
            if (confirm_open && text_area_)
            {
                text_area_->set_visible(false);
            }
            if (confirm_open && room_view_)
            {
                // Search field self-positions via RoomSearchBar::arrange(),
                // but the ConfirmDialog covering this window is pop-out-local
                // state the widget doesn't know about — force it off here.
                if (auto* bar = room_view_->room_search_bar())
                    if (auto* f = bar->search_field())
                        f->set_visible(false);
            }
            if (confirm_open && forward_picker_widget_)
            {
                // Search field self-positions via ForwardRoomPicker::arrange(),
                // but the ConfirmDialog covering this window is pop-out-local
                // state the widget doesn't know about — force it off here.
                if (auto* f = forward_picker_widget_->search_field())
                    f->set_visible(false);
            }
        });

    // Per-room "find in conversation" — search field is self-owned; only the
    // shell-level Up/Down/Escape nav needs wiring here (on_close is already
    // wired internally by RoomView's own constructor).
    if (room_view_)
    {
        if (auto* bar = room_view_->room_search_bar())
        {
            if (auto* rif = bar->search_field())
            {
                rif->push_popup_nav(
                    [this](tk::NavKey nk) -> bool
                    {
                        if (!room_view_ || !room_view_->room_search_open())
                            return false;
                        switch (nk)
                        {
                        case tk::NavKey::Up:
                            if (room_view_->on_room_search_navigate)
                                room_view_->on_room_search_navigate(-1);
                            return true;
                        case tk::NavKey::Down:
                            if (room_view_->on_room_search_navigate)
                                room_view_->on_room_search_navigate(+1);
                            return true;
                        case tk::NavKey::Escape:
                            room_view_->close_room_search();
                            return true;
                        default:
                            return false;
                        }
                    });
            }
        }
    }

    // Forward-message picker — search field is self-owned; only the
    // shell-level Up/Down/Escape nav needs wiring here.
    if (forward_picker_widget_)
    {
        if (auto* fpf = forward_picker_widget_->search_field())
        {
            fpf->push_popup_nav(
                [this](tk::NavKey nk) -> bool
                {
                    if (!forward_picker_widget_ || !forward_picker_widget_->is_open())
                        return false;
                    switch (nk)
                    {
                    case tk::NavKey::Up:
                        forward_picker_widget_->move_selection(-1);
                        if (surface_) surface_->relayout();
                        return true;
                    case tk::NavKey::Down:
                        forward_picker_widget_->move_selection(+1);
                        if (surface_) surface_->relayout();
                        return true;
                    case tk::NavKey::Escape:
                        forward_picker_widget_->close();
                        return true;
                    default:
                        return false;
                    }
                });
        }
    }

    room_view_->on_link_hovered = [this](const std::string& url)
    {
        // 0 = none pushed, 1 = pointing hand, 2 = I-beam.
        const auto hc = tesseract::views::MessageListView::hover_cursor_for(url);
        const int want = hc == tesseract::views::MessageListView::HoverCursor::Text ? 2
                         : hc == tesseract::views::MessageListView::HoverCursor::Pointer ? 1 : 0;
        if (want == link_hovered_)
            return;
        if (link_hovered_ != 0)
            [NSCursor pop];
        if (want == 2)
            [[NSCursor IBeamCursor] push];
        else if (want == 1)
            [[NSCursor pointingHandCursor] push];
        link_hovered_ = want;
    };

    // Wire up the ObjC window controller.
    controller_ = [[RoomWindowController alloc] initWithWindow:win];
    controller_.cppWindow = this;
    [win setDelegate:controller_];

    // Re-rasterize native-control image captures (tk::NativeTextField/
    // NativeTextArea) when this pop-out's own backing scale factor changes
    // — this is an independent window the user can drag to a
    // differently-scaled display on its own, so it needs its own observer
    // rather than relying on the main window's.
    [[NSNotificationCenter defaultCenter]
        addObserver:controller_
           selector:@selector(_windowDidChangeBackingProperties:)
               name:NSWindowDidChangeBackingPropertiesNotification
             object:win];

    [win makeKeyAndOrderFront:nil];

    finish_init_();
}

MacRoomWindow::~MacRoomWindow()
{
    close_window();
}

void MacRoomWindow::bring_to_front()
{
    if (!window_closed_ && controller_)
    {
        [controller_.window makeKeyAndOrderFront:nil];
    }
}

void MacRoomWindow::close_window()
{
    if (!window_closed_ && controller_)
    {
        window_closed_ = true;
        [controller_.window close];
    }
}

void MacRoomWindow::request_relayout()
{
    if (surface_)
    {
        surface_->relayout();
    }
}

void MacRoomWindow::update_window_title_(const std::string& name)
{
    if (!window_closed_ && controller_)
    {
        NSString* title = [NSString stringWithUTF8String:name.c_str()] ?: @"";
        [controller_.window setTitle:title];
    }
}

void MacRoomWindow::on_gif_results(std::uint64_t request_id,
                                   std::vector<tesseract::GifResult> results)
{
    if (popups_)
    {
        popups_->on_gif_results(request_id, std::move(results));
    }
}

void MacRoomWindow::on_gif_search_failed(std::uint64_t request_id,
                                         const std::string& message)
{
    if (popups_)
    {
        popups_->on_gif_search_failed(request_id, message);
    }
}

void MacRoomWindow::repaint_anim_frame()
{
    RoomWindowBase::repaint_anim_frame();
    if (popups_)
    {
        popups_->repaint_anim_frame();
    }
}

void MacRoomWindow::apply_theme(const tk::Theme& t)
{
    if (surface_)
    {
        surface_->set_theme(t);
        surface_->root()->apply_theme(t);
    }
    if (popups_)
        popups_->apply_theme(t);
    // Window chrome mirrors the main controller's -_applyTheme: policy: in
    // System mode leave it nil so it keeps tracking NSApp's (and thus the
    // OS's) appearance; only pin a concrete appearance for an explicit
    // Light/Dark override, so a pop-out doesn't stop following OS changes.
    if (!window_closed_ && controller_)
    {
        if (tesseract::Settings::instance().theme_pref ==
            tesseract::Settings::ThemePreference::System)
        {
            controller_.window.appearance = nil;
        }
        else
        {
            NSAppearanceName name = (t.mode == tk::ThemeMode::Dark)
                                        ? NSAppearanceNameDarkAqua
                                        : NSAppearanceNameAqua;
            controller_.window.appearance = [NSAppearance appearanceNamed:name];
        }
    }
}

void MacRoomWindow::apply_scale_change(float scale)
{
    if (surface_)
        surface_->apply_scale_change(scale);
}

void MacRoomWindow::surface_repaint_()
{
    if (surface_)
    {
        surface_->relayout();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RoomWindowController ObjC implementation
// ─────────────────────────────────────────────────────────────────────────────

@implementation RoomWindowController

@synthesize cppWindow = _cppWindow;

// Helper: save this popout window's geometry (top-left coords) to Settings.
- (void)_savePopoutGeometry
{
    if (!_cppWindow) return;
    NSRect f = self.window.frame;
    NSArray<NSScreen*>* screens = [NSScreen screens];
    if (!screens.count) return;
    const CGFloat primaryTop =
        [[screens firstObject] frame].origin.y +
        [[screens firstObject] frame].size.height;
    _cppWindow->save_popout_geometry_(
        static_cast<int>(f.origin.x),
        static_cast<int>(primaryTop - f.origin.y - f.size.height),
        static_cast<int>(f.size.width),
        static_cast<int>(f.size.height));
}

- (void)windowDidEndLiveResize:(NSNotification*)notification
{
    [self _savePopoutGeometry];
}

- (void)windowDidResize:(NSNotification*)notification
{
    if (_cppWindow) _cppWindow->dismiss_popups_on_resize();
}

- (void)windowDidMove:(NSNotification*)notification
{
    [self _savePopoutGeometry];
}

- (void)windowWillClose:(NSNotification*)notification
{
    (void)notification;
    if (_cppWindow)
    {
        _cppWindow->on_window_will_close();
        _cppWindow = nullptr; // prevent any further calls into the C++ object
    }
}

- (void)_windowDidChangeBackingProperties:(NSNotification*)note
{
    if (_cppWindow)
        _cppWindow->apply_scale_change((float)self.window.backingScaleFactor);
}

- (void)dealloc
{
    [[NSNotificationCenter defaultCenter] removeObserver:self];
}

- (void)keyDown:(NSEvent*)event
{
    if (event.keyCode == 53 /* kVK_Escape */ && _cppWindow)
    {
        if (_cppWindow->on_escape_key())
            return;
    }
    [super keyDown:event];
}

- (BOOL)validateUserInterfaceItem:(id<NSValidatedUserInterfaceItem>)item
{
    if (item.action == @selector(copy:))
    {
        auto* rv = _cppWindow ? _cppWindow->room_view() : nullptr;
        return rv && rv->has_active_selection();
    }
    return YES;
}

- (void)copy:(id)sender
{
    auto* rv = _cppWindow ? _cppWindow->room_view() : nullptr;
    if (rv)
        rv->copy_active_selection();
}

@end

// ─────────────────────────────────────────────────────────────────────────────
// C++ factory — called from MacShell::create_secondary_room_window_
// ─────────────────────────────────────────────────────────────────────────────

namespace tesseract
{

RoomWindowBase* make_mac_room_window(ShellBase*         shell,
                                     const std::string& room_id)
{
    return new MacRoomWindow(shell, room_id);
}

} // namespace tesseract
