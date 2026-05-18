#import "RoomWindowController.h"
#include "app/ShellBase.h"
#include "app/RoomWindowBase.h"
#include "tk/host_macos.h"
#include "views/RoomView.h"
#include "views/MessageListView.h"

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
    MacRoomWindow(
        tesseract::ShellBase* shell, const std::string& room_id,
        const std::unordered_map<std::string, tesseract::views::UrlPreviewData>*
            preview_data);
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

protected:
    void surface_repaint_() override;
    tk::NativeTextArea* compose_text_area_() override
    {
        return text_area_.get();
    }
    const tesseract::views::UrlPreviewData*
    preview_lookup_(const std::string& url) override;

private:
    __strong RoomWindowController* controller_ = nil;
    std::unique_ptr<tk::macos::Surface> surface_;
    std::unique_ptr<tk::NativeTextArea> text_area_;
    const std::unordered_map<std::string, tesseract::views::UrlPreviewData>*
        preview_data_ = nullptr;
    bool window_closed_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// MacRoomWindow implementation
// ─────────────────────────────────────────────────────────────────────────────

MacRoomWindow::MacRoomWindow(
    tesseract::ShellBase* shell, const std::string& room_id,
    const std::unordered_map<std::string, tesseract::views::UrlPreviewData>*
        preview_data)
    : tesseract::RoomWindowBase(shell, room_id), preview_data_(preview_data)
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
    [win center];

    surface_ = std::make_unique<tk::macos::Surface>(tk::Theme::light());
    NSView* surfaceView = (__bridge NSView*)surface_->view_handle();
    [win setContentView:surfaceView];

    auto room_root = std::make_unique<tesseract::views::RoomView>();
    room_view_ = room_root.get();
    surface_->set_root(std::move(room_root));

    // ── Shared RoomView wiring (providers + compose callbacks) ───────────────
    wire_room_view_(room_view_);

    // ── Surface-bound providers (need this shell's own surface_) ─────────────
    if (auto player = surface_->host().make_audio_player())
    {
        room_view_->set_audio_player(std::move(player));
    }
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

    // ── NativeTextArea overlay ────────────────────────────────────────────────
    text_area_ = surface_->host().make_text_area();
    text_area_->set_placeholder("Message\xe2\x80\xa6");
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
        });
    text_area_->set_on_submit(
        [this]
        {
            if (room_view_)
            {
                room_view_->compose_bar()->trigger_send();
            }
        });
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
        });

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

void MacRoomWindow::apply_theme(const tk::Theme& t)
{
    if (surface_)
    {
        surface_->set_theme(t);
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

const tesseract::views::UrlPreviewData*
MacRoomWindow::preview_lookup_(const std::string& url)
{
    if (!preview_data_)
    {
        return nullptr;
    }
    auto it = preview_data_->find(url);
    return it == preview_data_->end() ? nullptr : &it->second;
}

// ─────────────────────────────────────────────────────────────────────────────
// RoomWindowController ObjC implementation
// ─────────────────────────────────────────────────────────────────────────────

@implementation RoomWindowController

@synthesize cppWindow = _cppWindow;

- (void)windowWillClose:(NSNotification*)notification
{
    (void)notification;
    if (_cppWindow)
    {
        _cppWindow->on_window_will_close();
        _cppWindow = nullptr; // prevent any further calls into the C++ object
    }
}

@end

// ─────────────────────────────────────────────────────────────────────────────
// C++ factory — called from MacShell::create_secondary_room_window_
// ─────────────────────────────────────────────────────────────────────────────

namespace tesseract
{

RoomWindowBase* make_mac_room_window(
    ShellBase* shell, const std::string& room_id,
    const std::unordered_map<std::string, views::UrlPreviewData>* preview_data)
{
    return new MacRoomWindow(shell, room_id, preview_data);
}

} // namespace tesseract
