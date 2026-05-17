#import "RoomWindowController.h"
#include "util.h"
#include "app/ShellBase.h"
#include "app/RoomWindowBase.h"
#include "tk/host_macos.h"
#include "views/RoomView.h"
#include "views/MessageListView.h"
#include <tesseract/client.h>

// ─────────────────────────────────────────────────────────────────────────────
// Forward declaration — lets @interface reference MacRoomWindow before the
// C++ class definition.
// ─────────────────────────────────────────────────────────────────────────────

class MacRoomWindow;

@interface RoomWindowController ()
@property (nonatomic, assign) MacRoomWindow* cppWindow;
@end

// ─────────────────────────────────────────────────────────────────────────────
// MacRoomWindow — C++ RoomWindowBase subclass for macOS pop-out windows
// ─────────────────────────────────────────────────────────────────────────────

class MacRoomWindow : public tesseract::RoomWindowBase {
public:
    MacRoomWindow(
        tesseract::ShellBase* shell,
        const std::string& room_id,
        const std::unordered_map<std::string, tesseract::views::UrlPreviewData>* preview_data);
    ~MacRoomWindow() override;

    void bring_to_front()                              override;
    void close_window()                                override;
    void request_relayout()                            override;
    void update_window_title_(const std::string& name) override;

    // Called by -windowWillClose: delegate method.
    void on_window_will_close() {
        window_closed_ = true;
        schedule_self_close_();
    }

private:
    __strong RoomWindowController* controller_ = nil;
    std::unique_ptr<tk::macos::Surface> surface_;
    std::unique_ptr<tk::NativeTextArea> text_area_;
    bool window_closed_ = false;
};

// ─────────────────────────────────────────────────────────────────────────────
// MacRoomWindow implementation
// ─────────────────────────────────────────────────────────────────────────────

MacRoomWindow::MacRoomWindow(
    tesseract::ShellBase* shell,
    const std::string& room_id,
    const std::unordered_map<std::string, tesseract::views::UrlPreviewData>* preview_data)
    : tesseract::RoomWindowBase(shell, room_id)
{
    NSRect frame = NSMakeRect(0, 0, 800, 600);
    NSWindowStyleMask style =
        NSWindowStyleMaskTitled         |
        NSWindowStyleMaskClosable       |
        NSWindowStyleMaskMiniaturizable |
        NSWindowStyleMaskResizable;
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

    // ── RoomView providers ───────────────────────────────────────────────────
    room_view_->set_avatar_provider(
        [this](const std::string& mxc) -> const tk::Image* {
            return shell_avatar_(mxc);
        });
    room_view_->set_image_provider(
        [this](const std::string& mxc) -> const tk::Image* {
            return shell_image_(mxc);
        });
    room_view_->set_preview_provider(
        [preview_data](const std::string& url) -> const tesseract::views::UrlPreviewData* {
            if (!preview_data) return nullptr;
            auto it = preview_data->find(url);
            return it == preview_data->end() ? nullptr : &it->second;
        });
    if (auto player = surface_->host().make_audio_player())
        room_view_->set_audio_player(std::move(player));
    room_view_->set_voice_bytes_provider(
        [this](const std::string& source_json) -> std::vector<std::uint8_t> {
            return fetch_source_bytes_(source_json);
        });

    // ── Repaint / layout ─────────────────────────────────────────────────────
    room_view_->set_repaint_requester([this] {
        if (surface_) surface_->relayout();
    });
    room_view_->on_layout_changed = [this] {
        if (surface_) surface_->relayout();
    };

    // ── Compose callbacks ─────────────────────────────────────────────────────
    room_view_->on_send = [this](const std::string& body) {
        std::string trimmed = tesseract::macos::trim(body);
        if (trimmed.empty()) return;
        send_message_(trimmed);
        if (text_area_) text_area_->set_text("");
        if (room_view_) room_view_->set_current_text({});
    };
    room_view_->on_send_reply = [this](const std::string& reply_id,
                                        const std::string& body) {
        if (body.empty()) return;
        send_reply_(reply_id, body);
        if (text_area_) text_area_->set_text("");
        if (room_view_) room_view_->set_current_text({});
    };
    room_view_->on_send_edit = [this](const std::string& event_id,
                                       const std::string& new_body) {
        if (new_body.empty()) return;
        send_edit_(event_id, new_body);
        if (text_area_) text_area_->set_text("");
        if (room_view_) room_view_->set_current_text({});
    };
    room_view_->on_edit_cancelled = [this] {
        if (text_area_) text_area_->set_text("");
        if (room_view_) room_view_->set_current_text({});
    };
    room_view_->on_edit_prefill = [this](const std::string& body) {
        if (text_area_) text_area_->set_text(body);
    };
    room_view_->on_reply_focus = [this] {
        if (text_area_) text_area_->set_focused(true);
    };
    room_view_->on_delete_requested = [this](const std::string& event_id) {
        delete_event_(event_id);
    };
    room_view_->on_reaction_toggled =
        [this](const std::string& event_id, const std::string& key) {
            toggle_reaction_(event_id, key);
        };
    room_view_->on_receipt_needed = [this](const std::string& event_id) {
        send_receipt_(event_id);
    };
    room_view_->on_link_clicked = [](const std::string& url) {
        tesseract::Client::open_in_browser(url);
    };
    room_view_->on_near_top = [this] { request_pagination_back_(); };

    // ── NativeTextArea overlay ────────────────────────────────────────────────
    text_area_ = surface_->host().make_text_area();
    text_area_->set_placeholder("Message\xe2\x80\xa6");
    text_area_->set_on_changed([this](const std::string& s) {
        bool typing = !s.empty();
        if (typing != compose_typing_active_) {
            compose_typing_active_ = typing;
            send_typing_notice_(typing);
        }
        if (room_view_) room_view_->set_current_text(s);
    });
    text_area_->set_on_submit([this] {
        if (room_view_) room_view_->compose_bar()->trigger_send();
    });
    text_area_->set_on_height_changed([this](float h) {
        if (room_view_) room_view_->set_text_area_natural_height(h);
        if (surface_) surface_->relayout();
    });
    surface_->set_on_layout([this] {
        if (room_view_ && text_area_)
            text_area_->set_rect(room_view_->compose_text_area_rect());
    });

    // Wire up the ObjC window controller.
    controller_ = [[RoomWindowController alloc] initWithWindow:win];
    controller_.cppWindow = this;
    [win setDelegate:controller_];

    [win makeKeyAndOrderFront:nil];

    finish_init_();
}

MacRoomWindow::~MacRoomWindow() {
    close_window();
}

void MacRoomWindow::bring_to_front() {
    if (!window_closed_ && controller_)
        [controller_.window makeKeyAndOrderFront:nil];
}

void MacRoomWindow::close_window() {
    if (!window_closed_ && controller_) {
        window_closed_ = true;
        [controller_.window close];
    }
}

void MacRoomWindow::request_relayout() {
    if (surface_) surface_->relayout();
}

void MacRoomWindow::update_window_title_(const std::string& name) {
    if (!window_closed_ && controller_) {
        NSString* title = [NSString stringWithUTF8String:name.c_str()] ?: @"";
        [controller_.window setTitle:title];
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// RoomWindowController ObjC implementation
// ─────────────────────────────────────────────────────────────────────────────

@implementation RoomWindowController

@synthesize cppWindow = _cppWindow;

- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    if (_cppWindow) {
        _cppWindow->on_window_will_close();
        _cppWindow = nullptr;  // prevent any further calls into the C++ object
    }
}

@end

// ─────────────────────────────────────────────────────────────────────────────
// C++ factory — called from MacShell::create_secondary_room_window_
// ─────────────────────────────────────────────────────────────────────────────

namespace tesseract {

RoomWindowBase* make_mac_room_window(
    ShellBase* shell,
    const std::string& room_id,
    const std::unordered_map<std::string, views::UrlPreviewData>* preview_data)
{
    return new MacRoomWindow(shell, room_id, preview_data);
}

} // namespace tesseract
