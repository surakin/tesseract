#import "MacWebAuthSession.h"

#import <AuthenticationServices/AuthenticationServices.h>
#import <Foundation/Foundation.h>

#include <atomic>
#include <memory>
#include <utility>

// Supplies the window the sign-in sheet attaches to. The window is held weakly:
// the session must not keep a closed window alive.
@interface TesseractWebAuthAnchor : NSObject <ASWebAuthenticationPresentationContextProviding>
- (instancetype)initWithWindow:(NSWindow*)window;
@end

@implementation TesseractWebAuthAnchor
{
    __weak NSWindow* _window;
}

- (instancetype)initWithWindow:(NSWindow*)window
{
    self = [super init];
    if (self)
    {
        _window = window;
    }
    return self;
}

- (ASPresentationAnchor)presentationAnchorForWebAuthenticationSession:
    (ASWebAuthenticationSession*)session
{
    NSWindow* w = _window;
    return w ?: NSApp.keyWindow ?: NSApp.mainWindow ?: NSApp.windows.firstObject;
}

@end

namespace tesseract::macos
{

namespace
{

// Shared by the completion handler and the cancel function. Holds the session
// and anchor strongly (ASWebAuthenticationSession keeps its provider only
// weakly) until the flow ends. All callers are on the main thread.
struct WebAuthState
{
    ASWebAuthenticationSession* session = nil;
    TesseractWebAuthAnchor* anchor = nil;
    views::WebAuthDone done;
    std::atomic<bool> live{true}; // atomic as cheap insurance; callers are main-thread

    // Drops the ObjC objects, which also breaks the state <-> handler cycle.
    void release_objects()
    {
        session = nil;
        anchor = nil;
    }

    void finish(views::WebAuthResult result)
    {
        auto cb = std::move(done);
        done = nullptr;
        release_objects();
        if (live.exchange(false) && cb)
        {
            cb(std::move(result));
        }
    }
};

} // namespace

std::function<void()> start_web_auth_session(NSWindow* anchor, const std::string& auth_url,
                                             const std::string& callback_scheme,
                                             views::WebAuthDone done)
{
    auto state = std::make_shared<WebAuthState>();
    state->done = std::move(done);

    NSURL* url = [NSURL URLWithString:[NSString stringWithUTF8String:auth_url.c_str()]];
    NSString* scheme = [NSString stringWithUTF8String:callback_scheme.c_str()];
    if (!url || !scheme)
    {
        NSLog(@"Web auth: invalid sign-in URL or callback scheme");
        dispatch_async(dispatch_get_main_queue(), ^{
            state->finish({views::WebAuthResult::Outcome::Failed, {}});
        });
        return [state]
        {
            state->live = false;
        };
    }

    // The completion handler may run on any thread; hop to main before touching state.
    ASWebAuthenticationSession* session = [[ASWebAuthenticationSession alloc]
              initWithURL:url
        callbackURLScheme:scheme
        completionHandler:^(NSURL* callbackURL, NSError* error) {
            views::WebAuthResult result;
            if (callbackURL)
            {
                result.outcome = views::WebAuthResult::Outcome::Callback;
                result.url = callbackURL.absoluteString.UTF8String ?: "";
            }
            else if ([error.domain isEqualToString:ASWebAuthenticationSessionErrorDomain] &&
                     error.code == ASWebAuthenticationSessionErrorCodeCanceledLogin)
            {
                result.outcome = views::WebAuthResult::Outcome::Cancelled;
            }
            else
            {
                NSLog(@"Web auth failed: %@", error);
                result.outcome = views::WebAuthResult::Outcome::Failed;
            }
            dispatch_async(dispatch_get_main_queue(), ^{
                state->finish(result);
            });
        }];

    TesseractWebAuthAnchor* anchor_obj = [[TesseractWebAuthAnchor alloc] initWithWindow:anchor];
    session.presentationContextProvider = anchor_obj;
    session.prefersEphemeralWebBrowserSession = NO;
    state->session = session;
    state->anchor = anchor_obj;

    if (![session start])
    {
        NSLog(@"Web auth: ASWebAuthenticationSession failed to start");
        dispatch_async(dispatch_get_main_queue(), ^{
            state->finish({views::WebAuthResult::Outcome::Failed, {}});
        });
    }

    return [state]
    {
        state->live = false; // synchronous, so `done` cannot fire after this returns
        auto cancel = [state]
        {
            ASWebAuthenticationSession* s = state->session;
            state->release_objects();
            [s cancel];
        };
        if ([NSThread isMainThread])
        {
            cancel();
        }
        else
        {
            dispatch_async(dispatch_get_main_queue(), ^{
                cancel();
            });
        }
    };
}

} // namespace tesseract::macos
