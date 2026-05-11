#pragma once
#import <AppKit/AppKit.h>

#include <tesseract/client.h>

@class LoginView;

@protocol LoginViewDelegate <NSObject>
- (void)loginViewDidSucceed:(LoginView*)view;
@end

/// Inline sign-in view shown inside the main window when the user is not
/// logged in. Drives the same two-phase OAuth / MAS flow as the previous
/// modal LoginWindowController (form → worker → browser → worker → done),
/// but is a plain NSView the main window swaps in instead of presenting as
/// a sheet.
@interface LoginView : NSView

- (instancetype)initWithClient:(tesseract::Client*)client;

@property (nonatomic, weak) id<LoginViewDelegate> delegate;

/// Return the view to its initial "form" state. Cancels any in-flight
/// OAuth and clears errors. Call before showing the view again.
- (void)reset;

/// Display a status message above the form (e.g. "Saved session expired").
- (void)setStatusMessage:(NSString*)message;

@end
