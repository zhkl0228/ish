//
//  TerminalSession.h
//  iSH
//
//  A shell process plus the Terminal it is attached to. Sessions live in the
//  TerminalSessionStore independently of any view or window showing them.
//

#import <Foundation/Foundation.h>
#import "Terminal.h"

NS_ASSUME_NONNULL_BEGIN

// The boot (/usr/libexec/ish-sysinit, init's first sysinit action) writes
// this file when it is done, and the app removes it before it boots: the
// sessions' processes run once it is there (TerminalSession.m)
#define ISH_BOOT_DONE_PATH "/run/ish-sysinit.done"

// What a session did since its tab was last looked at (markSeen)
typedef NS_ENUM(NSInteger, TerminalSessionActivity) {
    TerminalSessionActivityNone,
    TerminalSessionActivityOutput,   // it wrote something
    TerminalSessionActivityFinished, // the job that owned its terminal ended
    TerminalSessionActivityBell,     // a program rang the bell or sent a notification
};

typedef NS_ENUM(NSInteger, TerminalSessionState) {
    TerminalSessionStateRunning,
    TerminalSessionStateExited, // the shell exited; the terminal still shows its last output
    TerminalSessionStateClosed, // removed from the store
};

@interface TerminalSession : NSObject

@property (readonly) NSUUID *uuid; // same as terminal.uuid
@property (readonly) Terminal *terminal;
@property (readonly) int pid;
@property (readonly) TerminalSessionState state;
@property (readonly) int exitCode; // valid once state is Exited
@property (readonly) NSUInteger number; // 1-based, in order of creation, for the default title
@property (readonly) NSDate *startDate;

// Set by the user (tab rename). nil means use the default title.
@property (nonatomic, copy, nullable) NSString *title;
// The tab's title: the user's, else the title the job owning the terminal set
// (OSC 0 or 2: claude, vim), else that job's program and the directory (top ·
// ~), else the directory while the shell owns the terminal (~ for root's
// home, else its last component), else "Shell <number>". It follows the
// terminal: TerminalSessionStore checks it every second and on each title a
// program sets, and a change posts TerminalSessionDidChangeNotification.
@property (readonly) NSString *displayTitle;
// What it did since its tab was last looked at: a program asked to be told
// of (the bell, a notification: claude waiting for an answer), else a job
// that owned the terminal for a second or more ended, else it wrote
// something. Checked with the title, a change posted the same way; the first
// at once, and with the app away, a notification of the system's too. None
// once it is not running.
@property (readonly) TerminalSessionActivity activity;
// Its tab is looked at: what it did so far is seen
- (void)markSeen;
// The program of the job that owns the terminal (top, claude), nil while the
// shell does
@property (readonly, nullable) NSString *foregroundProgram;

// The shell's working directory, or nil if it cannot be determined (e.g. after
// the shell exited).
@property (readonly, nullable) NSString *currentDirectory;
// Whether a shell, rather than a job like vim or ssh, owns the terminal: typed
// text goes to a prompt.
@property (readonly) BOOL shellIsForeground;
// The programs of the job that owns the terminal when the shell does not
// (claude, top, a script): what closing the tab ends, as the hangup goes to
// them. Empty while the shell owns it.
@property (readonly) NSArray<NSString *> *foregroundJobPrograms;

@end

// Posted on the main thread with the session as the object whenever its state
// or title changes.
extern NSNotificationName const TerminalSessionDidChangeNotification;
// In the userInfo of a session's notification of the system's (a program
// asked to be told of with the app away): the session's uuid, a string
extern NSString *const TerminalSessionNotificationUUIDKey;

@interface TerminalSessionStore : NSObject

+ (instancetype)shared;

// Every session that has not been closed, in creation order.
@property (readonly) NSArray<TerminalSession *> *sessions;

// Starts the configured launch command in a new pseudo terminal. Returns nil
// and sets *error (a negative errno) on failure. Main thread only.
- (nullable TerminalSession *)startSessionWithError:(int *_Nullable)error;
// The same, for a tab restored when the app starts again: its shell goes back
// to directory (nil: home, as any shell), through /ish/session/<pts>, which
// /etc/profile.d/ish-session.sh reads; and the terminal shows output (nil:
// nothing) before anything the shell writes.
- (nullable TerminalSession *)startSessionInDirectory:(nullable NSString *)directory
                                        restoredOutput:(nullable NSData *)output
                                                 error:(int *_Nullable)error;

- (nullable TerminalSession *)sessionWithUUID:(NSUUID *)uuid;

// Hangs up the terminal (the foreground job gets SIGHUP) and forgets the
// session. Idempotent.
- (void)closeSession:(TerminalSession *)session;

// The app being terminated (applicationWillTerminate:): every running
// session's terminal hung up, as a closed tab's is, and the sessions'
// processes given till timeout (seconds) to exit. Main thread, which it holds
// while it waits.
- (void)hangUpForTerminationWaiting:(NSTimeInterval)timeout;

@end

NS_ASSUME_NONNULL_END
