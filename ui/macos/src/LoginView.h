#pragma once
#import <AppKit/AppKit.h>

#include <tesseract/client.h>
#include "views/LoginView.h"
#include "tk/theme.h"

@class LoginView;

@protocol LoginViewDelegate <NSObject>
- (void)loginViewDidSucceed:(LoginView*)view;
- (void)loginViewDidCancel:(LoginView*)view;
@end

/// Inline sign-in view shown inside the main window when the user is not
/// logged in. Visuals come from the shared `tesseract::views::LoginView`
/// rendered through a `tk::macos::Surface` NSView; the OAuth state
/// machine + worker thread + native NSTextField overlay live here.
@interface LoginView : NSView

- (instancetype)init;

/// Rebind before each login attempt.
- (void)setClient:(tesseract::Client*)client;

/// Called on the main thread just before the OAuth worker thread starts.
/// Set by MainWindow to lazily create the pending directory on first sign-in.
@property(nonatomic, copy) void (^onBeginOAuth)(void);

/// Initial = no cancel button; AddAccount = cancel visible in Form + Waiting.
- (void)setMode:(tesseract::views::LoginView::Mode)mode;

/// Re-skin the login surface when the theme preference changes.
- (void)setTheme:(const tk::Theme&)t;

@property(nonatomic, weak) id<LoginViewDelegate> delegate;

/// Return the view to its initial "form" state.
- (void)reset;

/// Display a status message above the form (e.g. "Saved session expired").
- (void)setStatusMessage:(NSString*)message;

@end
