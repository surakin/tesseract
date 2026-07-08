#import "RoomWindowController.h"
#import "EmojiPicker.h"
#import "StickerPicker.h"
#include "app/ShellBase.h"
#include "app/RoomWindowBase.h"
#include "tk/host_macos.h"
#include "tk/i18n.h"
#include "views/ImageViewerOverlay.h"
#include "views/PopoutRoomWidget.h"
#include "views/VideoViewerOverlay.h"
#include "views/MessageListView.h"
#include "views/MentionController.h"
#include "views/MentionPopup.h"

#include <tesseract/client.h>
#include <tesseract/image_pack.h>

#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declaration — lets @interface reference MacRoomWindow before the
// C++ class definition.
// ─────────────────────────────────────────────────────────────────────────────

class MacRoomWindow;

@interface RoomWindowController ()
@property(nonatomic, assign) MacRoomWindow* cppWindow;
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

    // Called by -windowWillClose: delegate method.
    void on_window_will_close()
    {
        window_closed_ = true;
        schedule_self_close_();
    }

    // Expose protected RoomWindowBase members to ObjC++ callers.
    using tesseract::RoomWindowBase::save_popout_geometry_;

    // Called by -keyDown: when Escape is pressed.
    bool on_escape_key()
    {
        if (room_view_ && room_view_->room_search_open())
        {
            room_view_->close_room_search();
            return true;
        }
        if (vid_viewer_ && vid_viewer_->is_open())
        {
            vid_viewer_->close();
            vid_viewer_->set_visible(false);
            if (surface_) surface_->relayout();
            return true;
        }
        if (img_viewer_ && img_viewer_->is_open())
        {
            img_viewer_->close();
            img_viewer_->set_visible(false);
            if (surface_) surface_->relayout();
            return true;
        }
        return false;
    }

protected:
    void surface_repaint_() override;
    tk::NativeTextArea* compose_text_area_() override
    {
        return text_area_.get();
    }
    tk::EncodedImage encode_for_send_(const std::uint8_t* data,
                                      std::size_t size, bool compress) override
    {
        return surface_ ? surface_->host().encode_for_send(data, size, compress)
                        : tk::EncodedImage{};
    }
    bool
    put_image_on_clipboard_(std::span<const std::uint8_t> bytes) override
    {
        return surface_ && surface_->host().set_clipboard_image(bytes);
    }
    void post_delayed_(int ms, std::function<void()> fn) override
    {
        if (surface_)
            surface_->host().post_delayed(ms, std::move(fn));
    }

    void show_mention_popup_(tk::Rect cursor, int rows);
    void hide_mention_popup_();

private:
    // Configure + show the shared emoji / sticker panels anchored to this
    // pop-out's surface, routing selection to this window's room/composer.
    void show_emoji_panel_(tk::Rect anchor);
    void show_sticker_panel_(tk::Rect anchor);

    __strong RoomWindowController* controller_ = nil;
    std::unique_ptr<tk::macos::Surface> surface_;
    std::unique_ptr<tk::NativeTextArea> text_area_;
    std::unique_ptr<tk::NativeTextField> search_field_;
    NSPanel* mention_panel_ = nil;
    std::unique_ptr<tk::macos::Surface> mention_popup_surface_;
    tesseract::views::MentionPopup* mention_popup_widget_ = nullptr;
    std::unique_ptr<tesseract::views::MentionController> mention_controller_;
    // Reaction picker: set by on_add_reaction_requested, consumed by the next
    // emoji selection to send a reaction instead of inserting text.
    std::string pending_reaction_event_id_;
    NSPopover* topic_tooltip_popover_ = nil;
    bool link_hovered_ = false;
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

    auto room_widget = std::make_unique<tesseract::views::PopoutRoomWidget>();
    room_view_  = room_widget->room_view();
    img_viewer_ = room_widget->image_viewer();
    vid_viewer_ = room_widget->video_viewer();
    surface_->set_root(std::move(room_widget));

    // ── Shared RoomView wiring (providers + compose callbacks + overlays) ─
    wire_room_view_(room_view_);

    // ── Video player for this window's VideoViewerOverlay ────────────────────
    if (auto player = surface_->host().make_video_player())
    {
        vid_viewer_->set_video_player(std::move(player));
    }

    // ── Image / video save dialogs ────────────────────────────────────────────
    img_viewer_->on_save =
        [this](std::string source_url, std::string filename_hint)
    {
        NSSavePanel* panel = [NSSavePanel savePanel];
        NSString* suggested = filename_hint.empty()
            ? @"image"
            : [NSString stringWithUTF8String:filename_hint.c_str()];
        panel.nameFieldStringValue = suggested;
        NSModalResponse resp = [panel runModal];
        if (resp != NSModalResponseOK || !panel.URL)
            return;
        save_source_to_file_(std::move(source_url),
                              std::string(panel.URL.path.UTF8String));
    };
    vid_viewer_->on_save =
        [this](std::string source_json, std::string mime_type)
    {
        NSString* suggested = @"video";
        if (mime_type == "video/mp4")
            suggested = @"video.mp4";
        else if (mime_type == "video/webm")
            suggested = @"video.webm";
        NSSavePanel* panel = [NSSavePanel savePanel];
        panel.nameFieldStringValue = suggested;
        NSModalResponse resp = [panel runModal];
        if (resp != NSModalResponseOK || !panel.URL)
            return;
        save_source_to_file_(std::move(source_json),
                              std::string(panel.URL.path.UTF8String));
    };

    // ── Surface-bound providers (need this shell's own surface_) ─────────────
    if (auto player = surface_->host().make_audio_player())
    {
        room_view_->set_audio_player(std::move(player));
    }

    // Drag-and-drop file ingest into this pop-out's compose bar (shared base
    // routes the payload + runs the shell's media probe against this window).
    surface_->set_on_file_drop(
        [this](std::vector<std::uint8_t> bytes, std::string mime,
               std::string filename)
        {
            handle_file_drop_(std::move(bytes), std::move(mime),
                              std::move(filename));
        });
    surface_->set_on_file_drop_error(
        [this](std::string reason)
        {
            shell_show_status_message_(std::move(reason));
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
    room_view_->message_list()->on_show_copy_menu = [this]()
    {
        if (!surface_) return;
        auto* ml = room_view_->message_list();
        NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
        NSMenuItem* item = [[NSMenuItem alloc]
            initWithTitle:NSLocalizedString(@"Copy", nil)
                   action:@selector(copy:)
            keyEquivalent:@""];
        [menu addItem:item];
        NSEvent* event = [NSApp currentEvent];
        NSView* view = (__bridge NSView*)surface_->view_handle();
        if (event && view)
            [NSMenu popUpContextMenu:menu withEvent:event forView:view];
    };

    // ── NativeTextArea overlay ────────────────────────────────────────────────
    text_area_ = surface_->host().make_text_area();
    text_area_->set_font_role(tk::FontRole::Body);
    text_area_->set_placeholder("Message\xe2\x80\xa6");
    text_area_->set_mention_colors(surface_->theme().palette.accent,
                                   surface_->theme().palette.text_on_accent);
    text_area_->set_on_changed(
        [this](const std::string& s)
        {
            bool typing = !s.empty();
            if (typing != compose_typing_active_)
            {
                compose_typing_active_ = typing;
                send_typing_notice_(typing);
            }
            if (room_view_)
            {
                room_view_->set_current_text(s);
            }
            if (mention_controller_)
            {
                mention_controller_->on_text_changed(
                    s, text_area_->cursor_byte_pos());
            }
        });
    text_area_->set_on_submit(
        [this]
        {
            if (mention_controller_ && mention_controller_->on_submit())
            {
                return;
            }
            if (room_view_)
            {
                room_view_->compose_bar()->trigger_send();
            }
        });
    text_area_->set_on_popup_nav(
        [this](tk::NativeTextArea::NavKey nk) -> bool
        { return mention_controller_ && mention_controller_->on_nav(nk); });
    text_area_->set_on_height_changed(
        [this](float h)
        {
            if (room_view_)
            {
                room_view_->set_text_area_natural_height(h);
            }
            if (surface_)
            {
                surface_->relayout();
            }
        });
    surface_->set_on_layout(
        [this]
        {
            if (room_view_ && text_area_)
            {
                text_area_->set_rect(room_view_->compose_text_area_rect());
            }
            if (room_view_ && search_field_)
            {
                const bool vis = room_view_->room_search_field_visible();
                search_field_->set_visible(vis);
                if (vis)
                {
                    tk::Rect r = room_view_->room_search_field_rect();
                    r.x += 2; r.y += 2; r.w -= 4; r.h -= 4;
                    search_field_->set_rect(r);
                }
            }
        });

    // ── In-room search native text field ─────────────────────────────────
    search_field_ = surface_->host().make_text_field();
    search_field_->set_placeholder(tk::tr("Find in conversation\xe2\x80\xa6"));
    search_field_->set_visible(false);
    search_field_->set_on_changed(
        [this](const std::string& q)
        {
            if (room_view_)
                if (auto* bar = room_view_->room_search_bar())
                {
                    bar->set_query(q);
                    if (surface_) surface_->relayout();
                }
        });
    search_field_->set_on_popup_nav(
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

    // ── @mention autocomplete popup + controller ──────────────────────────
    {
        NSRect mf = NSMakeRect(0, 0, tesseract::views::MentionPopup::kWidth,
                               tesseract::views::MentionPopup::kRowHeight);
        mention_panel_ = [[NSPanel alloc]
            initWithContentRect:mf
                      styleMask:NSWindowStyleMaskNonactivatingPanel |
                                NSWindowStyleMaskBorderless
                        backing:NSBackingStoreBuffered
                          defer:NO];
        mention_panel_.floatingPanel = YES;
        mention_panel_.hidesOnDeactivate = NO;
        mention_panel_.becomesKeyOnlyIfNeeded = YES;
        mention_popup_surface_ =
            std::make_unique<tk::macos::Surface>(surface_->theme());
        auto mw = std::make_unique<tesseract::views::MentionPopup>();
        mention_popup_widget_ = mw.get();
        mention_popup_surface_->set_root(std::move(mw));
        [mention_panel_ setContentView:(__bridge NSView*)
                                           mention_popup_surface_->view_handle()];

        tesseract::views::MentionController::Hooks hooks;
        hooks.show = [this](tk::Rect cursor, int rows)
        { show_mention_popup_(cursor, rows); };
        hooks.hide = [this] { hide_mention_popup_(); };
        hooks.repaint = [this]
        {
            if (mention_popup_surface_)
                mention_popup_surface_->relayout();
        };
        hooks.room_id = [this] { return room_id_; };
        hooks.run_async = [this](std::function<void()> fn)
        { run_async_(std::move(fn)); };
        hooks.post_to_ui = [this](std::function<void()> fn)
        { post_to_ui_(std::move(fn)); };
        wire_mention_shell_hooks_(mention_popup_widget_, hooks);
        mention_controller_ =
            std::make_unique<tesseract::views::MentionController>(
                text_area_.get(), shell_client_(), mention_popup_widget_,
                std::move(hooks));
    }

    // ── Platform popups the shared wire_room_view_ can't provide ──────────
    room_view_->on_emoji = [this](tk::Rect btn) { show_emoji_panel_(btn); };
    room_view_->on_sticker = [this](tk::Rect btn) { show_sticker_panel_(btn); };
    room_view_->on_add_reaction_requested =
        [this](const std::string& event_id, tk::Rect anchor)
    {
        pending_reaction_event_id_ = event_id;
        show_emoji_panel_(anchor);
    };
    room_view_->on_show_tooltip = [this](std::string text, tk::Rect anchor)
    {
        if (!surface_)
            return;
        if (!topic_tooltip_popover_)
        {
            NSTextField* lbl = [NSTextField wrappingLabelWithString:@""];
            lbl.translatesAutoresizingMaskIntoConstraints = NO;
            NSView* cv = [[NSView alloc] init];
            cv.translatesAutoresizingMaskIntoConstraints = NO;
            [cv addSubview:lbl];
            [NSLayoutConstraint activateConstraints:@[
                [lbl.leadingAnchor constraintEqualToAnchor:cv.leadingAnchor
                                                  constant:8],
                [lbl.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor
                                                   constant:-8],
                [lbl.topAnchor constraintEqualToAnchor:cv.topAnchor constant:6],
                [lbl.bottomAnchor constraintEqualToAnchor:cv.bottomAnchor
                                                 constant:-6],
                [cv.widthAnchor constraintLessThanOrEqualToConstant:360],
            ]];
            NSViewController* vc = [[NSViewController alloc] init];
            vc.view = cv;
            NSPopover* pop = [[NSPopover alloc] init];
            pop.contentViewController = vc;
            pop.behavior = NSPopoverBehaviorSemitransient;
            pop.animates = NO;
            topic_tooltip_popover_ = pop;
        }
        NSTextField* lbl =
            (NSTextField*)topic_tooltip_popover_.contentViewController.view
                .subviews.firstObject;
        lbl.stringValue = [NSString stringWithUTF8String:text.c_str()] ?: @"";
        [topic_tooltip_popover_.contentViewController.view
                layoutSubtreeIfNeeded];
        topic_tooltip_popover_.contentSize =
            topic_tooltip_popover_.contentViewController.view.fittingSize;
        NSView* view = (__bridge NSView*)surface_->view_handle();
        NSRect anchorRect = NSMakeRect(anchor.x, anchor.y, anchor.w, anchor.h);
        [topic_tooltip_popover_ showRelativeToRect:anchorRect
                                            ofView:view
                                     preferredEdge:NSRectEdgeMinY];
    };
    room_view_->on_hide_tooltip = [this]
    {
        if (topic_tooltip_popover_ && topic_tooltip_popover_.shown)
        {
            [topic_tooltip_popover_ close];
        }
    };
    room_view_->on_link_hovered = [this](const std::string& url)
    {
        if (!url.empty() && !link_hovered_)
        {
            [[NSCursor pointingHandCursor] push];
            link_hovered_ = true;
        }
        else if (url.empty() && link_hovered_)
        {
            [NSCursor pop];
            link_hovered_ = false;
        }
    };

    // Wire up the ObjC window controller.
    controller_ = [[RoomWindowController alloc] initWithWindow:win];
    controller_.cppWindow = this;
    [win setDelegate:controller_];

    [win makeKeyAndOrderFront:nil];

    finish_init_();
}

MacRoomWindow::~MacRoomWindow()
{
    close_window();
}

void MacRoomWindow::show_emoji_panel_(tk::Rect anchor)
{
    if (!surface_)
        return;
    EmojiPickerPanel* panel = [EmojiPickerPanel sharedPanel];
    panel.client = shell_client_();
    [panel setTheme:surface_->theme()];
    [panel setImageProvider:picker_image_provider_(false)];
    // The panel is a process-wide singleton that can outlive this pop-out; guard
    // the stored blocks against use-after-free with the liveness token.
    std::weak_ptr<bool> alive_weak = alive_;
    __weak EmojiPickerPanel* weakPanel = panel;
    panel.onSelect = ^(NSString* glyph) {
        auto a = alive_weak.lock();
        if (!a || !*a)
            return;
        std::string g = glyph.UTF8String ?: "";
        if (g.empty())
            return;
        if (!pending_reaction_event_id_.empty())
        {
            std::string ev = pending_reaction_event_id_;
            pending_reaction_event_id_.clear();
            toggle_reaction_(ev, g, std::string{});
            [weakPanel close];
            return;
        }
        if (text_area_)
        {
            text_area_->insert_at_cursor(g);
            if (room_view_)
                room_view_->set_current_text(text_area_->text());
            text_area_->set_focused(true);
        }
    };
    panel.onEmoticonSelect = ^(const tesseract::ImagePackImage& img) {
        auto a = alive_weak.lock();
        if (!a || !*a)
            return;
        if (img.url.empty())
            return;
        if (!pending_reaction_event_id_.empty())
        {
            std::string ev = pending_reaction_event_id_;
            pending_reaction_event_id_.clear();
            toggle_reaction_(ev, std::string{}, img.url);
            [weakPanel close];
            return;
        }
        if (text_area_)
        {
            const tk::Image* image = picker_image_provider_(false)(img.url, img.url);
            int pos = text_area_->cursor_byte_pos();
            text_area_->insert_emoticon(pos, pos, img.shortcode, img.url, image);
            if (room_view_)
                room_view_->set_current_text(text_area_->text());
            text_area_->set_focused(true);
        }
    };
    NSView* anchorView = (__bridge NSView*)surface_->view_handle();
    [panel popupAtRect:anchor inView:anchorView];
}

void MacRoomWindow::show_sticker_panel_(tk::Rect anchor)
{
    if (!surface_ || room_id_.empty())
        return;
    StickerPickerPanel* panel = [StickerPickerPanel sharedPanel];
    panel.client = shell_client_();
    [panel setTheme:surface_->theme()];
    [panel setImageProvider:picker_image_provider_(true)];
    std::weak_ptr<bool> alive_weak = alive_;
    __weak StickerPickerPanel* weakPanel = panel;
    panel.onSelected = ^(NSString* url, NSString* body, NSString* infoJson) {
        auto a = alive_weak.lock();
        if (!a || !*a)
            return;
        if (room_id_.empty())
            return;
        std::string u = url.UTF8String ?: "";
        std::string b = body.UTF8String ?: "";
        std::string j = infoJson.UTF8String ?: "{}";
        if (auto* c = shell_client_())
        {
            c->send_sticker(room_id_, b, u, j);
        }
        [weakPanel orderOut:nil];
    };
    NSView* anchorView = (__bridge NSView*)surface_->view_handle();
    [panel popupAtRect:anchor inView:anchorView];
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

void MacRoomWindow::show_mention_popup_(tk::Rect cursor, int rows)
{
    if (!mention_panel_ || !surface_)
    {
        return;
    }
    NSSize size = NSMakeSize(tesseract::views::MentionPopup::kWidth,
                             rows * tesseract::views::MentionPopup::kRowHeight);
    [mention_panel_ setContentSize:size];
    if (mention_popup_surface_)
    {
        mention_popup_surface_->relayout();
    }
    NSView* hostView = (__bridge NSView*)surface_->view_handle();
    NSPoint localPt = NSMakePoint(cursor.x, cursor.y);
    NSPoint windowPt = [hostView convertPoint:localPt toView:nil];
    NSPoint screenPt = [hostView.window convertPointToScreen:windowPt];
    NSRect sf = mention_panel_.screen ? mention_panel_.screen.visibleFrame
                                      : [NSScreen mainScreen].visibleFrame;
    CGFloat panelH = size.height;
    CGFloat y_above = screenPt.y + 4;
    CGFloat y_below = screenPt.y - (CGFloat)cursor.h - 4 - panelH;
    CGFloat x = screenPt.x;
    CGFloat y = (y_above + panelH <= sf.origin.y + sf.size.height) ? y_above
                                                                   : y_below;
    x = std::clamp(x, sf.origin.x, sf.origin.x + sf.size.width - size.width);
    y = std::clamp(y, sf.origin.y, sf.origin.y + sf.size.height - size.height);
    [mention_panel_ setFrameOrigin:NSMakePoint(x, y)];
    [mention_panel_ orderFront:nil];
}

void MacRoomWindow::hide_mention_popup_()
{
    [mention_panel_ orderOut:nil];
}

void MacRoomWindow::apply_theme(const tk::Theme& t)
{
    if (surface_)
    {
        surface_->set_theme(t);
    }
    if (mention_popup_surface_)
    {
        mention_popup_surface_->set_theme(t);
    }
    if (text_area_)
    {
        text_area_->set_mention_colors(t.palette.accent,
                                       t.palette.text_on_accent);
    }
    // Window chrome follows the app-wide NSApp.appearance set by the main
    // controller's -_applyTheme:, but pin it on this window too so a
    // pop-out opened before the next app-appearance change is consistent.
    if (!window_closed_ && controller_)
    {
        NSAppearanceName name = (t.mode == tk::ThemeMode::Dark)
                                    ? NSAppearanceNameDarkAqua
                                    : NSAppearanceNameAqua;
        controller_.window.appearance = [NSAppearance appearanceNamed:name];
    }
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
        auto* ml = rv ? rv->message_list() : nullptr;
        return ml && ml->has_selection();
    }
    return YES;
}

- (void)copy:(id)sender
{
    auto* rv = _cppWindow ? _cppWindow->room_view() : nullptr;
    auto* ml = rv ? rv->message_list() : nullptr;
    if (ml)
        ml->copy_selection();
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
