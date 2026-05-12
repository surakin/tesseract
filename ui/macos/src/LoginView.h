#pragma once
#import <UIKit/UIKit.h>

#include <tesseract/client.h>

@class LoginView;

@protocol LoginViewDelegate <NSObject>
- (void)loginViewDidSucceed:(LoginView*)view;
@end

/// Inline sign-in view shown inside the main window when the user is not
/// logged in. Drives the same two-phase OAuth / MAS flow as before
/// (form → worker → browser → worker → done) but is a plain UIView the
/// main view controller swaps in instead of presenting modally.
@interface LoginView : UIView

- (instancetype)initWithClient:(tesseract::Client*)client;

@property (nonatomic, weak) id<LoginViewDelegate> delegate;

/// Return the view to its initial "form" state. Cancels any in-flight
/// OAuth and clears errors. Call before showing the view again.
- (void)reset;

/// Display a status message above the form (e.g. "Saved session expired").
- (void)setStatusMessage:(NSString*)message;

@end
