//
//  Screenshot.m
//  iSH
//
//  /proc/ish/screenshot: what the screen shows of iSH, a PNG, for a program
//  (Claude Code) to see the app as the person does. iSH's own windows only: an
//  app draws no other's, and none of its own while it is not in front.
//

#import <UIKit/UIKit.h>
#import "Screenshot.h"
#import "SceneDelegate.h"
#import "TerminalViewController.h"
#include "kernel/errno.h"
#include "util/sync.h"

@interface ScreenshotRequest : NSObject
// Under screenshot_lock: whether the main thread has drawn it, and what (nil:
// no terminal window was in front)
@property (nonatomic) BOOL drawn;
@property (nonatomic, nullable) UIImage *image;
@end

@implementation ScreenshotRequest
@end

static lock_t screenshot_lock = LOCK_INITIALIZER;
static cond_t screenshot_cond = COND_INITIALIZER;

// The main thread's: the windows of iSH's active scenes on the screen of the
// terminal window in front, each where it is on that screen and over those of
// lower levels, at the screen's scale; black where there is none (another
// app's part of the screen in Split View). nil with no terminal window in
// front (currentTerminalViewController, which an inactive scene is not).
//
// The terminal's window as iOS drew it on the screen (drawViewHierarchyInRect,
// once the changes pending are drawn), the others' layers rendered here: the
// UITextEffectsWindow over it, which has the keyboard's bar. On a phone,
// 2026-11-27: the layers rendered here leave out what iOS draws itself, the
// glass of the clipboard's shelf and of the column's keys, and Settings'
// first page altogether, which the phone showed (842); every window drawn as
// iOS drew it gave a black PNG (840), the UITextEffectsWindow opaque so drawn
// over the terminal's; drawn thus, each shows all of it (843).
static UIImage *DrawScreen(void) {
    UIWindowScene *front = currentTerminalViewController.view.window.windowScene;
    if (front == nil)
        return nil;
    UIScreen *screen = front.screen;
    NSMutableArray<UIWindow *> *windows = [NSMutableArray new];
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        if (![scene isKindOfClass:UIWindowScene.class] || scene.activationState != UISceneActivationStateForegroundActive)
            continue;
        UIWindowScene *windowScene = (UIWindowScene *) scene;
        if (windowScene.screen != screen)
            continue;
        for (UIWindow *window in windowScene.windows) {
            if (!window.hidden)
                [windows addObject:window];
        }
    }
    // Stable: windows of a level in the order their scenes list them
    [windows sortWithOptions:NSSortStable usingComparator:^NSComparisonResult(UIWindow *a, UIWindow *b) {
        if (a.windowLevel == b.windowLevel)
            return NSOrderedSame;
        return a.windowLevel < b.windowLevel ? NSOrderedAscending : NSOrderedDescending;
    }];
    UIGraphicsImageRendererFormat *format = [UIGraphicsImageRendererFormat preferredFormat];
    format.scale = screen.scale;
    format.opaque = YES;
    // 8 bits a channel: the extended range a wide color screen is given has
    // 16, a PNG twice the size
    format.preferredRange = UIGraphicsImageRendererFormatRangeStandard;
    UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithBounds:screen.bounds format:format];
    return [renderer imageWithActions:^(UIGraphicsImageRendererContext *context) {
        [UIColor.blackColor setFill];
        [context fillRect:screen.bounds];
        for (UIWindow *window in windows) {
            CGRect frame = [window convertRect:window.bounds toCoordinateSpace:screen.coordinateSpace];
            if ([window.rootViewController isKindOfClass:TerminalViewController.class]) {
                [window drawViewHierarchyInRect:frame afterScreenUpdates:YES];
            } else {
                CGContextSaveGState(context.CGContext);
                CGContextTranslateCTM(context.CGContext, frame.origin.x, frame.origin.y);
                [window.layer renderInContext:context.CGContext];
                CGContextRestoreGState(context.CGContext);
            }
        }
    }];
}

// On the reading program's thread: drawn on the main thread, which it waits
// for (a signal ends the wait, as the read's error), and made a PNG here, off
// the main thread
int take_app_screenshot(void **png, size_t *size) {
    @autoreleasepool {
        ScreenshotRequest *request = [ScreenshotRequest new];
        dispatch_async(dispatch_get_main_queue(), ^{
            UIImage *image = DrawScreen();
            lock(&screenshot_lock);
            request.image = image;
            request.drawn = YES;
            notify(&screenshot_cond);
            unlock(&screenshot_lock);
        });
        lock(&screenshot_lock);
        while (!request.drawn) {
            int err = wait_for(&screenshot_cond, &screenshot_lock, NULL);
            if (err < 0) {
                unlock(&screenshot_lock);
                return err;
            }
        }
        UIImage *image = request.image;
        unlock(&screenshot_lock);
        if (image == nil)
            return _EAGAIN;
        NSData *data = UIImagePNGRepresentation(image);
        *png = malloc(data.length);
        if (*png == NULL)
            return _ENOMEM;
        memcpy(*png, data.bytes, data.length);
        *size = data.length;
        return 0;
    }
}
