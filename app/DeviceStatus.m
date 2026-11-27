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

#import <UIKit/UIKit.h>
#import <Network/Network.h>
#import "DeviceStatus.h"
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
