//
//  AppDelegate.h
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import <UIKit/UIKit.h>

@interface AppDelegate : UIResponder <UIApplicationDelegate>

@property (strong, nonatomic) UIWindow *window;
- (void)exitApp;

+ (int)bootError;

/// Once per launch, offers to reset the default filesystem if the built-in one
/// has changed since it was set up.
+ (void)maybeOfferRootResetOnViewController:(UIViewController *)vc;
/// Replaces the default filesystem with the built-in one and quits, so that the
/// next launch boots from it; on failure, tells the user on vc.
+ (void)resetDefaultRootAndQuitOnViewController:(UIViewController *)vc;

@end

extern NSString *const ProcessExitedNotification;

