//
//  TerminalSession.m
//  iSH
//

#import <UserNotifications/UserNotifications.h>
#import "TerminalSession.h"
#import "AppDelegate.h"
#import "UserPreferences.h"
#include <stdatomic.h>
#include "kernel/init.h"
#include "kernel/task.h"
#include "kernel/calls.h"
#include "fs/devices.h"
#include "fs/path.h"
#include "debug.h"

NSNotificationName const TerminalSessionDidChangeNotification = @"TerminalSessionDidChangeNotification";
NSString *const TerminalSessionNotificationUUIDKey = @"session";

@interface TerminalSession ()
@property Terminal *terminal;
@property int pid;
@property TerminalSessionState state;
@property int exitCode;
@property NSUInteger number;
@property NSDate *startDate;
@property (nullable) NSString *shownTitle; // displayTitle as last announced
@property TerminalSessionActivity shownActivity; // activity as last announced
@property TerminalSessionAgent shownAgent; // agent as last announced
@property BOOL agentStoppedUnseen;
// What of it is seen: its output and its programs' bells and notifications
// so far, and whether a job ended since
@property NSUInteger seenOutputCount;
@property NSUInteger seenAttentionCount;
@property BOOL finishedUnseen;
// A job owned the terminal when last checked
@property BOOL jobRan;
- (void)refreshState;
- (void)attentionAsked;
@end

@implementation TerminalSession

- (NSUUID *)uuid {
    return self.terminal.uuid;
}

- (NSString *)displayTitle {
    if (self.title.length > 0)
        return self.title;
    NSString *running = self.runningTitle;
    if (running != nil)
        return running;
    return [NSString stringWithFormat:@"Shell %lu", (unsigned long) self.number];
}

// Claude Code's titles' glyphs (TerminalSessionAgent), each with the space
// after it
static NSDictionary<NSString *, NSNumber *> *ClaudeTitleGlyphs(void) {
    static NSDictionary<NSString *, NSNumber *> *glyphs;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        glyphs = @{
            @"✳ ": @(TerminalSessionAgentWaiting),
            @"◐ ": @(TerminalSessionAgentWorking),
            @"◑ ": @(TerminalSessionAgentWorking),
        };
    });
    return glyphs;
}

// The title the job owning the terminal set, nil if none: the agent's state
// told by its glyph, which *rest leaves off. Claude Code's begins with a
// glyph of those recorded; one it begins otherwise is said in the kernel log
// (the first of each), and is shown as it is, telling no state.
- (nullable NSString *)jobTitleWithAgent:(TerminalSessionAgent *)agent {
    *agent = TerminalSessionAgentNone;
    if (self.state != TerminalSessionStateRunning)
        return nil;
    int fg_group = self.terminal.foregroundProcessGroup;
    NSString *title = self.terminal.windowTitle;
    if (title.length == 0 || self.terminal.windowTitleGroup != fg_group)
        return nil;
    if (![self.foregroundProgram isEqualToString:@"claude"])
        return title;
    for (NSString *glyph in ClaudeTitleGlyphs()) {
        if ([title hasPrefix:glyph]) {
            *agent = ClaudeTitleGlyphs()[glyph].integerValue;
            return [title substringFromIndex:glyph.length];
        }
    }
    static NSMutableSet<NSString *> *unknown;
    NSString *first = [title substringWithRange:[title rangeOfComposedCharacterSequenceAtIndex:0]];
    if (unknown == nil)
        unknown = [NSMutableSet new];
    if (![unknown containsObject:first]) {
        [unknown addObject:first];
        NSData *bytes = [title dataUsingEncoding:NSUTF8StringEncoding];
        NSMutableString *hex = [NSMutableString new];
        for (NSUInteger i = 0; i < MIN(bytes.length, 16); i++)
            [hex appendFormat:@"%02x", ((const uint8_t *) bytes.bytes)[i]];
        printk("claude's title begins with none of its glyphs known (✳ ◐ ◑), shown as it is: %s (%s)\n",
               title.UTF8String, hex.UTF8String);
    }
    return title;
}

- (TerminalSessionAgent)agent {
    TerminalSessionAgent agent;
    [self jobTitleWithAgent:&agent];
    return agent;
}

// What runs in the terminal, and where: the title the job owning it set,
// else, while a job rather than the shell owns it, the job's program and
// the directory, else the directory. A title set by a job that has since
// given the terminal back is that job's, not the shell's.
- (nullable NSString *)runningTitle {
    if (self.state != TerminalSessionStateRunning)
        return nil;
    TerminalSessionAgent agent;
    NSString *jobTitle = [self jobTitleWithAgent:&agent];
    if (jobTitle != nil)
        return jobTitle;
    NSString *program = self.foregroundProgram;
    NSString *place = self.placeName;
    if (program != nil && place != nil)
        return [NSString stringWithFormat:@"%@ · %@", program, place];
    return program ?: place;
}

- (nullable NSString *)foregroundProgram {
    if (self.state != TerminalSessionStateRunning)
        return nil;
    int fg_group = self.terminal.foregroundProcessGroup;
    if (fg_group == 0 || fg_group == self.terminal.sessionId)
        return nil;
    // The group's leader, else whichever of it is left (a pipeline's)
    lock(&pids_lock);
    struct task *leader = pid_get_task(fg_group);
    NSString *program = leader != NULL && !leader->zombie ? [NSString stringWithUTF8String:leader->comm] : nil;
    unlock(&pids_lock);
    return program ?: self.foregroundJobPrograms.firstObject;
}

// The working directory as a tab shows it: ~ for root's home, else its last
// component
- (nullable NSString *)placeName {
    NSString *directory = self.currentDirectory;
    if (directory == nil)
        return nil;
    if ([directory isEqualToString:@"/root"])
        return @"~";
    if ([directory isEqualToString:@"/"])
        return directory;
    return directory.lastPathComponent;
}

- (TerminalSessionActivity)activity {
    if (self.state != TerminalSessionStateRunning)
        return TerminalSessionActivityNone;
    if (self.terminal.attentionCount != self.seenAttentionCount)
        return TerminalSessionActivityBell;
    if (self.finishedUnseen)
        return TerminalSessionActivityFinished;
    if (self.terminal.outputCount != self.seenOutputCount)
        return TerminalSessionActivityOutput;
    return TerminalSessionActivityNone;
}

- (void)markSeen {
    self.seenOutputCount = self.terminal.outputCount;
    self.seenAttentionCount = self.terminal.attentionCount;
    self.finishedUnseen = NO;
    self.agentStoppedUnseen = NO;
    [self announceChanges];
}

// Checks what runs (TerminalSessionStore): a job that owned the terminal and
// no longer does has ended. Seen once a second, a job shorter than that may
// go unnoticed: what ended at once needs no word.
- (void)refreshState {
    if (self.state != TerminalSessionStateRunning)
        return;
    BOOL jobRuns = self.foregroundProgram != nil;
    if (self.jobRan && !jobRuns)
        self.finishedUnseen = YES;
    self.jobRan = jobRuns;
    [self announceChanges];
}

// Announces displayTitle or activity if either changed. Only while it runs: a
// session that exited announced that already, and another change of one
// would read as its exit again (a tab kept for a shell that died at once
// would close).
- (void)announceChanges {
    if (self.state != TerminalSessionStateRunning)
        return;
    NSString *title = self.displayTitle;
    TerminalSessionActivity activity = self.activity;
    TerminalSessionAgent agent = self.agent;
    if (agent == TerminalSessionAgentWorking)
        self.agentStoppedUnseen = NO;
    else if (self.shownAgent == TerminalSessionAgentWorking)
        self.agentStoppedUnseen = YES;
    if ([title isEqualToString:self.shownTitle] && activity == self.shownActivity && agent == self.shownAgent)
        return;
    self.shownTitle = title;
    self.shownActivity = activity;
    self.shownAgent = agent;
    [self notifyChanged];
}

// A program asked to be told of: with the app away (in the background, a
// keep-alive's, or behind Control Center), a notification of the system's,
// the tab's title over what it said ("Bell" for the bell). A session's next
// takes the place of its last. Tapped, it shows the tab (AppDelegate).
- (void)attentionAsked {
    [self refreshState];
    if (UIApplication.sharedApplication.applicationState == UIApplicationStateActive)
        return;
    UNMutableNotificationContent *content = [UNMutableNotificationContent new];
    content.title = self.displayTitle;
    content.body = self.terminal.attentionText.length > 0 ? self.terminal.attentionText : @"Bell";
    content.sound = UNNotificationSound.defaultSound;
    content.threadIdentifier = self.uuid.UUIDString;
    content.userInfo = @{TerminalSessionNotificationUUIDKey: self.uuid.UUIDString};
    UNNotificationRequest *request = [UNNotificationRequest requestWithIdentifier:self.uuid.UUIDString content:content trigger:nil];
    [UNUserNotificationCenter.currentNotificationCenter addNotificationRequest:request withCompletionHandler:^(NSError *error) {
        if (error != nil)
            NSLog(@"notification of %@: %@", self.uuid, error);
    }];
}

- (void)setTitle:(NSString *)title {
    _title = [title copy];
    [self notifyChanged];
}

- (void)notifyChanged {
    NSAssert(NSThread.isMainThread, @"session changes are main thread only");
    [NSNotificationCenter.defaultCenter postNotificationName:TerminalSessionDidChangeNotification object:self];
}

- (NSString *)description {
    return [NSString stringWithFormat:@"<TerminalSession %@ pid=%d state=%ld>", self.displayTitle, self.pid, (long) self.state];
}

- (NSString *)currentDirectory {
    if (self.state != TerminalSessionStateRunning)
        return nil;
    // login forks the shell, so the process that owns the terminal (the shell, or
    // a job it started from the same directory) is the one to ask; the session's
    // own process is login, whose directory never changes.
    int fg_group = self.terminal.foregroundProcessGroup;
    NSString *directory = nil;
    lock(&pids_lock);
    struct task *task = pid_get_task(fg_group != 0 ? fg_group : self.pid);
    if (task != NULL && task->fs != NULL) {
        char path[MAX_PATH + 1];
        lock(&task->fs->lock);
        int err = generic_getpath(task->fs->pwd, path);
        unlock(&task->fs->lock);
        if (err >= 0)
            directory = [NSString stringWithUTF8String:path];
    }
    unlock(&pids_lock);
    return directory;
}

- (BOOL)shellIsForeground {
    if (self.state != TerminalSessionStateRunning)
        return NO;
    // The shell leads the session (login execs it there) and owns the terminal
    // while the session's process group is the foreground one: a job has a
    // group of its own, a script run by `sh` too. Until login has exec'd the
    // shell the leader is login, whose terminal setup would throw typed text
    // away: the leader's program tells.
    int fg_group = self.terminal.foregroundProcessGroup;
    if (fg_group == 0 || fg_group != self.terminal.sessionId)
        return NO;
    static NSSet<NSString *> *shells;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        shells = [NSSet setWithArray:@[@"sh", @"ash", @"bash", @"dash", @"zsh", @"fish", @"ksh", @"mksh", @"busybox"]];
    });
    lock(&pids_lock);
    struct task *task = pid_get_task(fg_group);
    NSString *comm = task != NULL ? [NSString stringWithUTF8String:task->comm] : nil;
    unlock(&pids_lock);
    return comm != nil && [shells containsObject:comm];
}

- (NSArray<NSString *> *)foregroundJobPrograms {
    if (self.state != TerminalSessionStateRunning)
        return @[];
    // The session's leader is the shell (login execs it), and its process
    // group is the session's: any other group in the foreground is a job.
    int fg_group = self.terminal.foregroundProcessGroup;
    int session = self.terminal.sessionId;
    if (fg_group == 0 || fg_group == session)
        return @[];
    NSMutableOrderedSet<NSString *> *programs = [NSMutableOrderedSet orderedSet];
    lock(&pids_lock);
    struct pid *pid = pid_get(fg_group);
    if (pid != NULL) {
        struct tgroup *group;
        list_for_each_entry(&pid->pgroup, group, pgroup) {
            if (!group->leader->zombie)
                [programs addObject:[NSString stringWithUTF8String:group->leader->comm]];
        }
    }
    unlock(&pids_lock);
    return programs.array;
}

@end

@interface TerminalSessionStore ()
@property NSMutableArray<TerminalSession *> *mutableSessions;
@property NSUInteger nextNumber;
// pids of every process started here that has not been reaped yet, including
// those of sessions already closed
@property NSMutableSet<NSNumber *> *livePids;
@end

@implementation TerminalSessionStore

+ (instancetype)shared {
    static TerminalSessionStore *store;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        store = [TerminalSessionStore new];
    });
    return store;
}

- (instancetype)init {
    if (self = [super init]) {
        self.mutableSessions = [NSMutableArray new];
        self.livePids = [NSMutableSet new];
        self.nextNumber = 1;
        // Tab titles follow what runs: a title a program sets at once, the job
        // that owns the terminal (top started, claude ended) within a second.
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(terminalTitleChanged:)
                                                   name:TerminalTitleDidChangeNotification
                                                 object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(terminalAskedAttention:)
                                                   name:TerminalAttentionNotification
                                                 object:nil];
        __weak typeof(self) weakSelf = self;
        NSTimer *titles = [NSTimer timerWithTimeInterval:1 repeats:YES block:^(NSTimer *_) {
            for (TerminalSession *session in [weakSelf.mutableSessions copy])
                [session refreshState];
        }];
        [NSRunLoop.mainRunLoop addTimer:titles forMode:NSRunLoopCommonModes];
        [NSNotificationCenter.defaultCenter addObserver:self
                                               selector:@selector(processExited:)
                                                   name:ProcessExitedNotification
                                                 object:nil];
    }
    return self;
}

- (NSArray<TerminalSession *> *)sessions {
    return [self.mutableSessions copy];
}

- (TerminalSession *)sessionWithUUID:(NSUUID *)uuid {
    for (TerminalSession *session in self.mutableSessions) {
        if ([session.uuid isEqual:uuid])
            return session;
    }
    return nil;
}

// As Linux starts the logins after the boot's sysinit actions, a session's
// process runs once the boot (/usr/libexec/ish-sysinit) is done: its login
// is then recorded with utmpd (w, who), which the boot starts. A login before
// it went unrecorded, the first session's after the app started.
//
// Waited for on the process's own thread before it runs (task->before_run),
// not the main thread: the boot can take seconds without the JIT. 30 seconds
// at most, for an init that does not run it (an /etc/inittab of one's own);
// not again after that.
static atomic_bool boot_waited;

static bool boot_is_awaited(void) {
    if (atomic_load(&boot_waited))
        return false;
    struct statbuf stat;
    // a root the app has not given it, or a boot that is not init's
    if (generic_statat(AT_PWD, "/usr/libexec/ish-sysinit", &stat, true) < 0 ||
            ![UserPreferences.shared.bootCommand.firstObject isEqualToString:@"/sbin/init"]) {
        atomic_store(&boot_waited, true);
        return false;
    }
    return true;
}

// task->before_run: current is the session's task, on its thread
static void wait_for_boot(void) {
    struct statbuf stat;
    for (int i = 0; i < 3000; i++) {
        if (atomic_load(&boot_waited) || generic_statat(AT_PWD, ISH_BOOT_DONE_PATH, &stat, true) == 0) {
            atomic_store(&boot_waited, true);
            return;
        }
        usleep(10000);
    }
    atomic_store(&boot_waited, true);
    NSLog(@"the boot did not say it was done in 30 seconds (%s): the sessions start without it", ISH_BOOT_DONE_PATH);
}

// The directory a shell on the pts goes back to (/etc/profile.d/ish-session.sh):
// written for a restored tab; one an earlier session there left removed, so
// that no other shell takes it. Returns a negative errno.
static int set_start_directory(int pts, NSString *directory) {
    char path[64];
    snprintf(path, sizeof(path), "/ish/session/pts-%d", pts);
    generic_unlinkat(AT_PWD, path);
    if (directory == nil)
        return 0;
    generic_mkdirat(AT_PWD, "/ish", 0755);
    generic_mkdirat(AT_PWD, "/ish/session", 0755);
    struct fd *fd = generic_open(path, O_WRONLY_|O_CREAT_|O_TRUNC_, 0644);
    if (IS_ERR(fd))
        return (int) PTR_ERR(fd);
    const char *dir = directory.fileSystemRepresentation;
    ssize_t written = fd->ops->write(fd, dir, strlen(dir));
    fd_close(fd);
    return written < 0 ? (int) written : 0;
}

// Creates the pty and the process, leaving it ready for task_start. Returns a negative errno.
static int start_process(NSArray<NSString *> *command, NSString *directory, Terminal **terminalOut) {
    int err = become_new_init_child();
    if (err < 0)
        return err;
    struct tty *tty;
    Terminal *terminal = [Terminal createPseudoTerminal:&tty];
    if (terminal == nil) {
        NSCAssert(IS_ERR(tty), @"tty should be error");
        return (int) PTR_ERR(tty);
    }
    *terminalOut = terminal;
    NSString *stdioFile = [NSString stringWithFormat:@"/dev/pts/%d", tty->num];
    err = create_stdio(stdioFile.fileSystemRepresentation, TTY_PSEUDO_SLAVE_MAJOR, tty->num);
    if (err < 0)
        return err;
    err = set_start_directory(tty->num, directory);
    if (err < 0)
        return err;
    tty_release(tty);

    char argv[4096];
    [Terminal convertCommand:command toArgs:argv limitSize:sizeof(argv)];
    const char *envp = "TERM=xterm-256color\0";
    err = do_execve(command[0].UTF8String, command.count, argv, envp);
    if (err < 0)
        return err;
    return current->pid;
}

- (TerminalSession *)startSessionWithError:(int *)errorOut {
    return [self startSessionInDirectory:nil error:errorOut];
}

- (TerminalSession *)startSessionInDirectory:(NSString *)directory error:(int *)errorOut {
    NSAssert(NSThread.isMainThread, @"sessions are started from the main thread");
    Terminal *terminal = nil;
    int pid = start_process(UserPreferences.shared.launchCommand, directory, &terminal);
    if (pid < 0) {
        if (terminal != nil)
            [terminal destroy];
        if (errorOut != NULL)
            *errorOut = pid;
        return nil;
    }

    TerminalSession *session = [TerminalSession new];
    session.terminal = terminal;
    session.pid = pid;
    session.state = TerminalSessionStateRunning;
    session.number = self.nextNumber++;
    session.startDate = [NSDate date];
    // Register before the process can run, so an immediately exiting command is still matched.
    [self.mutableSessions addObject:session];
    [self.livePids addObject:@(pid)];
    if (boot_is_awaited())
        current->before_run = wait_for_boot;
    int err = task_start(current);
    if (err < 0) {
        // No thread for it (the host out of them): it exits as one that never
        // ran (127, as a shell's command that could not run), is reaped here,
        // as processExited: does, and its terminal goes. No session, the error
        // said, rather than a terminal whose process never runs.
        exit_unstarted(127 << 8);
        reap_init_child(pid);
        [self.mutableSessions removeObject:session];
        [self.livePids removeObject:@(pid)];
        [terminal destroy];
        if (errorOut != NULL)
            *errorOut = err;
        return nil;
    }
    // The task now runs on its own thread. If main kept pointing at it, signals sent
    // from main (e.g. SIGHUP on close) would take the "signalling myself" path and
    // not wake the task's thread.
    current = NULL;
    return session;
}

- (void)terminalTitleChanged:(NSNotification *)notification {
    for (TerminalSession *session in [self.mutableSessions copy]) {
        if (session.terminal == notification.object)
            [session refreshState];
    }
}

- (void)terminalAskedAttention:(NSNotification *)notification {
    for (TerminalSession *session in [self.mutableSessions copy]) {
        if (session.terminal == notification.object)
            [session attentionAsked];
    }
}

- (void)closeSession:(TerminalSession *)session {
    NSAssert(NSThread.isMainThread, @"sessions are closed from the main thread");
    if (session.state == TerminalSessionStateClosed)
        return;
    [session.terminal destroy];
    session.state = TerminalSessionStateClosed;
    [self.mutableSessions removeObject:session];
    [session notifyChanged];
}

// The foreground jobs get SIGHUP, as when a tab is closed: Claude Code, for
// one, shuts down (its writes drained, its SessionEnd hooks run) rather than
// dying with the app as with kill -9. destroy, not hangup: a writer blocked
// till the page takes its output, which the main thread, held here, would
// never let it, gets EIO.
//
// Waited for: each session's process, its shell, which goes once its
// foreground job has and it reads the terminal, hung up. Polled in the process
// table, not told by processExited:, which comes on the main thread. Nothing
// reaps them, nor changes the sessions: the app is gone a moment after, and
// the tabs it saved as it went to the background come back at the next launch.
- (void)hangUpForTerminationWaiting:(NSTimeInterval)timeout {
    NSAssert(NSThread.isMainThread, @"sessions are hung up from the main thread");
    NSMutableArray<NSNumber *> *pids = [NSMutableArray new];
    for (TerminalSession *session in self.mutableSessions) {
        if (session.state != TerminalSessionStateRunning)
            continue;
        [session.terminal destroy];
        [pids addObject:@(session.pid)];
    }
    NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:timeout];
    while (pids.count > 0 && deadline.timeIntervalSinceNow > 0) {
        usleep(20 * 1000);
        lock(&pids_lock);
        NSIndexSet *exited = [pids indexesOfObjectsPassingTest:^BOOL(NSNumber *pid, NSUInteger index, BOOL *stop) {
            return pid_get_task(pid.intValue) == NULL; // a zombie, or gone
        }];
        unlock(&pids_lock);
        [pids removeObjectsAtIndexes:exited];
    }
}

- (void)processExited:(NSNotification *)notif {
    int pid = [notif.userInfo[@"pid"] intValue];
    int code = [notif.userInfo[@"code"] intValue];
    if (![self.livePids containsObject:@(pid)])
        return; // not ours (e.g. something init spawned itself)
    [self.livePids removeObject:@(pid)];
    // Sessions are children of init, and init does not wait for them. The zombie holds
    // a reference to its controlling terminal, so it must be reaped here (whether or not
    // its tab is still open) or the pty, and with it the Terminal and its web view,
    // live forever.
    reap_init_child(pid);
    for (TerminalSession *session in self.mutableSessions) {
        if (session.pid == pid && session.state == TerminalSessionStateRunning) {
            session.exitCode = code;
            session.state = TerminalSessionStateExited;
            // Not destroy: the tab stays visible, so output still in flight must be rendered.
            [session.terminal hangup];
            [session notifyChanged];
            return;
        }
    }
}

@end
