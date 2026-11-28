//
//  Terminal.m
//  iSH
//
//  Created by Theodore Dubois on 10/18/17.
//

#import "Terminal.h"
#import "DelayedUITask.h"
#import "UserPreferences.h"
#include "fs/devices.h"
#include "fs/tty.h"
#include "kernel/signal.h"
#include "debug.h"

extern struct tty_driver ios_pty_driver;

typedef struct tty *tty_t;

@interface Terminal () <WKScriptMessageHandler> {
    lock_t _dataLock;
    cond_t _dataConsumed;
}

@property BOOL loaded;
@property (nonatomic) tty_t tty;
// lock with dataLock for !linux and @synchronized(self) for linux
@property (nonatomic) NSMutableData *pendingData;
// sending output is an asynchronous thing due to javascript, this is used to ensure it doesn't happen twice at once
@property (nonatomic) BOOL outputInProgress;
// set by destroy (under dataLock for !linux); output is discarded from then on
@property (nonatomic) BOOL destroyed;
// set by hangup; the signals are only sent once
@property (nonatomic) BOOL hungUp;
// under dataLock: a page asked for, for output no view has taken (sendOutput:)
@property (nonatomic) BOOL pageAsked;

@property DelayedUITask *refreshTask;
@property DelayedUITask *scrollToBottomTask;

@property BOOL applicationCursor;

@property NSNumber *terminalsKey;
@property NSUUID *uuid;

@end

@implementation WeakScriptMessageHandler {
    __weak id <WKScriptMessageHandler> _handler;
}
- (instancetype)initWithHandler:(id <WKScriptMessageHandler>)handler {
    if (self = [super init])
        _handler = handler;
    return self;
}
- (void)userContentController:(WKUserContentController *)userContentController didReceiveScriptMessage:(WKScriptMessage *)message {
    [_handler userContentController:userContentController didReceiveScriptMessage:message];
}
@end

@interface CustomWebView : WKWebView
@end
@implementation CustomWebView
- (BOOL)canPerformAction:(SEL)action withSender:(id)sender {
    if (action == @selector(copy:) || action == @selector(paste:)) {
        return NO;
    }
    return [super canPerformAction:action withSender:sender];
}
@end

@implementation Terminal
@synthesize webView = _webView;

static const int BUF_SIZE = 1<<14;

static NSMapTable<NSNumber *, Terminal *> *terminals;
static NSMapTable<NSUUID *, Terminal *> *terminalsByUUID;

- (instancetype)initWithType:(int)type number:(int)num {
    @synchronized (Terminal.class) {
        self.terminalsKey = @(dev_make(type, num));
        Terminal *terminal = [terminals objectForKey:self.terminalsKey];
        if (terminal)
            return terminal;

        if (self = [super init]) {
            self.pendingData = [[NSMutableData alloc] initWithCapacity:BUF_SIZE];
            self.refreshTask = [[DelayedUITask alloc] initWithTarget:self action:@selector(refresh)];
            self.scrollToBottomTask = [[DelayedUITask alloc] initWithTarget:self action:@selector(scrollToBottom)];
            lock_init(&_dataLock);
            cond_init(&_dataConsumed);

            [terminals setObject:self forKey:self.terminalsKey];
            self.uuid = [NSUUID UUID];
            [terminalsByUUID setObject:self forKey:self.uuid];
        }
        return self;
    }
}

static NSString *const TERMINAL_HANDLERS[] = {@"load", @"log", @"sendInput", @"sendBinary", @"resize", @"propUpdate", @"setTitle", @"copyText",
                                              @"notify", @"programStatus"};

NSNotificationName const TerminalTitleDidChangeNotification = @"TerminalTitleDidChangeNotification";
NSNotificationName const TerminalAttentionNotification = @"TerminalAttentionNotification";
NSNotificationName const TerminalProgramStatusDidChangeNotification = @"TerminalProgramStatusDidChangeNotification";

- (WKWebView *)webView {
    if (_webView == nil) {
        WKWebViewConfiguration *config = [WKWebViewConfiguration new];
        id <WKScriptMessageHandler> handler = [[WeakScriptMessageHandler alloc] initWithHandler:self];
        for (size_t i = 0; i < sizeof(TERMINAL_HANDLERS)/sizeof(TERMINAL_HANDLERS[0]); i++)
            [config.userContentController addScriptMessageHandler:handler name:TERMINAL_HANDLERS[i]];
        // Start at the size of the screen, so that output written before the
        // terminal is shown wraps about where it will. Not much bigger: WebKit
        // gives a tiled layer whose tiles would take over 156 MB a reduced
        // contents scale, and keeps that scale when the layer shrinks back to
        // an untiled one (GraphicsLayerCA's contentsScaleLimitingFactor is only
        // recomputed while tiled). This used to be 10000x10000; when a
        // compositing pass ran at that size, as it now and then did for the
        // first terminal at launch, its text stayed blurry until it closed.
        CGRect webviewSize = {CGPointZero, UIScreen.mainScreen.bounds.size};
        _webView = [[CustomWebView alloc] initWithFrame:webviewSize configuration:config];
        if (@available(macOS 13.3, iOS 16.4, tvOS 16.4, *))
            _webView.inspectable = YES;
        _webView.scrollView.scrollEnabled = NO;
        NSURL *xtermHtmlFile = [NSBundle.mainBundle URLForResource:@"term" withExtension:@"html"];
        [_webView loadFileURL:xtermHtmlFile allowingReadAccessToURL:xtermHtmlFile];
    }
    return _webView;
}

+ (Terminal *)createPseudoTerminal:(struct tty **)tty {
    *tty = pty_open_fake(&ios_pty_driver);
    if (IS_ERR(*tty))
        return nil;
    return (__bridge Terminal *) (*tty)->data;
}

- (void)setTty:(tty_t)tty {
    @synchronized (self) {
        _tty = tty;
    }
    // The window's size from the page, if it has one already: a terminal no
    // view has shown (the consoles init's gettys open, tty1 to tty6) gets one
    // as a view shows it, the size then once it loads. Asking made it one, a
    // web content process loading the page for each console as the app
    // started: seven of them for one tab, a tenth of a second of a core each.
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self->_webView != nil)
            [self syncWindowSize];
    });
}

- (void)userContentController:(WKUserContentController *)userContentController didReceiveScriptMessage:(WKScriptMessage *)message {
    if ([message.name isEqualToString:@"load"]) {
        self.loaded = YES;
        [self.refreshTask schedule];
        // the tty's size, which it may not have had (setTty:)
        [self syncWindowSize];
        // make sure this setting works if it's set before loading
        self.enableVoiceOverAnnounce = self.enableVoiceOverAnnounce;
    } else if ([message.name isEqualToString:@"log"]) {
        NSLog(@"%@", message.body);
    } else if ([message.name isEqualToString:@"sendInput"]) {
        NSData *data = [message.body dataUsingEncoding:NSUTF8StringEncoding];
        [self sendInput:data];
    } else if ([message.name isEqualToString:@"sendBinary"]) {
        // bytes, one to a character (xterm.js's onBinary: old mouse reports)
        NSData *data = [message.body dataUsingEncoding:NSISOLatin1StringEncoding];
        [self sendInput:data];
    } else if ([message.name isEqualToString:@"copyText"]) {
        // a program's copy (OSC 52)
        UIPasteboard.generalPasteboard.string = message.body;
    } else if ([message.name isEqualToString:@"resize"]) {
        [self syncWindowSize];
    } else if ([message.name isEqualToString:@"propUpdate"]) {
        [self setValue:message.body[1] forKey:message.body[0]];
    } else if ([message.name isEqualToString:@"setTitle"]) {
        _windowTitle = message.body;
        _windowTitleGroup = self.foregroundProcessGroup;
        [NSNotificationCenter.defaultCenter postNotificationName:TerminalTitleDidChangeNotification object:self];
    } else if ([message.name isEqualToString:@"programStatus"]) {
        [self programStatusReported:message.body];
    } else if ([message.name isEqualToString:@"notify"]) {
        // [title, text]: the bell's both empty; iTerm2's, no title. The tab
        // shows its own title for it, which says where it came from.
        NSArray<NSString *> *notice = message.body;
        _attentionText = notice[1];
        _attentionCount++;
        [NSNotificationCenter.defaultCenter postNotificationName:TerminalAttentionNotification object:self];
    }
}

// A program status report (OSC 7501, its text after "7501;"), as Kimi Code's
// were recorded (2026-11-28, PI_PROGRAM_STATUS=1): state=idle:app=kimi-code,
// state=working:app=kimi-code:msg=<base64 of its session's title>, the same
// with state=done, and state=clear (as it starts and as it exits); and as
// Claude Code 2.1.332's were, iSH answering its query (2026-11-28, 851, in
// the kernel log): state=idle:app=claude-code, state=working:app=claude-code,
// the same with msg=<base64 of what it does> (a tool call's description, or
// "1 shell still running": it says it works while a background task of its
// runs, its turn over), and state=done:app=claude-code as its turn ends. Pairs of key=value by ':'. Those of the program as a
// whole have no id=; those of one of its tasks do, Claude Code's background
// shells' (2026-11-28): state=working:app=claude-code:id=<the task's
// id>:title=<base64 of its description> as one starts, state=clear:id=<the
// task's id> as it ends, taken for nothing, the program's own telling of
// what it does. One with a key, a state or an app not recorded is said in
// the kernel log, whole, and taken for nothing: what it would mean is not
// known.
- (void)programStatusReported:(NSString *)report {
    NSMutableDictionary<NSString *, NSString *> *pairs = [NSMutableDictionary new];
    for (NSString *pair in [report componentsSeparatedByString:@":"]) {
        // msg's base64 has '=' of its own
        NSRange equals = [pair rangeOfString:@"="];
        if (equals.location == NSNotFound) {
            printk("program status (OSC 7501): a pair not key=value (%s), taken for nothing: %s\n",
                   pair.UTF8String, report.UTF8String);
            return;
        }
        pairs[[pair substringToIndex:equals.location]] = [pair substringFromIndex:NSMaxRange(equals)];
    }
    static NSSet<NSString *> *keys, *taskStartKeys, *taskEndKeys;
    // Each app's states recorded; clear, alone, Kimi Code's only
    static NSDictionary<NSString *, NSSet<NSString *> *> *states;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        keys = [NSSet setWithArray:@[@"state", @"app", @"msg"]];
        taskStartKeys = [NSSet setWithArray:@[@"state", @"app", @"id", @"title"]];
        taskEndKeys = [NSSet setWithArray:@[@"state", @"id"]];
        states = @{
            @"kimi-code": [NSSet setWithArray:@[@"idle", @"working", @"done"]],
            @"claude-code": [NSSet setWithArray:@[@"idle", @"working", @"done"]],
        };
    });
    NSString *state = pairs[@"state"];
    BOOL clear = [state isEqualToString:@"clear"];
    NSSet<NSString *> *reportKeys = [NSSet setWithArray:pairs.allKeys];
    if (pairs[@"id"] != nil) {
        BOOL taskStarts = [state isEqualToString:@"working"] && [pairs[@"app"] isEqualToString:@"claude-code"] &&
            [reportKeys isEqualToSet:taskStartKeys];
        BOOL taskEnds = clear && [reportKeys isEqualToSet:taskEndKeys];
        if (!taskStarts && !taskEnds)
            printk("program status (OSC 7501) of a task, its keys, state or app not recorded, taken for nothing: %s\n",
                   report.UTF8String);
        return;
    }
    if (![reportKeys isSubsetOfSet:keys] ||
            (clear ? pairs.count != 1 : ![states[pairs[@"app"]] containsObject:state])) {
        printk("program status (OSC 7501) of a key, a state or an app not recorded, taken for nothing: %s\n",
               report.UTF8String);
        return;
    }
    _programStatus = clear ? nil : state;
    _programStatusGroup = self.foregroundProcessGroup;
    [NSNotificationCenter.defaultCenter postNotificationName:TerminalProgramStatusDidChangeNotification object:self];
}

- (void)syncWindowSize {
    [self.webView evaluateJavaScript:@"exports.getSize()" completionHandler:^(NSArray<NSNumber *> *dimensions, NSError *error) {
        int cols = dimensions[0].intValue;
        int rows = dimensions[1].intValue;
        if (self.tty == NULL)
            return;
        lock(&self.tty->lock);
        tty_set_winsize(self.tty, (struct winsize_) {.col = cols, .row = rows});
        unlock(&self.tty->lock);
    }];
}

- (void)setEnableVoiceOverAnnounce:(BOOL)enableVoiceOverAnnounce {
    _enableVoiceOverAnnounce = enableVoiceOverAnnounce;
    [self.webView evaluateJavaScript:[NSString stringWithFormat:@"exports.setAccessibilityEnabled(%@)",
                                      enableVoiceOverAnnounce ? @"true" : @"false"]
                   completionHandler:nil];
}

- (int)sendOutput:(const void *)buf length:(int)len {
    lock(&_dataLock);
    if (!NSThread.isMainThread) {
        // The main thread is the only one that can unblock this, so sleeping here would be a deadlock.
        // The only reason for this to be called on the main thread is if input is echoed.
        while (_pendingData.length > BUF_SIZE && !_destroyed) {
            // A terminal no view has shown has no page to take its output
            // (setTty:): one made for it now, or its writer waited for good
            if (!_pageAsked) {
                _pageAsked = YES;
                dispatch_async(dispatch_get_main_queue(), ^{
                    (void) self.webView;
                });
            }
            wait_for_ignore_signals(&_dataConsumed, &_dataLock, NULL);
        }
    }
    if (_destroyed) {
        // Nobody will ever render this; pretend it was written so the writer moves on to its EIO.
        unlock(&_dataLock);
        return len;
    }
    [_pendingData appendData:[NSData dataWithBytes:buf length:len]];
    [self.refreshTask schedule];
    unlock(&_dataLock);
    return len;
}


- (void)sendInput:(NSData *)input {
    if (self.tty == NULL)
        return;
    tty_input(self.tty, input.bytes, input.length, 0);
    [self.webView evaluateJavaScript:@"exports.setUserGesture()" completionHandler:nil];
    [self.scrollToBottomTask schedule];
}

- (void)pasteText:(NSString *)text {
    [self.webView callAsyncJavaScript:@"exports.paste(text)"
                            arguments:@{@"text": text}
                              inFrame:nil
                       inContentWorld:WKContentWorld.pageWorld
                    completionHandler:nil];
}

- (void)scrollToBottom {
    [self.webView evaluateJavaScript:@"exports.scrollToBottom()" completionHandler:nil];
}

- (NSString *)arrow:(char)direction {
    return [NSString stringWithFormat:@"\x1b%c%c", self.applicationCursor ? 'O' : '[', direction];
}

- (NSString *)arrow:(char)direction modifiers:(UIKeyModifierFlags)modifiers {
    int m = 1;
    if (modifiers & UIKeyModifierShift)     m += 1;
    if (modifiers & UIKeyModifierAlternate) m += 2;
    if (modifiers & UIKeyModifierControl)   m += 4;
    if (m == 1)
        return [self arrow:direction];
    return [NSString stringWithFormat:@"\x1b[1;%d%c", m, direction];
}

- (void)refresh {
    if (!self.loaded)
        return;

    lock(&_dataLock);
    if (_outputInProgress) {
        [self.refreshTask schedule];
        unlock(&_dataLock);
        return;
    }
    NSData *data = _pendingData;
    _pendingData = [[NSMutableData alloc] initWithCapacity:BUF_SIZE];
    _outputInProgress = YES;
    notify(&self->_dataConsumed);
    unlock(&_dataLock);

    if (data.length > 0)
        _outputCount++;
    // The bytes, one to a character, as an argument: no escaping. The page
    // answers once xterm.js has parsed them, and only then is more sent.
    NSString *dataString = [[NSString alloc] initWithBytes:data.bytes length:data.length encoding:NSISOLatin1StringEncoding];
    [self.webView callAsyncJavaScript:@"return exports.write(data)"
                            arguments:@{@"data": dataString}
                              inFrame:nil
                       inContentWorld:WKContentWorld.pageWorld
                    completionHandler:^(id result, NSError *error) {
        lock(&self->_dataLock);
        self->_outputInProgress = NO;
        unlock(&self->_dataLock);
        if (error != nil) {
            NSLog(@"error sending bytes to the terminal: %@", error);
            return;
        }
    }];
}

+ (void)convertCommand:(NSArray<NSString *> *)command toArgs:(char *)argv limitSize:(size_t)maxSize {
    char *p = argv;
    for (NSString *cmd in command) {
        const char *c = cmd.UTF8String;
        // Save space for the final NUL byte in argv
        while (p < argv + maxSize - 1 && (*p++ = *c++));
        // If we reach the end of the buffer, the last string still needs to be
        // NUL terminated
        *p = '\0';
    }
    // Add the final NUL byte to argv
    *++p = '\0';
}

+ (Terminal *)terminalWithType:(int)type number:(int)number {
    return [[Terminal alloc] initWithType:type number:number];
}

+ (Terminal *)terminalWithUUID:(NSUUID *)uuid {
    @synchronized (Terminal.class) {
        return [terminalsByUUID objectForKey:uuid];
    }
}

// self.tty is a non-owning pointer that ios_tty_cleanup clears when the tty is
// freed; cleanup runs under ttys_lock, so take our own reference under it.
- (struct tty *)retainTty {
    lock(&ttys_lock);
    struct tty *tty = self.tty;
    if (tty != NULL) {
        lock(&tty->lock);
        tty->refcount++;
        unlock(&tty->lock);
    }
    unlock(&ttys_lock);
    return tty;
}

- (void)releaseTty:(struct tty *)tty {
    lock(&ttys_lock);
    tty_release(tty);
    unlock(&ttys_lock);
}

- (int)foregroundProcessGroup {
    struct tty *tty = [self retainTty];
    if (tty == NULL)
        return 0;
    lock(&tty->lock);
    pid_t_ fg_group = tty->fg_group;
    unlock(&tty->lock);
    [self releaseTty:tty];
    return fg_group;
}

- (int)sessionId {
    struct tty *tty = [self retainTty];
    if (tty == NULL)
        return 0;
    lock(&tty->lock);
    pid_t_ session = tty->session;
    unlock(&tty->lock);
    [self releaseTty:tty];
    return session;
}

- (void)sendSignalToForegroundJob:(int)signal {
    int fg_group = self.foregroundProcessGroup;
    if (fg_group != 0)
        send_group_signal(fg_group, signal, SIGINFO_NIL);
}

- (void)hangup {
    @synchronized (self) {
        if (_hungUp)
            return;
        _hungUp = YES;
    }
    struct tty *tty = [self retainTty];
    if (tty != NULL) {
        lock(&tty->lock);
        tty_hangup(tty);
        pid_t_ fg_group = tty->fg_group;
        unlock(&tty->lock);
        // Like Linux, tell the foreground job the terminal went away, or a process
        // blocked somewhere other than a tty read would never notice. Sent after
        // dropping tty->lock: send_group_signal takes pids_lock, and tty_open takes
        // the two in the opposite order.
        if (fg_group != 0) {
            send_group_signal(fg_group, SIGHUP_, SIGINFO_NIL);
            send_group_signal(fg_group, SIGCONT_, SIGINFO_NIL);
        }
        [self releaseTty:tty];
    }
    @synchronized (Terminal.class) {
        if ([terminals objectForKey:self.terminalsKey] == self)
            [terminals removeObjectForKey:self.terminalsKey];
    }
}

- (void)destroy {
    [self hangup];
    // Drop unrendered output and wake a writer blocked in sendOutput; its next
    // write fails with EIO now that the tty is hung up.
    lock(&_dataLock);
    _destroyed = YES;
    _pendingData.length = 0;
    notify(&_dataConsumed);
    unlock(&_dataLock);
}

- (void)dealloc {
    // The last reference can be dropped from the emulator thread (ios_tty_cleanup),
    // but WebKit and the timers are main-thread only. Ownership of those objects is
    // moved out of the ivars and into the block, so their final release happens
    // wherever the block is destroyed: on the main thread.
    DelayedUITask *refreshTask = _refreshTask, *scrollToBottomTask = _scrollToBottomTask;
    WKWebView *webView = _webView;
    _refreshTask = _scrollToBottomTask = nil;
    _webView = nil;
    NSLog(@"Terminal %@ deallocated", self.uuid);
    dispatch_block_t cleanup = ^{
        [refreshTask cancel];
        [scrollToBottomTask cancel];
        if (webView != nil) {
            WKUserContentController *controller = webView.configuration.userContentController;
            for (size_t i = 0; i < sizeof(TERMINAL_HANDLERS)/sizeof(TERMINAL_HANDLERS[0]); i++)
                [controller removeScriptMessageHandlerForName:TERMINAL_HANDLERS[i]];
        }
    };
    if (NSThread.isMainThread)
        cleanup();
    else
        dispatch_async(dispatch_get_main_queue(), cleanup);
}

+ (void)initialize {
    if (self == Terminal.class) {
        terminals = [NSMapTable strongToWeakObjectsMapTable];
        terminalsByUUID = [NSMapTable strongToWeakObjectsMapTable];
    }
}

@end


static int ios_tty_init(struct tty *tty) {
    // This is called with ttys_lock but that results in deadlock since the main thread can also acquire ttys_lock. So release it.
    unlock(&ttys_lock);
    void (^init_block)(void) = ^{
        Terminal *terminal = [Terminal terminalWithType:tty->type number:tty->num];
        tty->data = (void *) CFBridgingRetain(terminal);
        terminal.tty = tty;
    };
    if ([NSThread isMainThread])
        init_block();
    else
        dispatch_sync(dispatch_get_main_queue(), init_block);

    lock(&ttys_lock);
    return 0;
}

static int ios_tty_write(struct tty *tty, const void *buf, size_t len, bool blocking) {
    Terminal *terminal = (__bridge Terminal *) tty->data;
    return [terminal sendOutput:buf length:(int) len];
}

static void ios_tty_cleanup(struct tty *tty) {
    Terminal *terminal = CFBridgingRelease(tty->data);
    tty->data = NULL;
    terminal.tty = NULL;
}

struct tty_driver_ops ios_tty_ops = {
    .init = ios_tty_init,
    .write = ios_tty_write,
    .cleanup = ios_tty_cleanup,
};
DEFINE_TTY_DRIVER(ios_console_driver, &ios_tty_ops, TTY_CONSOLE_MAJOR, 64);
struct tty_driver ios_pty_driver = {.ops = &ios_tty_ops};
