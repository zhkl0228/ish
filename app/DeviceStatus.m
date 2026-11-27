//
//  DeviceStatus.m
//  iSH
//
//  /proc/ish/network and /proc/ish/power: what the device is on now, for a
//  program to decide by (a large download on a cellular network, a long build
//  on a battery running low), with /proc/ish/thermal. Kept as iOS says it
//  changes, on the main thread, and read on any.
//
//  network:
//    status: satisfied, unsatisfied, satisfiable (once a connection is made,
//            as a VPN on demand) or invalid
//    interfaces: those the path uses (wifi, cellular, wired, loopback,
//                other), none if it uses none
//    expensive: yes on a cellular network or a phone's hotspot
//    constrained: yes in Low Data Mode
//  power:
//    battery: its charge, in percent
//    state: unplugged, charging, full (plugged in), unknown
//    low power mode: on or off
//
//  /proc/ish/device, what a program changing the app's look goes by (with
//  ish-screenshot), read on the main thread as it is asked for:
//    model: the hardware's (iPhone18,1)
//    system: iOS and its version
//    window: the terminal's window in front, its size in points and scale (a
//            phone that folds: 466x678 folded, 951x669 unfolded, @3)
//    orientation: portrait, portrait upside down, landscape left or right
//    size classes: its width's and height's, compact or regular
//    appearance: dark or light
//    external keyboard: yes or no
//

#import <UIKit/UIKit.h>
#import <Network/Network.h>
#import <GameController/GameController.h>
#include <sys/sysctl.h>
#import "DeviceStatus.h"
#import "SceneDelegate.h"
#include "kernel/errno.h"
#include "util/sync.h"

static lock_t status_lock = LOCK_INITIALIZER;
// Under status_lock: what each file says now; the network's nil until the
// path monitor's first update
static NSString *network_report;
static NSString *power_report;

static nw_path_monitor_t path_monitor;
static NSArray *power_observers;

static const char *PathStatusName(nw_path_status_t status) {
    switch (status) {
    case nw_path_status_invalid: return "invalid";
    case nw_path_status_satisfied: return "satisfied";
    case nw_path_status_unsatisfied: return "unsatisfied";
    case nw_path_status_satisfiable: return "satisfiable";
    }
    return "unknown";
}

static NSString *NetworkReport(nw_path_t path) {
    struct {
        nw_interface_type_t type;
        NSString *name;
    } types[] = {
        {nw_interface_type_wifi, @"wifi"},
        {nw_interface_type_cellular, @"cellular"},
        {nw_interface_type_wired, @"wired"},
        {nw_interface_type_loopback, @"loopback"},
        {nw_interface_type_other, @"other"},
    };
    NSMutableArray<NSString *> *interfaces = [NSMutableArray new];
    for (size_t i = 0; i < sizeof(types) / sizeof(*types); i++) {
        if (nw_path_uses_interface_type(path, types[i].type))
            [interfaces addObject:types[i].name];
    }
    return [NSString stringWithFormat:@"status: %s\ninterfaces: %@\nexpensive: %s\nconstrained: %s\n",
            PathStatusName(nw_path_get_status(path)),
            interfaces.count > 0 ? [interfaces componentsJoinedByString:@" "] : @"none",
            nw_path_is_expensive(path) ? "yes" : "no",
            nw_path_is_constrained(path) ? "yes" : "no"];
}

static const char *BatteryStateName(UIDeviceBatteryState state) {
    switch (state) {
    case UIDeviceBatteryStateUnknown: return "unknown";
    case UIDeviceBatteryStateUnplugged: return "unplugged";
    case UIDeviceBatteryStateCharging: return "charging";
    case UIDeviceBatteryStateFull: return "full";
    }
    return "unknown";
}

// The main thread's
static void UpdatePower(void) {
    UIDevice *device = UIDevice.currentDevice;
    NSString *report = [NSString stringWithFormat:@"battery: %.0f%%\nstate: %s\nlow power mode: %s\n",
                        device.batteryLevel * 100, BatteryStateName(device.batteryState),
                        NSProcessInfo.processInfo.lowPowerModeEnabled ? "on" : "off"];
    lock(&status_lock);
    power_report = report;
    unlock(&status_lock);
}

void DeviceStatusStart(void) {
    path_monitor = nw_path_monitor_create();
    nw_path_monitor_set_queue(path_monitor, dispatch_get_main_queue());
    nw_path_monitor_set_update_handler(path_monitor, ^(nw_path_t path) {
        NSString *report = NetworkReport(path);
        lock(&status_lock);
        network_report = report;
        unlock(&status_lock);
    });
    nw_path_monitor_start(path_monitor);

    // The battery's level and state are read only while it is monitored
    UIDevice.currentDevice.batteryMonitoringEnabled = YES;
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    void (^update)(NSNotification *) = ^(NSNotification *_) {
        UpdatePower();
    };
    // Low Power Mode's is posted on any thread: on the main one here
    power_observers = @[
        [center addObserverForName:UIDeviceBatteryLevelDidChangeNotification object:nil
                             queue:NSOperationQueue.mainQueue usingBlock:update],
        [center addObserverForName:UIDeviceBatteryStateDidChangeNotification object:nil
                             queue:NSOperationQueue.mainQueue usingBlock:update],
        [center addObserverForName:NSProcessInfoPowerStateDidChangeNotification object:nil
                             queue:NSOperationQueue.mainQueue usingBlock:update],
    ];
    UpdatePower();
}

static const char *OrientationName(UIInterfaceOrientation orientation) {
    switch (orientation) {
    case UIInterfaceOrientationUnknown: return "unknown";
    case UIInterfaceOrientationPortrait: return "portrait";
    case UIInterfaceOrientationPortraitUpsideDown: return "portrait upside down";
    case UIInterfaceOrientationLandscapeLeft: return "landscape left";
    case UIInterfaceOrientationLandscapeRight: return "landscape right";
    }
    return "unknown";
}

static const char *SizeClassName(UIUserInterfaceSizeClass sizeClass) {
    switch (sizeClass) {
    case UIUserInterfaceSizeClassUnspecified: return "unspecified";
    case UIUserInterfaceSizeClassCompact: return "compact";
    case UIUserInterfaceSizeClassRegular: return "regular";
    }
    return "unspecified";
}

// The main thread's: nil with no terminal window in front
static NSString *DeviceReport(void) {
    UIWindow *window = currentTerminalViewController.view.window;
    if (window == nil)
        return nil;
    char model[64];
    size_t length = sizeof(model);
    sysctlbyname("hw.machine", model, &length, NULL, 0);
    UIDevice *device = UIDevice.currentDevice;
    UITraitCollection *traits = window.traitCollection;
    return [NSString stringWithFormat:@"model: %s\nsystem: %@ %@\nwindow: %.0fx%.0f pt @%.0f\norientation: %s\n"
                                      "size classes: width %s, height %s\nappearance: %s\nexternal keyboard: %s\n",
            model, device.systemName, device.systemVersion,
            window.bounds.size.width, window.bounds.size.height, window.windowScene.screen.scale,
            OrientationName(window.windowScene.effectiveGeometry.interfaceOrientation),
            SizeClassName(traits.horizontalSizeClass), SizeClassName(traits.verticalSizeClass),
            traits.userInterfaceStyle == UIUserInterfaceStyleDark ? "dark" : "light",
            GCKeyboard.coalescedKeyboard != nil ? "yes" : "no"];
}

static lock_t device_lock = LOCK_INITIALIZER;
static cond_t device_cond = COND_INITIALIZER;

@interface DeviceReportRequest : NSObject
// Under device_lock: whether the main thread has made it, and what (nil: no
// terminal window was in front)
@property (nonatomic) BOOL made;
@property (nonatomic, nullable) NSString *report;
@end

@implementation DeviceReportRequest
@end

// On the reading program's thread: made on the main thread, which it waits
// for (a signal ends the wait, as the read's error)
int device_device_report(char **report) {
    @autoreleasepool {
        DeviceReportRequest *request = [DeviceReportRequest new];
        dispatch_async(dispatch_get_main_queue(), ^{
            NSString *made = DeviceReport();
            lock(&device_lock);
            request.report = made;
            request.made = YES;
            notify(&device_cond);
            unlock(&device_lock);
        });
        lock(&device_lock);
        while (!request.made) {
            int err = wait_for(&device_cond, &device_lock, NULL);
            if (err < 0) {
                unlock(&device_lock);
                return err;
            }
        }
        NSString *made = request.report;
        unlock(&device_lock);
        if (made == nil)
            return _EAGAIN;
        *report = strdup(made.UTF8String);
        return 0;
    }
}

static char *Copy(NSString *__strong *report) {
    @autoreleasepool {
        lock(&status_lock);
        NSString *now = *report;
        unlock(&status_lock);
        return now == nil ? NULL : strdup(now.UTF8String);
    }
}

char *device_network_report(void) {
    return Copy(&network_report);
}

char *device_power_report(void) {
    return Copy(&power_report);
}
