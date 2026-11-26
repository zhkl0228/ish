//
//  OnDevicePairing.m
//  iSH
//
//  The order, as tctiSH found it works (its PairableHostPairing.swift): an
//  identity and its TXT records (pairable_host_prepare); a listening socket;
//  the Bonjour service published with NetService, with an empty domain and the
//  type's trailing dot (with "local." and none the device never saw it); the
//  device's connection accepted, and pair-setup run on it
//  (pairable_host_accept_fd), which asks for the code the user types.
//

#import <BackgroundTasks/BackgroundTasks.h>
#import <UIKit/UIKit.h>
#import <arpa/inet.h>
#import <netinet/in.h>
#import <poll.h>
#import <sys/socket.h>
#import <unistd.h>
#import "OnDevicePairing.h"
#include "idevice.h"

// What the device shows: "Pair with iSH", a Mac
static const char *const kHostName = "iSH";
static NSString *const kServiceType = @"_remotepairing-pairable-host._tcp.";
// How long the advertisement waits for the device
static const NSTimeInterval kDeadline = 5 * 60;

// An idevice error as text, freed
static NSString *ErrorText(struct IdeviceFfiError *error) {
    NSString *text = [NSString stringWithFormat:@"%s (code %d)", error->message ? error->message : "no message",
                      error->code];
    idevice_error_free(error);
    return text;
}

@interface OnDevicePairing () <NSNetServiceDelegate>
@property (nonatomic) void (^onPIN)(NSString *);
@property (nonatomic) void (^done)(NSData *_Nullable, NSString *_Nullable);
@end

@implementation OnDevicePairing {
    struct PairableHostHandle *_host;
    NSNetService *_service;
    int _listener;
    int _connection;           // the device's, while pair-setup runs on it
    BOOL _cancelled, _finished;
    UIBackgroundTaskIdentifier _assertion;
}

+ (BOOL)available {
    if (@available(iOS 27.0, *))
        return YES;
    return NO;
}

#pragma mark Keeping iSH running

// The continued processing task the system runs while the user is away, and
// shows: where the code is read in Settings (its subtitle). An iOS 26 SDK's:
// an earlier one's build keeps iSH running only as beginBackgroundTask does.
static id continuedTask;
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
// Info.plist's BGTaskSchedulerPermittedIdentifiers has it
static NSString *TaskIdentifier(void) {
    return [NSBundle.mainBundle.bundleIdentifier stringByAppendingString:@".pairing"];
}
#endif

+ (void)registerBackgroundTask {
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
    if (@available(iOS 26.0, *)) {
        [BGTaskScheduler.sharedScheduler registerForTaskWithIdentifier:TaskIdentifier()
                                                            usingQueue:dispatch_get_main_queue()
                                                         launchHandler:^(BGTask *task) {
            continuedTask = task;
            task.expirationHandler = ^{
                continuedTask = nil;
            };
        }];
    }
#endif
}

- (void)keepAlive:(NSString *)subtitle {
    _assertion = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"Pairing" expirationHandler:^{
        [UIApplication.sharedApplication endBackgroundTask:self->_assertion];
        self->_assertion = UIBackgroundTaskInvalid;
    }];
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
    if (@available(iOS 26.0, *)) {
        BGContinuedProcessingTaskRequest *request =
            [[BGContinuedProcessingTaskRequest alloc] initWithIdentifier:TaskIdentifier() title:@"Pairing iSH" subtitle:subtitle];
        request.strategy = BGContinuedProcessingTaskRequestSubmissionStrategyQueue;
        NSError *error = nil;
        if (![BGTaskScheduler.sharedScheduler submitTaskRequest:request error:&error])
            NSLog(@"Pairing: no continued processing task: %@", error);
    }
#endif
}

- (void)keepAliveSays:(NSString *)subtitle {
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
    if (@available(iOS 26.0, *)) {
        BGContinuedProcessingTask *task = continuedTask;
        [task updateTitle:@"Pairing iSH" subtitle:subtitle];
    }
#endif
}

- (void)letGo {
#if __IPHONE_OS_VERSION_MAX_ALLOWED >= 260000
    if (@available(iOS 26.0, *)) {
        BGContinuedProcessingTask *task = continuedTask;
        [task setTaskCompletedWithSuccess:YES];
        continuedTask = nil;
    }
#endif
    if (_assertion != UIBackgroundTaskInvalid) {
        [UIApplication.sharedApplication endBackgroundTask:_assertion];
        _assertion = UIBackgroundTaskInvalid;
    }
}

#pragma mark Pairing

- (instancetype)init {
    if (self = [super init]) {
        _listener = -1;
        _connection = -1;
        _assertion = UIBackgroundTaskInvalid;
    }
    return self;
}

- (void)startWithPIN:(void (^)(NSString *))onPIN done:(void (^)(NSData *, NSString *))done {
    self.onPIN = onPIN;
    self.done = done;
    [self keepAlive:@"Waiting for this device to connect"];

    char *serviceIdentifier = NULL;
    uint8_t *txt = NULL;
    size_t txtLength = 0;
    struct IdeviceFfiError *error = pairable_host_prepare(kHostName, NULL, false, &_host, &serviceIdentifier, &txt,
                                                          &txtLength, NULL);
    if (error != NULL) {
        [self finish:nil failure:[@"no host identity: " stringByAppendingString:ErrorText(error)]];
        return;
    }
    NSString *name = @(serviceIdentifier);
    NSDictionary<NSString *, NSString *> *records =
        [NSPropertyListSerialization propertyListWithData:[NSData dataWithBytes:txt length:txtLength]
                                                  options:0 format:NULL error:NULL];
    idevice_string_free(serviceIdentifier);
    idevice_data_free(txt, txtLength);

    NSString *failure = nil;
    uint16_t port = [self listen:&failure];
    if (failure != nil) {
        [self finish:nil failure:failure];
        return;
    }
    NSMutableDictionary<NSString *, NSData *> *txtRecord = [NSMutableDictionary new];
    [records enumerateKeysAndObjectsUsingBlock:^(NSString *key, NSString *value, BOOL *_) {
        txtRecord[key] = [value dataUsingEncoding:NSUTF8StringEncoding];
    }];
    _service = [[NSNetService alloc] initWithDomain:@"" type:kServiceType name:name port:port];
    _service.delegate = self;
    [_service setTXTRecordData:[NSNetService dataFromTXTRecordDictionary:txtRecord]];
    [_service publish];
    NSLog(@"Pairing: advertising %@ on port %u", name, port);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        [self waitForDevice];
    });
}

- (void)netService:(NSNetService *)sender didNotPublish:(NSDictionary<NSString *, NSNumber *> *)errorDict {
    [self finish:nil failure:[NSString stringWithFormat:@"the Bonjour service was not published: %@", errorDict]];
}

// A listening socket on a free port; 0 and why if there is none
- (uint16_t)listen:(NSString **)failure {
    _listener = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in address = {.sin_len = sizeof(address), .sin_family = AF_INET, .sin_addr.s_addr = INADDR_ANY};
    socklen_t length = sizeof(address);
    if (_listener < 0 || bind(_listener, (struct sockaddr *) &address, sizeof(address)) != 0 ||
        listen(_listener, 1) != 0 || getsockname(_listener, (struct sockaddr *) &address, &length) != 0) {
        *failure = [NSString stringWithFormat:@"no listening socket: %s", strerror(errno)];
        return 0;
    }
    return ntohs(address.sin_port);
}

- (BOOL)stopping {
    @synchronized (self) {
        return _cancelled || _finished;
    }
}

- (void)waitForDevice {
    NSDate *started = NSDate.date;
    while (!self.stopping) {
        if (-started.timeIntervalSinceNow > kDeadline) {
            [self finish:nil failure:@"the device did not connect in 5 minutes: was \"Pair with iSH\" tapped in "
                                      "Settings › Privacy & Security › Developer Mode, and is iSH allowed the local "
                                      "network (Settings › Privacy & Security › Local Network)?"];
            return;
        }
        struct pollfd p = {.fd = _listener, .events = POLLIN};
        int ready = poll(&p, 1, 1000);
        if (ready < 0 && errno != EINTR) {
            [self finish:nil failure:[NSString stringWithFormat:@"poll: %s", strerror(errno)]];
            return;
        }
        if (ready <= 0)
            continue;
        int connection = accept(_listener, NULL, NULL);
        if (connection < 0) {
            if (errno == EINTR || errno == ECONNABORTED)
                continue;
            [self finish:nil failure:[NSString stringWithFormat:@"accept: %s", strerror(errno)]];
            return;
        }
        [self pairOn:connection];
        return;
    }
}

static void PairingPIN(const char *pin, void *context) {
    OnDevicePairing *pairing = (__bridge OnDevicePairing *) context;
    NSString *code = @(pin);
    dispatch_async(dispatch_get_main_queue(), ^{
        [pairing keepAliveSays:[NSString stringWithFormat:@"Enter %@ on this device", code]];
        pairing.onPIN(code);
    });
}

- (void)pairOn:(int)connection {
    @synchronized (self) {
        _connection = connection;
        if (_cancelled)
            shutdown(connection, SHUT_RDWR);
    }
    NSLog(@"Pairing: the device connected");
    struct RpPairingFileHandle *pairingFile = NULL;
    struct IdeviceFfiError *error = pairable_host_accept_fd(_host, connection, PairingPIN, (__bridge void *) self,
                                                            NULL, &pairingFile);
    @synchronized (self) {
        _connection = -1;
        close(connection);
    }
    if (error != NULL) {
        NSString *text = ErrorText(error);
        // A cancel breaks the connection under it: no failure of the user's
        if (!self.stopping)
            [self finish:nil failure:[@"pairing failed: " stringByAppendingString:text]];
        return;
    }
    uint8_t *bytes = NULL;
    size_t length = 0;
    error = rp_pairing_file_to_bytes(pairingFile, &bytes, &length);
    rp_pairing_file_free(pairingFile);
    if (error != NULL) {
        [self finish:nil failure:[@"the pairing file was not written out: " stringByAppendingString:ErrorText(error)]];
        return;
    }
    NSData *data = [NSData dataWithBytes:bytes length:length];
    idevice_data_free(bytes, length);
    [self finish:data failure:nil];
}

- (void)cancel {
    @synchronized (self) {
        _cancelled = YES;
        // pair-setup takes no cancellation: its connection, broken, ends it
        // (shutdown reaches the copy it works on; close would not)
        if (_connection >= 0)
            shutdown(_connection, SHUT_RDWR);
    }
    [self finish:nil failure:nil];
}

// The end, once: done on the main thread, and everything given back
- (void)finish:(nullable NSData *)pairingFile failure:(nullable NSString *)failure {
    @synchronized (self) {
        if (_finished)
            return;
        _finished = YES;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        [self->_service stop];
        self->_service = nil;
        if (self->_listener >= 0) {
            close(self->_listener);
            self->_listener = -1;
        }
        [self letGo];
        self.done(pairingFile, failure);
        // The handle once nothing uses it: pair-setup has returned, or never began
        dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
            while (YES) {
                @synchronized (self) {
                    if (self->_connection < 0)
                        break;
                }
                usleep(100000);
            }
            if (self->_host != NULL) {
                pairable_host_free(self->_host);
                self->_host = NULL;
            }
        });
    });
}

@end
