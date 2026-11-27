//
//  AppDelegate.m
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#include <resolv.h>
#include <arpa/inet.h>
#include <netdb.h>
#import <Network/Network.h>
#import <UserNotifications/UserNotifications.h>
#import "AboutViewController.h"
#import "AppDelegate.h"
#import "TerminalSession.h"
#import "AppGroup.h"
#import "CurrentRoot.h"
#import "ExceptionExfiltrator.h"
#import "iOSFS.h"
#import "SceneDelegate.h"
#import "PasteboardDevice.h"
#import "LocationDevice.h"
#import "MainMenu.h"
#import "ClipboardStore.h"
#import "BackgroundKeepAlive.h"
#import "BuiltinJIT.h"
#import "OnDevicePairing.h"
#import "NSObject+SaneKVO.h"
#import "Roots.h"
#import "TerminalViewController.h"
#import "UIApplication+OpenURL.h"
#import "Screenshot.h"
#import "UIActions.h"
#import "DeviceStatus.h"
#import "UserPreferences.h"
#include "kernel/init.h"
#include "kernel/calls.h"
#include "kernel/mm.h"     // [T-ish-footprint-brake] memory governor feed
#import <mach/mach.h>       // task_info / TASK_VM_INFO (phys_footprint)
#import <os/proc.h>         // os_proc_available_memory
#include "fs/dyndev.h"
#include "fs/devices.h"
#include "fs/path.h"
#include "fs/proc/ish.h"
#include "fs/real.h"
#include "kernel/errno.h"
#include "kernel/fs.h"
#include "asbestos/guest-arm64/host_fault.h"
#if defined(DEBUG)
#include "DebugServer.h"
#endif
#include "kernel/native_offload.h"
#ifdef ISH_FFMPEG_TEST
extern void native_builtins_init(void);
#endif


@interface AppDelegate () <UNUserNotificationCenterDelegate>

@property BOOL exiting;
@property nw_path_monitor_t pathMonitor;
// What configureDns last wrote into /etc/resolv.conf
@property NSString *resolvConf;

@end

// /proc/ish/thermal: ProcessInfo's thermal state, which may be read on any thread
static int thermal_state(void) {
    return (int) NSProcessInfo.processInfo.thermalState;
}

static void ios_handle_exit(struct task *task, int code) {
    // we are interested in init and in children of init
    // this is called with pids_lock as an implementation side effect, please do not cite as an example of good API design
    if (task->parent != NULL && task->parent->parent != NULL)
        return;
    // pid should be saved now since task would be freed
    pid_t pid = task->pid;
    dispatch_async(dispatch_get_main_queue(), ^{
        [[NSNotificationCenter defaultCenter] postNotificationName:ProcessExitedNotification
                                                            object:nil
                                                          userInfo:@{@"pid": @(pid),
                                                                     @"code": @(code)}];
    });
}

static void ios_handle_die(const char *msg) {
    NSString *message = [NSString stringWithFormat:@"%s: %s", __func__, msg];
    iSHExceptionHandler([[NSException alloc] initWithName:NSGenericException reason:message userInfo:nil]);
}

static int bootError;

// [T-ish-footprint-brake] Feed the kernel's memory governor the two numbers
// jetsam actually operates on: the app's physical footprint and its remaining
// allowance. `limit = phys_footprint + os_proc_available_memory()` is the live
// per-app jetsam line, so there is no hardcoded per-device table and no
// boot-time snapshot that goes stale.
//
// Without this feed ish_footprint_mode() stays false and admission falls back
// to the fixed ANON_MMAP_LIMIT_PAGES ledger, which is not device-derived --
// V8's ~5x128MB startup cage commit then hits that ceiling regardless of how
// much memory the device actually has. The kernel side owns the state machine
// (BRAKE below 10% headroom, release above 15%, critical pressure forces it,
// and a feed older than 2s fails closed); this just reports the truth.
static void ish_memory_governor_tick(bool pressureCritical) {
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    uint64_t footprint = 0;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        footprint = info.phys_footprint;
    uint64_t avail = (uint64_t) os_proc_available_memory();
    if (footprint == 0 || avail == 0)
        return; // couldn't measure -- the kernel's staleness rule handles a dead feed
    ish_set_memory_status(footprint + avail, avail, pressureCritical);
}

static void ish_memory_governor_start(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_UTILITY, 0);
        // 250ms cadence: guest dirtying is bounded by emulation speed, so the
        // per-tick overshoot stays small against a 10%-of-limit brake margin.
        static dispatch_source_t timer;
        timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, q);
        dispatch_source_set_timer(timer, DISPATCH_TIME_NOW,
                                  250 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
        dispatch_source_set_event_handler(timer, ^{ ish_memory_governor_tick(false); });
        dispatch_resume(timer);
        // OS pressure events arrive faster than any poll -- treat CRITICAL as
        // an immediate brake regardless of our own arithmetic.
        static dispatch_source_t pressure;
        pressure = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0,
                                          DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
                                          q);
        dispatch_source_set_event_handler(pressure, ^{
            bool critical = (dispatch_source_get_data(pressure) & DISPATCH_MEMORYPRESSURE_CRITICAL) != 0;
            ish_memory_governor_tick(critical);
        });
        dispatch_resume(pressure);
    });
    // Synchronous first feed so footprint mode is active before the first
    // guest process ever runs.
    ish_memory_governor_tick(false);
}

// [T-ish-anon-cap-page-units] Install the legacy ledger cap from the device's
// real allowance. This stays as the fallback regime for the case where the
// governor can never measure (footprint mode never activates); once the first
// governor feed lands this stops being the admission control and only keeps
// counting for meminfo/diagnostics.
//
// Divide by the HOST page size, not the guest 4KB one: the counter is in guest
// pages but each committed guest page occupies a whole host page, so
// converting a host-byte budget with 4096 authorises 4x what was intended.
static void ish_install_anon_cap(void) {
    size_t avail = os_proc_available_memory();
    size_t hostPage = (size_t) getpagesize();
    if (avail == 0 || hostPage == 0) {
        NSLog(@"iSH: os_proc_available_memory unavailable -- keeping default anon cap");
        return;
    }
    static const double kGuestMemoryShare = 0.8;
    long pages = (long)((double) avail * kGuestMemoryShare / (double) hostPage);
    ish_set_anon_page_limit(pages);
    extern _Atomic long anon_page_limit;
    long effective = atomic_load(&anon_page_limit);
    double capMB = (double) effective * (double) hostPage / (1024 * 1024);
    NSLog(@"iSH: guest anon cap = %.0f MB host (%ld guest pages, host page %zuKB, "
          @"share %.0f%%, available %.0f MB)",
          capMB, effective, hostPage / 1024, kGuestMemoryShare * 100.0,
          (double) avail / (1024 * 1024));
}

// The app's Documents (Finder's file sharing, Files' On My iPhone: the
// built-in JIT's pairing file in StikJIT/, the themes) in the home of root,
// whom the launch command logs in (login -f root): /root/Documents, as the
// directory is named. Its host path (/proc/ish/documents) changes as the app
// is installed again, so it is mounted at each boot; the directory is made
// the first time, in the root's /root, which an update keeps.
static void mount_documents(void) {
    NSURL *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                            inDomains:NSUserDomainMask].firstObject;
    int err = generic_mkdirat(AT_PWD, "/root/Documents", 0755);
    if (err < 0 && err != _EEXIST) {
        NSLog(@"iSH: the app's Documents not mounted: /root/Documents not made (error %d)", err);
        return;
    }
    err = do_mount(&realfs, documents.fileSystemRepresentation, "/root/Documents", "", 0);
    if (err < 0)
        NSLog(@"iSH: the app's Documents (%@) not mounted at /root/Documents: error %d", documents.path, err);
}

@implementation AppDelegate

- (int)boot {
    // A host fault in translated guest code (a stale TLB entry) is retried,
    // in native code at its access; any other crash still reaches the
    // system's handler and its crash report.
    asbestos_install_host_fault_handler();
    // Install the device-derived ledger cap, then start the live governor.
    // Both must happen before the first guest process runs.
    ish_install_anon_cap();
    ish_memory_governor_start();

    NSURL *root = [Roots.instance rootUrl:Roots.instance.defaultRoot];

    int err = mount_root(&fakefs, [root URLByAppendingPathComponent:@"data"].fileSystemRepresentation);
    if (err < 0)
        return err;

    fs_register(&iosfs);
    fs_register(&iosfs_unsafe);

    // need to do this first so that we can have a valid current for the generic_mknod calls
    err = become_first_process();
    if (err < 0)
        return err;

#ifdef ISH_FFMPEG_TEST
    // Register built-in native handlers (fake_ffmpeg) for the ffmpeg test target.
    // This is NOT called in the standard iSH ARM64 target.
    native_builtins_init();
#endif

    FsInitialize();

    create_some_device_nodes();
    
    // Permissions on / have been broken for a while, let's fix them
    generic_setattrat(AT_PWD, "/", (struct attr) {.type = attr_mode, .mode = 0755}, false);
    
    // Register clipboard device driver and create device node for it
    err = dyn_dev_register(&clipboard_dev, DEV_CHAR, DYN_DEV_MAJOR, DEV_CLIPBOARD_MINOR);
    if (err != 0) {
        return err;
    }
    generic_mknodat(AT_PWD, "/dev/clipboard", S_IFCHR|0666, dev_make(DYN_DEV_MAJOR, DEV_CLIPBOARD_MINOR));
    
    err = dyn_dev_register(&location_dev, DEV_CHAR, DYN_DEV_MAJOR, DEV_LOCATION_MINOR);
    if (err != 0)
        return err;
    generic_mknodat(AT_PWD, "/dev/location", S_IFCHR|0666, dev_make(DYN_DEV_MAJOR, DEV_LOCATION_MINOR));

    do_mount(&procfs, "proc", "/proc", "", 0);
    do_mount(&devptsfs, "devpts", "/dev/pts", "", 0);
    mount_documents();

    iosfs_init(); // let it mount any filesystems from user defaults

    [self configureDns];
    
    exit_hook = ios_handle_exit;
    die_handler = ios_handle_die;
#if !TARGET_OS_SIMULATOR
    NSString *sockTmp = [NSTemporaryDirectory() stringByAppendingString:@"ishsock"];
    sock_tmp_prefix = strdup(sockTmp.UTF8String);
#endif
    
    tty_drivers[TTY_CONSOLE_MAJOR] = &ios_console_driver;
    set_console_device(TTY_CONSOLE_MAJOR, 1);
    err = create_stdio("/dev/console", TTY_CONSOLE_MAJOR, 1);
    if (err < 0)
        return err;

#if defined(DEBUG)
    debug_server_start(1234);
#endif

    // The last boot's: the sessions' processes wait for this boot's
    // (TerminalSession.m)
    generic_unlinkat(AT_PWD, ISH_BOOT_DONE_PATH);

    NSArray<NSString *> *command;
    command = UserPreferences.shared.bootCommand;
    char argv[4096];
    [Terminal convertCommand:command toArgs:argv limitSize:sizeof(argv)];
    const char *envp =
        "TERM=xterm-256color\0"
        "HOME=/root\0"
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\0"
        ;
    err = do_execve(command[0].UTF8String, command.count, argv, envp);
    if (err < 0)
        return err;
    // no thread for it (the host out of them): the boot fails, said so,
    // rather than waiting on an init that never runs
    return task_start(current);
}


- (void)configureDns {
    struct __res_state res;
    if (EXIT_SUCCESS != res_ninit(&res)) {
        exit(2);
    }
    NSMutableString *resolvConf = [NSMutableString new];
    if (res.dnsrch[0] != NULL) {
        [resolvConf appendString:@"search"];
        for (int i = 0; res.dnsrch[i] != NULL; i++) {
            [resolvConf appendFormat:@" %s", res.dnsrch[i]];
        }
        [resolvConf appendString:@"\n"];
    }
    union res_sockaddr_union servers[NI_MAXSERV];
    int serversFound = res_getservers(&res, servers, NI_MAXSERV);
    char address[NI_MAXHOST];
    for (int i = 0; i < serversFound; i ++) {
        union res_sockaddr_union s = servers[i];
        if (s.sin.sin_len == 0)
            continue;
        getnameinfo((struct sockaddr *) &s.sin, s.sin.sin_len,
                    address, sizeof(address),
                    NULL, 0, NI_NUMERICHOST);
        [resolvConf appendFormat:@"nameserver %s\n", address];
    }
    // what res_ninit allocated, the search domains read above among it
    res_ndestroy(&res);

    // The same servers again: the path monitor says so of any change to the
    // path, on cellular every few seconds, and the file truncated for each
    // rewrite reads empty meanwhile
    if ([resolvConf isEqualToString:self.resolvConf])
        return;
    current = pid_get_task(1);
    struct fd *fd = generic_open("/etc/resolv.conf", O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0666);
    if (!IS_ERR(fd)) {
        fd->ops->write(fd, resolvConf.UTF8String, [resolvConf lengthOfBytesUsingEncoding:NSUTF8StringEncoding]);
        fd_close(fd);
        self.resolvConf = [resolvConf copy];
    }
}

+ (int)bootError {
    return bootError;
}

+ (void)maybeOfferRootResetOnViewController:(UIViewController *)vc {
    static BOOL offered;
    // Not over another alert
    if (offered || vc.presentedViewController != nil || !Roots.instance.defaultRootIsOutdated)
        return;
    offered = YES;
    NSString *message = [NSString stringWithFormat:
                         @"This version of iSH comes with a new built-in filesystem. Your filesystem \"%@\" was set up from an earlier one, or from somewhere else.\n\n"
                         @"Resetting replaces it with the new one. Your files in /root and /home are kept; everything else is replaced, including the packages you installed. iSH then quits; open it again to use the new filesystem.",
                         Roots.instance.defaultRoot];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"New Built-in Filesystem"
                                                                   message:message
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Reset and Quit"
                                              style:UIAlertActionStyleDestructive
                                            handler:^(UIAlertAction *action) {
        [self resetDefaultRootAndQuitOnViewController:vc];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Keep Mine"
                                              style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction *action) {
        [Roots.instance keepOutdatedDefaultRoot];
    }]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Not Now"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    [vc presentViewController:alert animated:YES completion:nil];
}

+ (void)resetDefaultRootAndQuitOnViewController:(UIViewController *)vc {
    NSError *error = nil;
    if (![Roots.instance resetDefaultRoot:&error]) {
        NSLog(@"Failed to reset the default filesystem: %@", error);
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Reset Failed"
                                                                       message:error.localizedDescription
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [vc presentViewController:alert animated:YES completion:nil];
        return;
    }
    exit(0);
}

- (BOOL)application:(UIApplication *)application willFinishLaunchingWithOptions:(NSDictionary<UIApplicationLaunchOptionsKey,id> *)launchOptions {
    // Before anything starts the JIT (BuiltinJIT.shared does, the boot does):
    // a debugger iSH is launched under makes the code region as it starts
    [BuiltinJIT applyCodeRegionSize];
    NSUserDefaults *defaults = NSUserDefaults.standardUserDefaults;
    if ([defaults boolForKey:@"hail mary"]) {
        [defaults removeObjectForKey:kPreferenceBootCommandKey];
        [defaults removeObjectForKey:kPreferenceLaunchCommandKey];
        [defaults setBool:NO forKey:@"hail mary"];
    }
    if ([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"])
        return YES;

    // Registered before launch ends, as BGTaskScheduler requires
    [OnDevicePairing registerBackgroundTask];
    bootError = [self boot];
    // iSH runs from here as it does without the JIT; it translates once the
    // built-in JIT, if enabled in Settings, has its code region (asked for
    // three seconds on, once the launch is done)
    if (bootError == 0)
        [BuiltinJIT.shared startIfEnabled];

    return YES;
}

- (void)userNotificationCenter:(UNUserNotificationCenter *)center didReceiveNotificationResponse:(UNNotificationResponse *)response withCompletionHandler:(void (^)(void))completionHandler {
    NSString *uuid = response.notification.request.content.userInfo[TerminalSessionNotificationUUIDKey];
    for (TerminalViewController *controller in ConnectedTerminalViewControllers()) {
        if ([controller showSessionWithUUID:[[NSUUID alloc] initWithUUIDString:uuid]])
            break;
    }
    completionHandler();
}

- (BOOL)application:(UIApplication *)application didFinishLaunchingWithOptions:(NSDictionary *)launchOptions {
    // get the network permissions popup to appear on chinese devices
    [[NSURLSession.sharedSession dataTaskWithURL:[NSURL URLWithString:@"http://captive.apple.com"]] resume];

    if ([NSUserDefaults.standardUserDefaults boolForKey:@"FASTLANE_SNAPSHOT"])
        [UIView setAnimationsEnabled:NO];

    // Resumes location updates if the keep-alive was left on.
    [BackgroundKeepAlive shared];
    // A tab's notification tapped shows the tab (TerminalSession's
    // attentionAsked); set before launch ends, for one that launched the app
    UNUserNotificationCenter.currentNotificationCenter.delegate = self;
    // Starts collecting what gets copied, for the clipboard manager.
    [ClipboardStore.shared start];

    NSString *ishVersion = [NSString stringWithFormat:@"iSH %@ (%@)",
                         [NSBundle.mainBundle objectForInfoDictionaryKey:@"CFBundleShortVersionString"],
                         [NSBundle.mainBundle objectForInfoDictionaryKey:(NSString *) kCFBundleVersionKey]];
    proc_ish_version = strdup(ishVersion.UTF8String);
    // A program's URL (xdg-open) opens once the person allows it
    open_url = ask_to_open_url;
    // What the screen shows of the app, for a program to see (ish-screenshot);
    // what it brings up in the app to see (ish-ui); the image on the clipboard
    // (ish-paste-image)
    take_screenshot = take_app_screenshot;
    do_ui_action = do_app_ui_action;
    get_clipboard_image = clipboard_image_png;
    // The device's network and power, for a program to decide by
    DeviceStatusStart();
    get_network_report = device_network_report;
    get_power_report = device_power_report;
    get_thermal_state = thermal_state;
    // this defaults key is set when taking app store screenshots
    extern const char *uname_hostname_override;
    // iOS hides the device's own hostname (hostname obfuscation), so the name
    // set in iSH is the hostname even when overriding is not switched on
    NSString *hostnameOverride = UserPreferences.shared._hostnameOverride;
    hostnameOverride = hostnameOverride ? hostnameOverride : UserPreferences.shared.hostnameOverride;
    if (hostnameOverride) {
        uname_hostname_override = strdup(hostnameOverride.UTF8String);
    }
    
    [UserPreferences.shared observe:@[@"shouldDisableDimming"] options:NSKeyValueObservingOptionInitial
                              owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            UIApplication.sharedApplication.idleTimerDisabled = UserPreferences.shared.shouldDisableDimming;
        });
    }];
    
    // The DNS servers again as the network changes (Wi-Fi, cellular, a VPN):
    // the monitor says so of every path, the first at its start. Into init's
    // /etc/resolv.conf: none without a boot.
    if (bootError == 0) {
        self.pathMonitor = nw_path_monitor_create();
        nw_path_monitor_set_queue(self.pathMonitor, dispatch_get_main_queue());
        __weak AppDelegate *weakSelf = self;
        nw_path_monitor_set_update_handler(self.pathMonitor, ^(nw_path_t path) {
            [weakSelf configureDns];
        });
        nw_path_monitor_start(self.pathMonitor);
    }

    if (self.window != nil) {
        // For iOS <13, where the app delegate owns the window instead of the scene
        if ([NSUserDefaults.standardUserDefaults boolForKey:@"recovery"]) {
            UINavigationController *vc = [[UIStoryboard storyboardWithName:@"About" bundle:nil] instantiateInitialViewController];
            AboutViewController *avc = (AboutViewController *) vc.topViewController;
            avc.recoveryMode = YES;
            self.window.rootViewController = vc;
            return YES;
        }
        TerminalViewController *vc = (TerminalViewController *) self.window.rootViewController;
        currentTerminalViewController = vc;
        [vc startNewSession];
    }
    return YES;
}

- (void)buildMenuWithBuilder:(id<UIMenuBuilder>)builder {
    [super buildMenuWithBuilder:builder];
    if (builder.system != UIMenuSystem.mainSystem)
        return;
    [MainMenu buildWithBuilder:builder];
}

#pragma mark Command routing

// Menu items and key commands are sent to the first responder and up its chain.
// While the terminal has the keyboard that chain includes the terminal view
// controller, but with the keyboard dismissed nothing is first responder, the
// window is, and the controller would be skipped. Being the end of every chain,
// the app delegate hands such commands to the active window's controller.
- (TerminalViewController *)terminalTargetForAction:(SEL)action sender:(id)sender {
    TerminalViewController *vc = currentTerminalViewController;
    if (vc == nil || vc.presentedViewController != nil)
        return nil;
    if (![vc canPerformAction:action withSender:sender])
        return nil;
    return vc;
}

- (BOOL)canPerformAction:(SEL)action withSender:(id)sender {
    return [super canPerformAction:action withSender:sender] || [self terminalTargetForAction:action sender:sender] != nil;
}

- (id)targetForAction:(SEL)action withSender:(id)sender {
    return [super targetForAction:action withSender:sender] ?: [self terminalTargetForAction:action sender:sender];
}

- (void)validateCommand:(UICommand *)command {
    TerminalViewController *vc = [self terminalTargetForAction:command.action sender:command];
    if (vc != nil)
        [vc validateCommand:command];
    else
        [super validateCommand:command];
}

- (id)forwardingTargetForSelector:(SEL)selector {
    TerminalViewController *vc = currentTerminalViewController;
    if (vc != nil && [vc respondsToSelector:selector])
        return vc;
    return [super forwardingTargetForSelector:selector];
}

- (void)application:(UIApplication *)application didDiscardSceneSessions:(NSSet<UISceneSession *> *)sceneSessions {
    // Tabs can be moved between windows, so a session listed by a discarded window
    // may live on in another one; only sessions no window shows are closed.
    NSMutableSet<NSUUID *> *shown = [NSMutableSet new];
    for (TerminalViewController *vc in ConnectedTerminalViewControllers())
        [shown addObjectsFromArray:vc.tabTerminalUUIDs];
    for (UISceneSession *sceneSession in sceneSessions) {
        if (sceneSession.stateRestorationActivity == nil)
            continue;
        TerminalSessionStore *store = TerminalSessionStore.shared;
        for (NSUUID *uuid in SceneTerminalUUIDs(sceneSession.stateRestorationActivity)) {
            if ([shown containsObject:uuid])
                continue;
            TerminalSession *session = [store sessionWithUUID:uuid];
            if (session != nil)
                [store closeSession:session];
        }
    }
}

- (void)dealloc {
    if (self.pathMonitor != nil)
        nw_path_monitor_cancel(self.pathMonitor);
}

- (void)exitApp {
    self.exiting = YES;
    id app = [UIApplication sharedApplication];
    [app suspend];
}

- (void)applicationDidEnterBackground:(UIApplication *)application {
    if (self.exiting)
        exit(0);
}

// iOS terminating the app while it runs (swiped away in the app switcher, in
// the foreground or kept alive in the background): the programs in the tabs
// are told the terminal went away, and given a second to finish, rather than
// killed with the app (TerminalSessionStore). Swiped away, iOS killed the app
// 0.5 to 1.6 s after the tabs were hung up, not the five seconds its
// documentation gives this method: waited longer for, the main thread was
// held till iOS killed it. A shell or a program that ends on SIGHUP is done
// in that time; Claude Code's shutdown, slower in iSH, is not. A suspended app
// iOS kills without calling this, and an app being updated too.
- (void)applicationWillTerminate:(UIApplication *)application {
    [TerminalSessionStore.shared hangUpForTerminationWaiting:1];
}

@end

NSString *const ProcessExitedNotification = @"ProcessExitedNotification";
