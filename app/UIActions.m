//
//  UIActions.m
//  iSH
//
//  /proc/ish/ui (ish-ui): an action in iSH's window, for a program (Claude
//  Code) to bring up what it then looks at (ish-screenshot). One of:
//
//    settings [appearance|keyboard|icon|jit|clipboard]
//                         Settings, or one of its pages, over the terminal
//    back                 the page before, in what is over the terminal
//    close                what is over the terminal, closed
//    tab N                the Nth tab
//    keyboard show|hide   the keyboard, given to the terminal or taken
//    clipboard show|hide  the clipboard's shelf
//
//  Done without animation; the write returns once the window shows it, the
//  keyboard's moves (which iOS animates) made. EINVAL for any other action,
//  EAGAIN while no terminal window is in front, EBUSY while an alert waits
//  for the person's answer (a program's URL to open among them), which is
//  theirs to give.
//

#import <UIKit/UIKit.h>
#import "UIActions.h"
#import "SceneDelegate.h"
#import "TerminalViewController.h"
#import "BuiltinJITViewController.h"
#include "kernel/errno.h"
#include "util/sync.h"

// What became of a request: PENDING until it is done, then 0 or the error
// the program's write gives
#define PENDING 1

@interface UIActionRequest : NSObject
// Under ui_lock
@property (nonatomic) int result;
@end

@implementation UIActionRequest
@end

static lock_t ui_lock = LOCK_INITIALIZER;
static cond_t ui_cond = COND_INITIALIZER;

static void Finish(UIActionRequest *request, int result) {
    lock(&ui_lock);
    request.result = result;
    notify(&ui_cond);
    unlock(&ui_lock);
}

// The keyboard's moves since it was made (a first responder taken or given up
// sets them going, which iOS then animates): those the keyboard said it will
// make (UIKeyboardWill...) and has not made yet (UIKeyboardDid...). A move
// made that began before is none of them. Its observers keep it till it stops.
@interface KeyboardMoves : NSObject
@property (nonatomic) int pending;
@property (nonatomic, copy) void (^settled)(void);
@property (nonatomic) NSArray *observers;
@end

@implementation KeyboardMoves

- (instancetype)init {
    if (self = [super init]) {
        NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
        void (^will)(NSNotification *) = ^(NSNotification *_) {
            self.pending++;
        };
        void (^did)(NSNotification *) = ^(NSNotification *_) {
            [self moved];
        };
        self.observers = @[
            [center addObserverForName:UIKeyboardWillShowNotification object:nil queue:nil usingBlock:will],
            [center addObserverForName:UIKeyboardWillHideNotification object:nil queue:nil usingBlock:will],
            [center addObserverForName:UIKeyboardDidShowNotification object:nil queue:nil usingBlock:did],
            [center addObserverForName:UIKeyboardDidHideNotification object:nil queue:nil usingBlock:did],
        ];
    }
    return self;
}

- (void)moved {
    if (self.pending == 0)
        return;
    self.pending--;
    if (self.pending == 0 && self.settled != nil) {
        void (^settled)(void) = self.settled;
        [self stop];
        settled();
    }
}

// settled called once every move said is made: at once if none is to be
- (void)whenSettled:(void (^)(void))settled {
    if (self.pending == 0) {
        [self stop];
        settled();
    } else {
        self.settled = settled;
    }
}

- (void)stop {
    for (id observer in self.observers)
        [NSNotificationCenter.defaultCenter removeObserver:observer];
    self.observers = nil;
    self.settled = nil;
}

@end

// The main thread's: the action the words name, done; 0, or the write's error
static int Perform(NSArray<NSString *> *words, TerminalViewController *terminal) {
    for (UIViewController *shown = terminal.presentedViewController; shown != nil; shown = shown.presentedViewController) {
        if ([shown isKindOfClass:UIAlertController.class])
            return _EBUSY;
    }
    if ([words isEqualToArray:@[@"settings"]]) {
        [terminal showSettingsPage:nil];
        return 0;
    }
    if (words.count == 2 && [words[0] isEqualToString:@"settings"]) {
        NSString *page = words[1];
        UIStoryboard *about = [UIStoryboard storyboardWithName:@"About" bundle:nil];
        if ([page isEqualToString:@"appearance"])
            [terminal showSettingsPage:[about instantiateViewControllerWithIdentifier:@"Appearance"]];
        else if ([page isEqualToString:@"keyboard"])
            [terminal showSettingsPage:[about instantiateViewControllerWithIdentifier:@"ExternalKeyboard"]];
        else if ([page isEqualToString:@"icon"])
            [terminal showSettingsPage:[about instantiateViewControllerWithIdentifier:@"AppIcon"]];
        else if ([page isEqualToString:@"jit"])
            [terminal showSettingsPage:[BuiltinJITViewController new]];
        else if ([page isEqualToString:@"clipboard"])
            [terminal showClipboardSettingsPage];
        else
            return _EINVAL;
        return 0;
    }
    if ([words isEqualToArray:@[@"back"]]) {
        UIViewController *top = terminal.presentedViewController;
        while (top.presentedViewController != nil)
            top = top.presentedViewController;
        if (![top isKindOfClass:UINavigationController.class] || ((UINavigationController *) top).viewControllers.count < 2)
            return _EINVAL;
        [(UINavigationController *) top popViewControllerAnimated:NO];
        return 0;
    }
    if ([words isEqualToArray:@[@"close"]]) {
        [terminal dismissViewControllerAnimated:NO completion:nil];
        return 0;
    }
    if (words.count == 2 && [words[0] isEqualToString:@"tab"]) {
        NSInteger n = words[1].integerValue;
        // the number as written, none other (integerValue reads "2x" as 2)
        if (![@(n).stringValue isEqualToString:words[1]] || n < 1 || (NSUInteger) n > terminal.tabCount)
            return _EINVAL;
        [terminal selectTabAtIndex:(NSUInteger) n - 1];
        return 0;
    }
    if ([words isEqualToArray:@[@"keyboard", @"show"]]) {
        terminal.keyboardShown = YES;
        return 0;
    }
    if ([words isEqualToArray:@[@"keyboard", @"hide"]]) {
        terminal.keyboardShown = NO;
        return 0;
    }
    if ([words isEqualToArray:@[@"clipboard", @"show"]]) {
        [terminal showClipboard];
        return 0;
    }
    if ([words isEqualToArray:@[@"clipboard", @"hide"]]) {
        [terminal hideClipboard];
        return 0;
    }
    return _EINVAL;
}

// On the writing program's thread: done on the main thread, which it waits
// for (a signal ends the wait, as the write's error)
int do_app_ui_action(const char *bytes, size_t size) {
    @autoreleasepool {
        NSString *text = [[NSString alloc] initWithBytes:bytes length:size encoding:NSUTF8StringEncoding];
        // echo's newline taken off
        if ([text hasSuffix:@"\n"])
            text = [text substringToIndex:text.length - 1];
        NSArray<NSString *> *words = [text componentsSeparatedByString:@" "];
        UIActionRequest *request = [UIActionRequest new];
        request.result = PENDING;
        dispatch_async(dispatch_get_main_queue(), ^{
            TerminalViewController *terminal = currentTerminalViewController;
            if (terminal == nil) {
                Finish(request, _EAGAIN);
                return;
            }
            KeyboardMoves *moves = [KeyboardMoves new];
            __block int result;
            [UIView performWithoutAnimation:^{
                result = Perform(words, terminal);
            }];
            if (result < 0) {
                [moves stop];
                Finish(request, result);
                return;
            }
            // A turn later, when the keyboard has said what it will do
            dispatch_async(dispatch_get_main_queue(), ^{
                [moves whenSettled:^{
                    [terminal.view.window layoutIfNeeded];
                    Finish(request, 0);
                }];
            });
        });
        lock(&ui_lock);
        int result;
        while ((result = request.result) == PENDING) {
            int err = wait_for(&ui_cond, &ui_lock, NULL);
            if (err < 0) {
                result = err;
                break;
            }
        }
        unlock(&ui_lock);
        return result;
    }
}
