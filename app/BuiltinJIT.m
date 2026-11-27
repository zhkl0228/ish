//
//  BuiltinJIT.m
//  iSH
//
//  See BuiltinJIT.h. The order, as StikJIT's INTEGRATION.md has it for an app
//  that is its own debugger's client: the tunnel; the helper started, to
//  attach with StikJIT's universal script; iSH, once a debugger has it, asks
//  for its code region (brk #0xf00d, JIT26) and lets it go; the helper
//  answers when the script ends.
//

#import <NetworkExtension/NetworkExtension.h>
#import <arpa/inet.h>
#import <fcntl.h>
#import <netinet/in.h>
#import <poll.h>
#import <sys/socket.h>
#import <sys/sysctl.h>
#import <unistd.h>
#import "BuiltinJIT.h"
#include "fs/proc/ish.h"
#include "kernel/mm.h"
#include "platform/platform.h"

NSNotificationName const BuiltinJITChangedNotification = @"BuiltinJITChangedNotification";

static NSString *const kEnabledAtStartKey = @"Built-in JIT";
static NSString *const kCodeRegionMBKey = @"Built-in JIT Code Region MB";
static const NSUInteger kCodeRegionMBDefault = 120;

// asbestos/guest-arm64/jit.h's (a header for builds with ISH_JIT, which libish
// is for the app: GuestARM64.xcconfig's ISH_ENABLE_JIT)
bool jit_region_now(bool from_debugger);
size_t jit_describe(char *buf, size_t size);
void jit_set_region_mb(unsigned mb);
bool jit_region_usage(size_t *used, size_t *size);

// <sys/codesign.h>'s, which the iOS SDK leaves out
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_OPS_STATUS 0
#define CS_DEBUGGED 0x10000000

// Whether a debugger has had iSH (it stays once the debugger detached)
static bool HadDebugger(void) {
    uint32_t flags = 0;
    return csops(getpid(), CS_OPS_STATUS, &flags, sizeof(flags)) == 0 && (flags & CS_DEBUGGED);
}

// Whether a debugger has iSH now
static bool DebuggerAttached(void) {
    struct kinfo_proc info;
    size_t size = sizeof(info);
    int mib[] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    return sysctl(mib, 4, &info, &size, NULL, 0) == 0 && (info.kp_proc.p_flag & P_TRACED);
}

// What /proc/ish/jit says: whether the JIT translates, and of its code region
static bool JITTranslating(NSString **region) {
    char text[16384];
    size_t n = jit_describe(text, sizeof(text));
    NSString *description = [[NSString alloc] initWithBytes:text length:n encoding:NSUTF8StringEncoding];
    bool translating = false;
    for (NSString *line in [description componentsSeparatedByString:@"\n"]) {
        if ([line hasPrefix:@"mode: JIT"])
            translating = true;
        if ([line hasPrefix:@"code region:"] && region != NULL)
            *region = line;
    }
    return translating;
}

// Whether a loopback tunnel is up: the device answers at the peer address
// (StikJIT's default, 10.7.0.1), refusing or taking a connection at once.
// Without a tunnel the address leads nowhere.
static bool LoopbackUp(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    struct sockaddr_in peer = {.sin_len = sizeof(peer), .sin_family = AF_INET, .sin_port = htons(49152)};
    inet_pton(AF_INET, "10.7.0.1", &peer.sin_addr);
    int err = connect(fd, (struct sockaddr *) &peer, sizeof(peer)) == 0 ? 0 : errno;
    if (err == EINPROGRESS) {
        struct pollfd p = {.fd = fd, .events = POLLOUT};
        if (poll(&p, 1, 1000) == 1) {
            socklen_t len = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        }
    }
    close(fd);
    return err == 0 || err == ECONNREFUSED;
}

// The private NSExtension: an app can start an extension of its own on
// demand no other way. iSH is installed by its user, signed as they sign it,
// never through the App Store. A request is its own reply channel.
@protocol ISHNSExtensionClass <NSObject>
- (nullable id)extensionWithIdentifier:(NSString *)identifier error:(NSError **)error;
@end
@protocol ISHNSExtension <NSObject>
- (void)beginExtensionRequestWithInputItems:(nullable NSArray *)items completion:(void (^)(NSUUID *request))completion;
- (void)setRequestCompletionBlock:(void (^)(NSUUID *request, NSArray *items))block;
- (void)setRequestInterruptionBlock:(void (^)(NSUUID *request))block;
@end

// One request to the helper (app/JITHelper/JITHelperRequestHandler.swift has
// what goes and what comes back)
@interface HelperRequest : NSObject
@property (nullable) NSDictionary *reply;
@property (nullable) NSString *failure;
@end

@implementation HelperRequest {
    // Left when the request ends: waited for as often as need be
    dispatch_group_t _done;
    // Kept until the request ends: the blocks it holds hold this
    id<ISHNSExtension> _extension;
    BOOL _finished;
}

// Why it could not be started, or nil
- (nullable NSString *)start:(NSDictionary *)request {
    _done = dispatch_group_create();
    NSString *identifier = [NSBundle.mainBundle.bundleIdentifier stringByAppendingString:@".JITHelper"];
    Class extensionClass = NSClassFromString(@"NSExtension");
    if (extensionClass == nil)
        return @"this iOS has no NSExtension to start the helper with";
    NSError *error = nil;
    id<ISHNSExtension> extension = [(id<ISHNSExtensionClass>) extensionClass extensionWithIdentifier:identifier error:&error];
    if (extension == nil)
        return [NSString stringWithFormat:@"no helper extension %@: %@", identifier, error.localizedDescription];
    _extension = extension;
    dispatch_group_enter(_done);
    [extension setRequestCompletionBlock:^(NSUUID *_, NSArray *items) {
        [self finishWithReply:((NSExtensionItem *) items.firstObject).userInfo failure:nil];
    }];
    [extension setRequestInterruptionBlock:^(NSUUID *_) {
        [self finishWithReply:nil failure:@"the helper extension ended before it answered"];
    }];
    NSExtensionItem *item = [NSExtensionItem new];
    item.userInfo = request;
    [extension beginExtensionRequestWithInputItems:@[item] completion:^(NSUUID *_) {}];
    return nil;
}

- (void)finishWithReply:(nullable NSDictionary *)reply failure:(nullable NSString *)failure {
    @synchronized (self) {
        if (_finished)
            return;
        _finished = YES;
        _extension = nil;
    }
    self.reply = reply;
    self.failure = failure;
    dispatch_group_leave(_done);
}

// Whether it ended within seconds
- (BOOL)waitFor:(NSTimeInterval)seconds {
    return dispatch_group_wait(_done, dispatch_time(DISPATCH_TIME_NOW, (int64_t) (seconds * NSEC_PER_SEC))) == 0;
}

// Given up on: what it answers later is dropped
- (void)abandon:(NSString *)why {
    [self finishWithReply:nil failure:why];
}

@end

@interface BuiltinJIT ()
@property NSString *status;
@property NSArray<NSString *> *log;
@property BOOL busy;
@property BOOL translating;
@end

// /proc/ish/jit's lines of it (fs/proc/ish.h), read on a guest's thread: its
// properties are atomic, and status and log immutable once set
static char *BuiltinJITReport(void) {
    BuiltinJIT *jit = BuiltinJIT.shared;
    NSMutableString *text = [NSMutableString new];
    [text appendFormat:@"  pairing file: %@ (Documents/StikJIT/pairingFile.plist)\n", jit.hasPairingFile ? @"yes" : @"no"];
    [text appendFormat:@"  enabled as iSH starts: %@\n", jit.enabledAtStart ? @"yes" : @"no"];
    [text appendFormat:@"  code region asked for: %lu MB (Settings)\n", (unsigned long) BuiltinJIT.codeRegionMB];
    [text appendFormat:@"  translating: %@\n", jit.translating ? @"yes" : @"no"];
    [text appendFormat:@"  status: %@\n", jit.status];
    NSArray<NSString *> *log = jit.log;
    if (log.count != 0) {
        [text appendString:@"  the last attempt's steps:\n"];
        for (NSString *line in log)
            [text appendFormat:@"    %@\n", line];
    }
    return strdup(text.UTF8String);
}

@implementation BuiltinJIT {
    dispatch_queue_t _queue;
    NSMutableArray<NSString *> *_steps;
}

+ (void)load {
    get_builtin_jit_report = BuiltinJITReport;
}

+ (instancetype)shared {
    static BuiltinJIT *shared;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        shared = [BuiltinJIT new];
    });
    return shared;
}

- (instancetype)init {
    if (self = [super init]) {
        _queue = dispatch_queue_create("app.ish.BuiltinJIT", DISPATCH_QUEUE_SERIAL);
        _steps = [NSMutableArray new];
        _log = @[];
        NSString *region = nil;
        _translating = JITTranslating(&region);
        _status = _translating ? @"The JIT translates (a debugger had iSH as it started)" : @"Not enabled";
    }
    return self;
}

- (void)changed {
    dispatch_async(dispatch_get_main_queue(), ^{
        [NSNotificationCenter.defaultCenter postNotificationName:BuiltinJITChangedNotification object:self];
    });
}

- (void)step:(NSString *)line {
    NSLog(@"Built-in JIT: %@", line);
    @synchronized (_steps) {
        [_steps addObject:line];
        self.log = [_steps copy];
    }
    self.status = line;
    [self changed];
}

#pragma mark Settings

- (BOOL)enabledAtStart {
    return [NSUserDefaults.standardUserDefaults boolForKey:kEnabledAtStartKey];
}

- (void)setEnabledAtStart:(BOOL)enabledAtStart {
    [NSUserDefaults.standardUserDefaults setBool:enabledAtStart forKey:kEnabledAtStartKey];
    [self changed];
}

// Whole arenas of 128 MB (asbestos/guest-arm64/jit.c) past the first: the
// native dispatch's window, 128 MB from each arena's start, ends where the
// region does. 120, the size before there were arenas, leaves 8 MB past it,
// which the JIT reserves when it can.
+ (NSArray<NSNumber *> *)codeRegionChoices {
    return @[@120, @256, @512, @768, @1024, @1280];
}

+ (NSUInteger)codeRegionMB {
    NSInteger mb = [NSUserDefaults.standardUserDefaults integerForKey:kCodeRegionMBKey];
    return mb > 0 ? (NSUInteger) mb : kCodeRegionMBDefault;
}

+ (void)setCodeRegionMB:(NSUInteger)codeRegionMB {
    [NSUserDefaults.standardUserDefaults setInteger:(NSInteger) codeRegionMB forKey:kCodeRegionMBKey];
    // A region not made yet is made this size (jit_region_now())
    jit_set_region_mb((unsigned) codeRegionMB);
    [BuiltinJIT.shared changed];
}

+ (void)applyCodeRegionSize {
    jit_set_region_mb((unsigned) self.codeRegionMB);
}

- (NSUInteger)codeRegionMBNow {
    size_t used, size;
    return jit_region_usage(&used, &size) ? size >> 20 : 0;
}

- (NSURL *)pairingFileURL {
    NSURL *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask][0];
    return [documents URLByAppendingPathComponent:@"StikJIT/pairingFile.plist"];
}

- (BOOL)hasPairingFile {
    return [NSFileManager.defaultManager isReadableFileAtPath:self.pairingFileURL.path];
}

- (BOOL)importPairingFileFromURL:(NSURL *)url error:(NSError **)error {
    BOOL scoped = [url startAccessingSecurityScopedResource];
    NSData *data = [NSData dataWithContentsOfURL:url options:0 error:error];
    if (scoped)
        [url stopAccessingSecurityScopedResource];
    if (data == nil)
        return NO;
    return [self savePairingFile:data error:error];
}

- (BOOL)savePairingFile:(NSData *)data error:(NSError **)error {
    NSURL *destination = self.pairingFileURL;
    if (![NSFileManager.defaultManager createDirectoryAtURL:destination.URLByDeletingLastPathComponent
                                withIntermediateDirectories:YES attributes:nil error:error])
        return NO;
    if (![data writeToURL:destination options:NSDataWritingAtomic error:error])
        return NO;
    self.status = @"Pairing file saved";
    [self changed];
    return YES;
}

#pragma mark Enabling

- (void)startIfEnabled {
    if (self.enabledAtStart)
        [self enable];
}

- (void)enable {
    dispatch_async(_queue, ^{
        [self run:@"Enabling the JIT" with:^NSString *{
            return [self enableJIT];
        }];
    });
}

- (void)prepareDevice {
    dispatch_async(_queue, ^{
        [self run:@"Preparing the device" with:^NSString *{
            return [self askHelper:@"prepare"];
        }];
    });
}

- (void)resetDeveloperDiskImage {
    dispatch_async(_queue, ^{
        [self run:@"Removing the developer disk image" with:^NSString *{
            return [self askHelper:@"resetDDI"];
        }];
    });
}

// An attempt, its log new; body returns why it failed, or nil
- (void)run:(NSString *)what with:(NSString *_Nullable (^)(void))body {
    @synchronized (_steps) {
        [_steps removeAllObjects];
    }
    self.busy = YES;
    [self step:[what stringByAppendingString:@"…"]];
    NSString *failure = body();
    self.busy = NO;
    if (failure != nil)
        [self step:[@"Failed: " stringByAppendingString:failure]];
}

// Why the helper can not be asked, or nil
- (nullable NSString *)preflight {
    char why[160];
    if (host_entitlement("get-task-allow", why, sizeof(why)) != ENTITLEMENT_YES)
        return @"iSH is not signed with get-task-allow, which a debugger needs to attach: sign it with a development profile";
    if (!self.hasPairingFile)
        return @"no pairing file: import one in Settings, or put it at Documents/StikJIT/pairingFile.plist";
    return nil;
}

// Whether StikJIT's universal script stays attached for iSH's requests,
// TXM or not: iSH asks by the protocol on iOS 26 (jit.c), and a request no
// script answers kills it
static BOOL ScriptStays(void) {
    if (@available(iOS 26.0, *))
        return YES;
    return NO;
}

// The helper's request: the operation; the pairing file for those that
// reach the device; for "enable" iSH's pid and whether the script stays
- (NSDictionary *)request:(NSString *)operation {
    NSMutableDictionary *request = [@{@"operation": operation} mutableCopy];
    if (![operation isEqualToString:@"resetDDI"])
        request[@"pairingFile"] = [NSData dataWithContentsOfURL:self.pairingFileURL];
    if ([operation isEqualToString:@"enable"]) {
        request[@"pid"] = @(getpid());
        request[@"forceScript"] = @(ScriptStays());
    }
    return request;
}

// Its reply, in the log; why it failed, or nil
- (nullable NSString *)takeReply:(HelperRequest *)helper {
    if (helper.failure != nil)
        return helper.failure;
    NSDictionary *reply = helper.reply;
    for (NSString *line in reply[@"log"])
        [self step:[@"helper: " stringByAppendingString:line]];
    NSNumber *txm = reply[@"txm"];
    if (txm != nil)
        [self step:txm.boolValue ? @"TXM: present" : @"TXM: not present"];
    if (![reply[@"outcome"] isEqualToString:@"succeeded"])
        return reply[@"detail"];
    [self step:reply[@"detail"]];
    return nil;
}

// A request that needs no debugger: the device's preparation (which reaches
// it, over the tunnel), the disk image's removal (which does not)
- (nullable NSString *)askHelper:(NSString *)operation {
    BOOL reachesDevice = ![operation isEqualToString:@"resetDDI"];
    NSString *failure = reachesDevice ? [self preflight] : nil;
    if (failure != nil)
        return failure;
    BOOL ownTunnel = NO;
    if (reachesDevice && (failure = [self tunnelUp:&ownTunnel]) != nil)
        return failure;
    HelperRequest *helper = [HelperRequest new];
    failure = [helper start:[self request:operation]];
    if (failure == nil && ![helper waitFor:15 * 60]) {
        failure = @"the helper did not answer in 15 minutes";
        [helper abandon:failure];
    }
    if (failure == nil)
        failure = [self takeReply:helper];
    if (ownTunnel)
        [self tunnelDown];
    return failure;
}

- (nullable NSString *)enableJIT {
    NSString *region = nil;
    if (JITTranslating(&region)) {
        self.translating = YES;
        [self step:@"The JIT translates already"];
        return nil;
    }
    NSString *failure = [self preflight];
    if (failure != nil)
        return failure;
    BOOL ownTunnel = NO;
    failure = [self tunnelUp:&ownTunnel];
    if (failure != nil)
        return failure;

    HelperRequest *helper = [HelperRequest new];
    failure = [helper start:[self request:@"enable"]];
    if (failure == nil) {
        [self step:@"Helper started: it reaches the device's services, mounts the developer disk image if it is not "
                    "(the first time it downloads it), and attaches"];
        if (ScriptStays()) {
            // The script attached waits for iSH's requests: iSH makes them
            // once it is attached, and the script detaches at the last
            BOOL attached = NO;
            while (!(attached = DebuggerAttached()) && ![helper waitFor:0.1])
                ;
            if (attached) {
                [self step:@"Debugger attached: asking it for the code region"];
                jit_region_now(true);
            }
            if (![helper waitFor:5 * 60]) {
                failure = @"the helper did not answer in 5 minutes after";
                [helper abandon:failure];
            }
        } else {
            // It attaches and detaches; then iSH maps its region itself
            if (![helper waitFor:15 * 60]) {
                failure = @"the helper did not answer in 15 minutes";
                [helper abandon:failure];
            } else if (HadDebugger()) {
                jit_region_now(false);
            }
        }
        if (failure == nil)
            failure = [self takeReply:helper];
    }
    if (ownTunnel)
        [self tunnelDown];

    self.translating = JITTranslating(&region);
    if (region != nil)
        [self step:region];
    if (failure != nil)
        return failure;
    if (!self.translating)
        return @"no code region (/proc/ish/jit)";
    // Processes waiting now compiled their code without it: they compile it
    // again, translated, when they next run
    ish_reclaim_translations();
    [self step:@"The JIT translates"];
    return nil;
}

#pragma mark The tunnel

// The tunnel to the device's services: one up already (LocalDevVPN's, or
// this one from before), or this app's, started (ownTunnel then)
- (nullable NSString *)tunnelUp:(BOOL *)ownTunnel {
    if (LoopbackUp()) {
        [self step:@"A loopback tunnel is up"];
        return nil;
    }
    [self step:@"Starting iSH's loopback tunnel"];
    NSString *failure = nil;
    NETunnelProviderManager *manager = [self tunnelManager:&failure];
    if (manager == nil)
        return failure;
    NSError *error = nil;
    if (![manager.connection startVPNTunnelAndReturnError:&error])
        return [@"the loopback tunnel did not start: " stringByAppendingString:error.localizedDescription];
    for (int i = 0; i < 150 && manager.connection.status != NEVPNStatusConnected; i++)
        usleep(100000);
    if (manager.connection.status != NEVPNStatusConnected) {
        [manager.connection stopVPNTunnel];
        return [NSString stringWithFormat:@"the loopback tunnel did not connect in 15 seconds (status %ld)",
                (long) manager.connection.status];
    }
    *ownTunnel = YES;
    return nil;
}

- (void)tunnelDown {
    NETunnelProviderManager *manager = [self tunnelManager:NULL];
    [manager.connection stopVPNTunnel];
    [self step:@"iSH's loopback tunnel stopped"];
}

- (NSString *)tunnelIdentifier {
    return [NSBundle.mainBundle.bundleIdentifier stringByAppendingString:@".LoopbackTunnel"];
}

// iSH's tunnel configuration, made the first time (iOS asks to allow it),
// enabled and loaded; nil and why if there is none
- (nullable NETunnelProviderManager *)tunnelManager:(NSString **)why {
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    __block NETunnelProviderManager *manager = nil;
    __block NSError *error = nil;
    [NETunnelProviderManager loadAllFromPreferencesWithCompletionHandler:^(NSArray<NETunnelProviderManager *> *managers,
                                                                           NSError *loadError) {
        for (NETunnelProviderManager *candidate in managers) {
            NETunnelProviderProtocol *protocol = (NETunnelProviderProtocol *) candidate.protocolConfiguration;
            if ([protocol.providerBundleIdentifier isEqualToString:self.tunnelIdentifier])
                manager = candidate;
        }
        error = loadError;
        dispatch_semaphore_signal(done);
    }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    if (error != nil) {
        if (why != NULL)
            *why = [@"the VPN configurations could not be read: " stringByAppendingString:error.localizedDescription];
        return nil;
    }
    if (manager == nil) {
        manager = [NETunnelProviderManager new];
        NETunnelProviderProtocol *protocol = [NETunnelProviderProtocol new];
        protocol.providerBundleIdentifier = self.tunnelIdentifier;
        protocol.serverAddress = @"10.7.0.1 (this device)";
        manager.protocolConfiguration = protocol;
        manager.localizedDescription = @"iSH JIT loopback";
    }
    if (!manager.enabled) {
        // iOS asks to allow it, the first time: the wait is the user's
        manager.enabled = YES;
        [manager saveToPreferencesWithCompletionHandler:^(NSError *saveError) {
            error = saveError;
            dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        if (error != nil) {
            if (why != NULL)
                *why = [@"the loopback tunnel's VPN configuration was not saved: " stringByAppendingString:error.localizedDescription];
            return nil;
        }
        // A configuration saved is loaded again before it is started
        [manager loadFromPreferencesWithCompletionHandler:^(NSError *loadError) {
            error = loadError;
            dispatch_semaphore_signal(done);
        }];
        dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
        if (error != nil) {
            if (why != NULL)
                *why = [@"the loopback tunnel's VPN configuration was not loaded: " stringByAppendingString:error.localizedDescription];
            return nil;
        }
    }
    return manager;
}

@end
