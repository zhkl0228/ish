//
//  UIApplication+OpenURL.m
//  iSH
//
//  Created by Theodore Dubois on 9/23/18.
//

#import "UIApplication+OpenURL.h"
#import "SceneDelegate.h"
#include "kernel/errno.h"
#include "util/sync.h"

@implementation UIApplication (OpenURL)

+ (void)openURL:(NSString *)url {
    [[self sharedApplication] openURL:[NSURL URLWithString:url] options:@{} completionHandler:nil];
}

@end

// What became of a request: PENDING until it is answered, then 0 (opened) or
// the error the program's write gives
#define PENDING 1

@interface OpenURLRequest : NSObject
@property (nonatomic) NSURL *url;
// Under requests_lock
@property (nonatomic) int answer;
// The main thread's: the alert asking (weak: its actions hold the request),
// and whether the program stopped waiting for it (a signal: it was ended, or
// gave up)
@property (nonatomic, weak) UIAlertController *alert;
@property (nonatomic) BOOL withdrawn;
@end

@implementation OpenURLRequest
@end

// One alert at a time: another program's request waits until the one asked
// now is answered
static lock_t requests_lock = LOCK_INITIALIZER;
static cond_t requests_cond = COND_INITIALIZER;
static bool asking;

static void answer_request(OpenURLRequest *request, int answer) {
    lock(&requests_lock);
    request.answer = answer;
    notify(&requests_cond);
    unlock(&requests_lock);
}

// The main thread's: the alert, in the terminal window in front, over what it
// shows already (Settings, another sheet)
static void present_request(OpenURLRequest *request) {
    TerminalViewController *terminal = currentTerminalViewController;
    if (terminal == nil) {
        NSLog(@"open_url: no terminal window in front to ask in, not opened: %@", request.url);
        answer_request(request, _ENODEV);
        return;
    }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Open URL?"
                                                                   message:request.url.absoluteString
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *_) {
        answer_request(request, _ECANCELED);
    }]];
    UIAlertAction *open = [UIAlertAction actionWithTitle:@"Open" style:UIAlertActionStyleDefault handler:^(UIAlertAction *_) {
        [UIApplication.sharedApplication openURL:request.url options:@{} completionHandler:^(BOOL success) {
            if (!success)
                NSLog(@"open_url: iOS did not open %@", request.url);
            answer_request(request, success ? 0 : _EIO);
        }];
    }];
    [alert addAction:open];
    alert.preferredAction = open;
    request.alert = alert;
    UIViewController *presenter = terminal;
    while (presenter.presentedViewController != nil)
        presenter = presenter.presentedViewController;
    [presenter presentViewController:alert animated:YES completion:^{
        // Withdrawn while it was coming up, when it could not be dismissed
        if (request.withdrawn)
            [alert dismissViewControllerAnimated:YES completion:nil];
    }];
}

static void withdraw_request(OpenURLRequest *request) {
    request.withdrawn = YES;
    UIAlertController *alert = request.alert;
    if (alert != nil && alert.presentingViewController != nil && !alert.isBeingPresented)
        [alert dismissViewControllerAnimated:YES completion:nil];
}

int ask_to_open_url(const char *bytes, size_t size) {
    NSString *string = [[NSString alloc] initWithBytes:bytes length:size encoding:NSUTF8StringEncoding];
    NSURL *url = string == nil ? nil : [NSURL URLWithString:string];
    if (url == nil || !([url.scheme isEqualToString:@"http"] || [url.scheme isEqualToString:@"https"])) {
        NSLog(@"open_url: not an http or https URL, not opened: %@", string ?: [NSData dataWithBytes:bytes length:size]);
        return _EINVAL;
    }
    OpenURLRequest *request = [OpenURLRequest new];
    request.url = url;
    request.answer = PENDING;

    lock(&requests_lock);
    while (asking) {
        int err = wait_for(&requests_cond, &requests_lock, NULL);
        if (err < 0) {
            unlock(&requests_lock);
            return err;
        }
    }
    asking = true;
    unlock(&requests_lock);

    dispatch_async(dispatch_get_main_queue(), ^{
        present_request(request);
    });

    lock(&requests_lock);
    int answer;
    while ((answer = request.answer) == PENDING) {
        int err = wait_for(&requests_cond, &requests_lock, NULL);
        if (err < 0) {
            answer = err;
            dispatch_async(dispatch_get_main_queue(), ^{
                withdraw_request(request);
            });
            break;
        }
    }
    asking = false;
    notify(&requests_cond);
    unlock(&requests_lock);
    return answer;
}
