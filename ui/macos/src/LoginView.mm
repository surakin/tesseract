#import "LoginView.h"

#include "tk/host.h"
#include "tk/host_macos.h"
#include "tk/theme.h"
#include "views/LoginView.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace {

std::string nsstr(NSString* s) {
    return s ? std::string(s.UTF8String ? s.UTF8String : "") : std::string{};
}

std::string trim(std::string s) {
    auto a = s.find_first_not_of(" \t\n\r");
    auto b = s.find_last_not_of (" \t\n\r");
    if (a == std::string::npos) return {};
    return s.substr(a, b - a + 1);
}

} // namespace

@implementation LoginView {
    tesseract::Client*                              _client;
    std::unique_ptr<tk::macos::Surface>             _surface;
    tesseract::views::LoginView*                    _shared;       // borrowed
    std::unique_ptr<tk::NativeTextField>            _hsField;

    std::thread                                     _worker;
    std::atomic<bool>                               _cancelled;
    std::atomic<uint32_t>                           _discoveryGen;
}

- (instancetype)init {
    self = [super initWithFrame:NSZeroRect];
    if (!self) return nil;
    _client       = nullptr;
    _cancelled    = false;
    _discoveryGen = 0;

    _surface = std::make_unique<tk::macos::Surface>(tk::Theme::light());

    auto shared = std::make_unique<tesseract::views::LoginView>();
    _shared     = shared.get();
    __weak LoginView* weakSelf = self;
    _shared->on_sign_in = [weakSelf] {
        LoginView* s = weakSelf;
        if (s) [s _onSignIn];
    };
    _shared->on_cancel = [weakSelf] {
        LoginView* s = weakSelf;
        if (s) [s _onCancel];
    };
    _surface->set_root(std::move(shared));

    NSView* surfaceView = (__bridge NSView*)_surface->view_handle();
    surfaceView.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:surfaceView];
    [NSLayoutConstraint activateConstraints:@[
        [surfaceView.topAnchor      constraintEqualToAnchor:self.topAnchor],
        [surfaceView.leadingAnchor  constraintEqualToAnchor:self.leadingAnchor],
        [surfaceView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [surfaceView.bottomAnchor   constraintEqualToAnchor:self.bottomAnchor],
    ]];

    _hsField = _surface->host().make_text_field();
    _hsField->set_placeholder("matrix.org or @user:matrix.org");
    _hsField->set_text("matrix.org");
    _hsField->set_on_submit([weakSelf] {
        LoginView* s = weakSelf;
        if (s) [s _onSignIn];
    });
    _hsField->set_on_changed([weakSelf](const std::string& text) {
        LoginView* s = weakSelf;
        if (!s || !s->_shared) return;
        uint32_t gen = ++s->_discoveryGen;
        if (text.empty()) {
            s->_shared->set_discovery_state(
                tesseract::views::LoginView::DiscoveryState::Idle);
            s->_surface->relayout();
            return;
        }
        s->_shared->set_discovery_state(
            tesseract::views::LoginView::DiscoveryState::Discovering);
        s->_surface->relayout();

        auto* snap = s->_client;
        if (!snap) return;

        std::thread([weakSelf, gen, snap, text] {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
            LoginView* strongSelf = weakSelf;
            if (!strongSelf || gen != strongSelf->_discoveryGen.load()) return;
            auto result = snap->discover_homeserver(text);
            strongSelf->_surface->host().post_to_ui(
                [weakSelf, gen, result = std::move(result)] {
                    LoginView* s = weakSelf;
                    if (!s || gen != s->_discoveryGen.load() || !s->_shared) return;
                    if (result)
                        s->_shared->set_discovery_state(
                            tesseract::views::LoginView::DiscoveryState::Resolved,
                            result.base_url);
                    else
                        s->_shared->set_discovery_state(
                            tesseract::views::LoginView::DiscoveryState::Failed,
                            result.error);
                    s->_surface->relayout();
                });
        }).detach();
    });

    _surface->set_on_layout([weakSelf] {
        LoginView* s = weakSelf;
        if (s) [s _positionOverlay];
    });

    return self;
}

- (void)setClient:(tesseract::Client*)client {
    _client = client;
}

- (void)setMode:(tesseract::views::LoginView::Mode)mode {
    if (_shared) _shared->set_mode(mode);
    if (_surface) _surface->relayout();
}

- (void)dealloc {
    ++_discoveryGen;
    _cancelled = true;
    if (_client) _client->cancel_oauth();
    if (_worker.joinable()) _worker.join();
}

- (BOOL)isFlipped { return YES; }

- (void)_positionOverlay {
    if (!_shared || !_hsField) return;
    _hsField->set_rect(_shared->homeserver_field_rect());
}

// ---------------------------------------------------------------------------

- (void)reset {
    ++_discoveryGen;  // invalidate any in-flight discovery callback
    _cancelled = true;
    if (_client) _client->cancel_oauth();
    if (_worker.joinable()) _worker.join();
    _cancelled = false;

    _shared->set_status("");
    _shared->set_discovery_state(tesseract::views::LoginView::DiscoveryState::Idle);
    _shared->set_state(tesseract::views::LoginView::State::Form);
    _hsField->set_enabled(true);
    _hsField->set_visible(true);
    _hsField->set_focused(true);
    _surface->relayout();
}

- (void)setStatusMessage:(NSString*)message {
    if (!_shared) return;
    if (!message || message.length == 0) {
        _shared->set_status("");
    } else {
        _shared->set_status(nsstr(message));
    }
    _surface->relayout();
}

- (void)_onSignIn {
    if (!_client) return;
    std::string hs_raw = trim(_hsField->text());
    if (hs_raw.empty()) {
        _shared->set_status("Please enter a homeserver.",
                             tk::Color::rgb(0xB00020));
        _surface->relayout();
        return;
    }
    // Use the pre-resolved URL when available; extract server name from MXID
    // otherwise so begin_oauth doesn't receive a raw @user:server string.
    std::string hs;
    using DS = tesseract::views::LoginView::DiscoveryState;
    if (_shared->discovery_state() == DS::Resolved) {
        hs = _shared->resolved_base_url();
    } else if (!hs_raw.empty() && hs_raw.front() == '@') {
        auto colon = hs_raw.find(':');
        hs = (colon != std::string::npos) ? hs_raw.substr(colon + 1) : hs_raw;
    } else {
        hs = hs_raw;
    }
    _shared->set_status("");
    _hsField->set_enabled(false);
    _shared->set_state(tesseract::views::LoginView::State::Waiting);
    _surface->relayout();

    if (_worker.joinable()) _worker.join();
    _cancelled = false;
    if (self.onBeginOAuth) self.onBeginOAuth();
    __weak LoginView* weakSelf = self;
    _worker = std::thread([weakSelf, hs] {
        LoginView* strongSelf = weakSelf;
        if (!strongSelf) return;
        auto flow = strongSelf->_client->begin_oauth(hs);
        if (strongSelf->_cancelled.load()) return;
        bool        ok      = static_cast<bool>(flow);
        std::string payload = ok ? flow.auth_url : flow.message;
        strongSelf->_surface->host().post_to_ui(
            [weakSelf, ok, payload = std::move(payload)] {
                LoginView* s = weakSelf;
                if (s) [s _onBeginCompleted:ok payload:payload];
            });
    });
}

- (void)_onBeginCompleted:(bool)ok payload:(std::string)payload {
    if (_worker.joinable()) _worker.join();
    if (!ok) {
        _shared->set_status("Sign-in failed: " + payload,
                             tk::Color::rgb(0xB00020));
        _shared->set_state(tesseract::views::LoginView::State::Form);
        _hsField->set_enabled(true);
        _surface->relayout();
        return;
    }
    tesseract::Client::open_in_browser(payload);

    _cancelled = false;
    __weak LoginView* weakSelf = self;
    _worker = std::thread([weakSelf] {
        LoginView* strongSelf = weakSelf;
        if (!strongSelf) return;
        auto res = strongSelf->_client->await_oauth();
        if (strongSelf->_cancelled.load()) return;
        bool        ok  = static_cast<bool>(res);
        std::string msg = res.message;
        strongSelf->_surface->host().post_to_ui(
            [weakSelf, ok, msg = std::move(msg)] {
                LoginView* s = weakSelf;
                if (s) [s _onAwaitCompleted:ok message:msg];
            });
    });
}

- (void)_onAwaitCompleted:(bool)ok message:(std::string)err {
    if (_worker.joinable()) _worker.join();
    if (ok) {
        if ([self.delegate respondsToSelector:@selector(loginViewDidSucceed:)]) {
            [self.delegate loginViewDidSucceed:self];
        }
        return;
    }
    _shared->set_status("Sign-in failed: " + err,
                         tk::Color::rgb(0xB00020));
    _shared->set_state(tesseract::views::LoginView::State::Form);
    _hsField->set_enabled(true);
    _surface->relayout();
}

- (void)_onCancel {
    _cancelled = true;
    if (_client) _client->cancel_oauth();
    _shared->set_status("Cancelling…");
    _surface->relayout();
    if (_worker.joinable()) _worker.join();
    _shared->set_status("");
    _shared->set_state(tesseract::views::LoginView::State::Form);
    _hsField->set_enabled(true);
    _surface->relayout();
    if ([self.delegate respondsToSelector:@selector(loginViewDidCancel:)]) {
        [self.delegate loginViewDidCancel:self];
    }
}

@end
