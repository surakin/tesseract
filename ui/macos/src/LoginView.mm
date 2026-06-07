#import "LoginView.h"

#include "tk/host.h"
#include "tk/host_macos.h"
#include "tk/theme.h"
#include "views/LoginView.h"

#include <memory>

namespace
{

std::string nsstr(NSString* s)
{
    return s ? std::string(s.UTF8String ? s.UTF8String : "") : std::string{};
}

} // namespace

@implementation LoginView
{
    std::unique_ptr<tk::macos::Surface> _surface;
    tesseract::views::LoginView*        _shared; // borrowed from surface
}

- (instancetype)init
{
    self = [super initWithFrame:NSZeroRect];
    if (!self)
        return nil;

    _surface = std::make_unique<tk::macos::Surface>(tk::Theme::light());

    auto view = std::make_unique<tesseract::views::LoginView>();
    _shared   = view.get();

    __weak LoginView* weakSelf = self;

    std::weak_ptr<bool> w = _shared->alive_token();
    _shared->set_post_to_ui(
        [weakSelf, w](std::function<void()> fn)
        {
            LoginView* s = weakSelf;
            if (!s)
                return;
            s->_surface->host().post_to_ui(
                [w, fn = std::move(fn)]
                {
                    if (!w.expired())
                        fn();
                });
        });
    _shared->set_relayout(
        [weakSelf]
        {
            if (LoginView* s = weakSelf)
                s->_surface->relayout();
        });
    _shared->set_on_success(
        [weakSelf]
        {
            LoginView* s = weakSelf;
            if (s && [s.delegate respondsToSelector:@selector(loginViewDidSucceed:)])
                [s.delegate loginViewDidSucceed:s];
        });
    _shared->set_on_cancel_done(
        [weakSelf]
        {
            LoginView* s = weakSelf;
            if (s && [s.delegate respondsToSelector:@selector(loginViewDidCancel:)])
                [s.delegate loginViewDidCancel:s];
        });
    _shared->set_on_begin_oauth(
        [weakSelf]
        {
            LoginView* s = weakSelf;
            if (s && s.onBeginOAuth)
                s.onBeginOAuth();
        });

    _surface->set_on_layout(
        [weakSelf]
        {
            if (LoginView* s = weakSelf)
                s->_shared->position_overlay();
        });
    _surface->set_root(std::move(view));
    _shared->init_with_field(_surface->host().make_text_field());

    NSView* surfaceView = (__bridge NSView*)_surface->view_handle();
    surfaceView.translatesAutoresizingMaskIntoConstraints = NO;
    [self addSubview:surfaceView];
    [NSLayoutConstraint activateConstraints:@[
        [surfaceView.topAnchor constraintEqualToAnchor:self.topAnchor],
        [surfaceView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
        [surfaceView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [surfaceView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];

    return self;
}

- (void)dealloc
{
    if (_shared)
        _shared->shutdown();
}

- (BOOL)isFlipped
{
    return YES;
}

- (void)setClient:(tesseract::Client*)client
{
    if (_shared)
        _shared->set_client(client);
}

- (void)setRunAsync:(void (^)(void (^body)(void)))runAsync
{
    if (!_shared)
        return;
    if (!runAsync)
    {
        _shared->set_run_async({});
        return;
    }
    auto run = [runAsync](std::function<void()> fn)
    {
        runAsync(^{ fn(); });
    };
    _shared->set_run_async(std::move(run));
}

- (void)setTheme:(const tk::Theme&)t
{
    if (_surface)
        _surface->set_theme(t);
}

- (void)setMode:(tesseract::views::LoginView::Mode)mode
{
    if (_shared)
        _shared->set_mode(mode);
}

- (void)reset
{
    if (_shared)
        _shared->reset();
}

- (void)setStatusMessage:(NSString*)message
{
    if (_shared)
        _shared->set_status_message(nsstr(message));
}

- (void)showRestoreError:(NSString*)body
           retryCallback:(void (^)(void))retryCallback
{
    if (!_shared) return;
    __block void (^cb)(void) = [retryCallback copy];
    _shared->show_restore_error(nsstr(body), [cb]() { if (cb) cb(); });
}

@end
