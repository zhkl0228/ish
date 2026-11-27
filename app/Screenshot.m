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
#include "kernel/errno.h"
#include "util/sync.h"
#include "debug.h"

@interface ScreenshotRequest : NSObject
// Under screenshot_lock: whether the main thread has drawn it, and what (nil:
// no terminal window was in front), with what it said of the drawing and how
// many windows it drew
@property (nonatomic) BOOL drawn;
@property (nonatomic, nullable) UIImage *image;
@property (nonatomic) NSString *said;
@property (nonatomic) int windowsDrawn;
@end

@implementation ScreenshotRequest
@end

static lock_t screenshot_lock = LOCK_INITIALIZER;
static cond_t screenshot_cond = COND_INITIALIZER;

static const char *ActivationStateName(UISceneActivationState state) {
    switch (state) {
    case UISceneActivationStateUnattached: return "unattached";
    case UISceneActivationStateForegroundActive: return "active";
    case UISceneActivationStateForegroundInactive: return "inactive";
    case UISceneActivationStateBackground: return "background";
    }
    return "unknown";
}

// The main thread's: the windows of iSH's active scenes on the screen of the
// terminal window in front, each where it is on that screen and over those of
// lower levels, at the screen's scale, by method; black where there is none
// (another app's part of the screen in Split View). nil with no terminal
// window in front (currentTerminalViewController, which an inactive scene is
// not). What it did goes to said, for the kernel log: the screen, each scene,
// each window and whether it was drawn, as *drawn counts them.
static UIImage *DrawScreen(enum screenshot_method method, NSMutableString *said, int *drawn) {
    [said appendFormat:@"screenshot (%s):", screenshot_method_names[method]];
    *drawn = 0;
    UIWindowScene *front = currentTerminalViewController.view.window.windowScene;
    if (front == nil) {
        [said appendString:@" no terminal window in front"];
        return nil;
    }
    UIScreen *screen = front.screen;
    [said appendFormat:@" screen %@ @%.0f;", NSStringFromCGRect(screen.bounds), screen.scale];
    NSMutableArray<UIWindow *> *windows = [NSMutableArray new];
    for (UIScene *scene in UIApplication.sharedApplication.connectedScenes) {
        BOOL isWindowScene = [scene isKindOfClass:UIWindowScene.class];
        BOOL here = isWindowScene && ((UIWindowScene *) scene).screen == screen;
        [said appendFormat:@" scene %@ %s%s%s;", NSStringFromClass(scene.class), ActivationStateName(scene.activationState),
                           scene == front ? ", the terminal's" : "", here ? "" : ", not on the screen"];
        if (!here || scene.activationState != UISceneActivationStateForegroundActive)
            continue;
        for (UIWindow *window in ((UIWindowScene *) scene).windows) {
            if (window.hidden)
                [said appendFormat:@" %@ hidden;", NSStringFromClass(window.class)];
            else
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
            BOOL done;
            switch (method) {
            case SCREENSHOT_VIEW:
                done = [window drawViewHierarchyInRect:frame afterScreenUpdates:NO];
                break;
            case SCREENSHOT_VIEW_UPDATED:
                done = [window drawViewHierarchyInRect:frame afterScreenUpdates:YES];
                break;
            case SCREENSHOT_LAYER:
                CGContextSaveGState(context.CGContext);
                CGContextTranslateCTM(context.CGContext, frame.origin.x, frame.origin.y);
                [window.layer renderInContext:context.CGContext];
                CGContextRestoreGState(context.CGContext);
                done = YES;
                break;
            }
            [said appendFormat:@" %@ %@ level %.0f alpha %.2f: %s;", NSStringFromClass(window.class),
                               NSStringFromCGRect(frame), window.windowLevel, window.alpha,
                               done ? "drawn" : "not drawn"];
            *drawn += done;
        }
    }];
}

// On the reading program's thread: drawn on the main thread, which it waits
// for (a signal ends the wait, as the read's error), and made a PNG here, off
// the main thread. What the drawing did goes to the kernel log.
int take_app_screenshot(enum screenshot_method method, void **png, size_t *size) {
    @autoreleasepool {
        ScreenshotRequest *request = [ScreenshotRequest new];
        dispatch_async(dispatch_get_main_queue(), ^{
            NSMutableString *said = [NSMutableString new];
            int drawn;
            UIImage *image = DrawScreen(method, said, &drawn);
            lock(&screenshot_lock);
            request.image = image;
            request.said = said;
            request.windowsDrawn = drawn;
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
        int windowsDrawn = request.windowsDrawn;
        NSString *said = request.said;
        unlock(&screenshot_lock);
        printk("%s\n", said.UTF8String);
        if (image == nil)
            return _EAGAIN;
        if (windowsDrawn == 0)
            return _EIO;
        NSData *data = UIImagePNGRepresentation(image);
        *png = malloc(data.length);
        if (*png == NULL)
            return _ENOMEM;
        memcpy(*png, data.bytes, data.length);
        *size = data.length;
        return 0;
    }
}
