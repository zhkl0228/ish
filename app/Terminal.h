//
//  Terminal.h
//  iSH
//
//  Created by Theodore Dubois on 10/18/17.
//

#import <UIKit/UIKit.h>
#import <WebKit/WebKit.h>

struct tty;

// WKUserContentController retains its message handlers, so anything that owns
// the web view must register through this to avoid a retain cycle.
@interface WeakScriptMessageHandler : NSObject <WKScriptMessageHandler>
- (instancetype)initWithHandler:(id <WKScriptMessageHandler>)handler;
@end

@interface Terminal : NSObject

+ (Terminal *)terminalWithType:(int)type number:(int)number;
// Returns a strong struct tty and a Terminal that has a weak reference to the same tty
+ (Terminal *)createPseudoTerminal:(struct tty **)tty;

+ (Terminal *)terminalWithUUID:(NSUUID *)uuid;
@property (readonly) NSUUID *uuid;

+ (void)convertCommand:(NSArray<NSString *> *)command toArgs:(char *)argv limitSize:(size_t)maxSize;

- (int)sendOutput:(const void *)buf length:(int)len;
- (void)sendInput:(NSData *)input;
// A paste: to the input, between the markers of a bracketed paste if the
// program asked for them, its newlines returns (xterm.js's term.paste)
- (void)pasteText:(NSString *)text;

- (NSString *)arrow:(char)direction;
// With modifiers, xterm's CSI 1;m form (2=Shift, 3=Alt, 5=Ctrl, ...).
- (NSString *)arrow:(char)direction modifiers:(UIKeyModifierFlags)modifiers;

// Make this terminal no longer be the singleton terminal with its type and number. Will happen eventually if all references go away, but sometimes you want it to happen now.
// Hangs up the tty: reads fail, the foreground job gets SIGHUP, and the
// terminal is no longer found by device number (so a new pty reusing the
// number gets a fresh Terminal). Output already buffered is still rendered.
// Safe to call more than once, and after the process has gone.
- (void)hangup;
// hangup, plus: drop any output still waiting to be rendered and unblock a
// writer waiting for it. For terminals nobody will look at again.
- (void)destroy;

// The process group that owns the terminal right now (0 if none), and a way to
// signal it, e.g. to kill a job that ignores Ctrl+C.
@property (readonly) int foregroundProcessGroup;
- (void)sendSignalToForegroundJob:(int)signal;
// The session the terminal is the controlling terminal of: its leader's pid,
// and its process group while the shell, not a job, owns the terminal. 0 if
// none.
@property (readonly) int sessionId;
// The title a program set (OSC 0 or 2), and the process group that owned the
// terminal then: the title is that job's (TerminalSession.displayTitle).
// Each change posts TerminalTitleDidChangeNotification.
@property (readonly) NSString *windowTitle; // nil until a program sets one
@property (readonly) int windowTitleGroup;
// What the program owning the terminal says it does (Kimi Code's program
// status, OSC 7501: term.js): working, idle, done (and not yet seen, which it
// says itself, idle again once it thinks it was), nil before any report and
// after clear; and the process group that owned the terminal then, as with
// the title. Each change posts TerminalProgramStatusDidChangeNotification.
@property (readonly) NSString *programStatus;
@property (readonly) int programStatusGroup;
// Output rendered so far, counted in the pieces it was rendered in: a tab
// tells from it that something was written while it was not looked at.
// Main thread.
@property (readonly) NSUInteger outputCount;
// What a program asked to be told of (the bell, a notification: term.js),
// counted, and the text of the last ("" for the bell). Each posts
// TerminalAttentionNotification. Main thread.
@property (readonly) NSUInteger attentionCount;
@property (readonly) NSString *attentionText;

@property (readonly) WKWebView *webView;
@property (nonatomic) BOOL enableVoiceOverAnnounce;
// Use KVO on this
@property (readonly) BOOL loaded;

@end

extern NSNotificationName const TerminalTitleDidChangeNotification;
extern NSNotificationName const TerminalAttentionNotification;
extern NSNotificationName const TerminalProgramStatusDidChangeNotification;

extern struct tty_driver ios_console_driver;
