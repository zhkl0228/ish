//
//  MonitorSampler.h
//  iSH
//
//  What the meters show (MonitorStripView) and their details
//  (MonitorDetailViewController): the device's CPUs and load, the app's
//  memory and the kernel's brake on it, how hot the device runs. Read once a
//  second while one of its views has it started, each reading told
//  (MonitorSamplerDidReadNotification): the meters in the terminal's margin
//  and in the window's column right of it read one sampler, and say the same.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// What one reading found. The CPUs' shares are of their time since the
// reading before, which the first reading after start has none of: counted
// NO, and they are 0.
@interface MonitorReading : NSObject

@property (readonly) BOOL counted;
// Of all the CPUs' time: busy, which is user (with nice) and system
@property (readonly) double cpuBusy;
@property (readonly) double cpuUser;
@property (readonly) double cpuSystem;
// Each CPU's busy share, as many as the host has (get_per_cpu_usage); none
// where not counted
@property (readonly) NSArray<NSNumber *> *coreBusy;
// The device's load averages over 1, 5 and 15 minutes (vm.loadavg, as the
// guest's /proc/loadavg has them)
@property (readonly) double load1;
@property (readonly) double load5;
@property (readonly) double load15;
// The memory as the guest's free counts it (get_mem_usage): on iOS the app's
// footprint of its limit (appLimit), past which iOS ends it; with no limit
// (the simulator), the device's
@property (readonly) uint64_t memoryUsed;
@property (readonly) uint64_t memoryTotal;
@property (readonly) BOOL appLimit;
@property (readonly) double memoryFraction;
// The kernel's memory governor refuses the guest's new memory (ish_mem_braked)
@property (readonly) BOOL memoryBraked;
@property (readonly) NSProcessInfoThermalState thermalState;
// Of the blocks the guest's programs had compiled in the last
// kMonitorNativeWindow readings, the share with native code (jit_block_counts:
// an AOT image's, or the JIT's), the rest running as gadgets. Known once any
// were compiled; held while none are, as code run before is run on.
@property (readonly) BOOL nativeKnown;
@property (readonly) double nativeShare;
// The JIT's code region has filled up (jit_region_is_full): till it is
// flushed, a moment, blocks compiled run as gadgets unless an AOT image has
// them; then every block is translated again once hot
@property (readonly) BOOL codeRegionFull;
// The JIT translates (it has its code region), and what its code takes of the
// region, all of it once full, of its size (jit_region_usage): the details
// show it. All 0 while it does not.
@property (readonly) BOOL jitTranslating;
@property (readonly) uint64_t codeRegionUsed;
@property (readonly) uint64_t codeRegionSize;

@end

// A process taking the CPUs, as the details list it (MonitorDetailViewController)
@interface MonitorProcess : NSObject
@property (readonly) int pid;
// When it started, in ticks since boot: a pid used again is another process
@property (readonly) uint64_t startTicks;
@property (readonly) NSString *name;
// Its share of all the CPUs since the reading before, as the CPU's meter has
// the device's: the processes' add up to no more than it
@property (readonly) double cpuShare;
// Its memory in use (resident), bytes
@property (readonly) uint64_t resident;
@end

extern NSNotificationName const MonitorSamplerDidReadNotification;
// topProcesses counted again (wantsProcesses)
extern NSNotificationName const MonitorSamplerDidCountProcessesNotification;
// Readings in cpuHistory: a minute's
extern const NSUInteger kMonitorHistoryLength;
// Readings whose blocks the native share is of: ten seconds'
#define kMonitorNativeWindow 10

@interface MonitorSampler : NSObject

// The last reading; nil before the first
@property (readonly, nullable) MonitorReading *reading;
// The CPUs' busy share of the last counted readings, oldest first (at most
// kMonitorHistoryLength): the column's line (MonitorColumnView). Kept while
// the sampler is stopped, the time it was stopped not in it.
@property (readonly) NSArray<NSNumber *> *cpuHistory;

// While set (the details shown, the keyboard down), the three processes
// taking the most of the CPUs since the count before, off the main thread
// (fs/proc/ish.h's ish_processes): counted as it is set and a quarter of a
// second after, so that they show at once, then with each reading; nil till
// counted twice, each count told (MonitorSamplerDidCountProcessesNotification)
@property (nonatomic) BOOL wantsProcesses;
@property (readonly, nullable) NSArray<MonitorProcess *> *topProcesses;

// Reads now, then once a second, while any of its users has it started: each
// start is paired with a stop, the last stop stops it. Started again, the CPUs
// are counted from its first reading, not across the time it was stopped.
- (void)start;
- (void)stop;

@end

NS_ASSUME_NONNULL_END
