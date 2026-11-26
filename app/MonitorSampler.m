//
//  MonitorSampler.m
//  iSH
//

#import "MonitorSampler.h"
#include "platform/platform.h"
#include "kernel/mm.h"

// asbestos/guest-arm64/jit.h's (a header for builds with ISH_JIT, which libish
// is for the app: GuestARM64.xcconfig's ISH_ENABLE_JIT)
void jit_block_counts(uint64_t *blocks, uint64_t *native);
bool jit_region_is_full(void);
bool jit_region_usage(size_t *used, size_t *size);

NSNotificationName const MonitorSamplerDidReadNotification = @"MonitorSamplerDidReadNotification";
const NSUInteger kMonitorHistoryLength = 60;

// The most CPUs counted one by one
#define MAX_CORES 64

@interface MonitorReading ()
@property BOOL counted;
@property double cpuBusy;
@property double cpuUser;
@property double cpuSystem;
@property NSArray<NSNumber *> *coreBusy;
@property double load1;
@property double load5;
@property double load15;
@property uint64_t memoryUsed;
@property uint64_t memoryTotal;
@property BOOL appLimit;
@property double memoryFraction;
@property BOOL memoryBraked;
@property NSProcessInfoThermalState thermalState;
@property BOOL nativeKnown;
@property double nativeShare;
@property BOOL codeRegionFull;
@property BOOL jitTranslating;
@property uint64_t codeRegionUsed;
@property uint64_t codeRegionSize;
@end

@implementation MonitorReading
@end

// A CPU's ticks (or all of theirs) between two readings. The counters are 32
// bits (natural_t), and a difference taken in 32 bits is right across a wrap.
struct ticks_between {
    uint32_t user, system, idle;
};
static struct ticks_between TicksBetween(struct cpu_usage last, struct cpu_usage now) {
    return (struct ticks_between) {
        .user = (uint32_t) (now.user_ticks - last.user_ticks) + (uint32_t) (now.nice_ticks - last.nice_ticks),
        .system = (uint32_t) (now.system_ticks - last.system_ticks),
        .idle = (uint32_t) (now.idle_ticks - last.idle_ticks),
    };
}

@implementation MonitorSampler {
    NSTimer *_timer;
    // Its users that have it started (start and stop in pairs)
    unsigned _users;
    NSMutableArray<NSNumber *> *_cpuHistory;
    // The last reading's ticks, all the CPUs' and each one's; none counted
    // yet while _hasTicks is NO and _lastCoreCount 0
    BOOL _hasTicks;
    struct cpu_usage _lastTicks;
    struct cpu_usage _lastCores[MAX_CORES];
    unsigned _lastCoreCount;
    // The blocks compiled and of them native at the last reading (none yet
    // while _hasBlocks is NO), and each of the last kMonitorNativeWindow
    // readings' (a ring); the share they last made, held while none were
    BOOL _hasBlocks;
    uint64_t _lastBlocks, _lastNative;
    uint64_t _windowBlocks[kMonitorNativeWindow], _windowNative[kMonitorNativeWindow];
    unsigned _windowNext;
    BOOL _nativeKnown;
    double _nativeShare;
}

- (instancetype)init {
    if (self = [super init])
        _cpuHistory = [NSMutableArray new];
    return self;
}

- (void)dealloc {
    [_timer invalidate];
}

- (NSArray<NSNumber *> *)cpuHistory {
    return [_cpuHistory copy];
}

- (void)start {
    if (_users++ > 0)
        return;
    [self read];
    __weak typeof(self) weakSelf = self;
    _timer = [NSTimer timerWithTimeInterval:1 repeats:YES block:^(NSTimer *_) {
        [weakSelf read];
    }];
    _timer.tolerance = 0.1;
    // and while the terminal is scrolled (the run loop's tracking mode)
    [NSRunLoop.mainRunLoop addTimer:_timer forMode:NSRunLoopCommonModes];
}

- (void)stop {
    if (_users == 0)
        [NSException raise:NSInternalInconsistencyException format:@"MonitorSampler stopped more often than started"];
    if (--_users > 0)
        return;
    [_timer invalidate];
    _timer = nil;
    _hasTicks = NO;
    _lastCoreCount = 0;
    _hasBlocks = NO;
    // The history is kept: started again (the app back in the foreground, the
    // column shown again, the device folded or unfolded), the line goes on
    // where it was, the time it was stopped left out. Emptied here, the line
    // started over each time the app came back.
}

- (void)read {
    MonitorReading *reading = [MonitorReading new];

    // No ticks at all in a second are get_cpu_usage's zeros, host_statistics
    // having failed (which /proc/stat shows too): not counted
    struct cpu_usage ticks = get_cpu_usage();
    struct ticks_between all = TicksBetween(_lastTicks, ticks);
    uint32_t total = all.user + all.system + all.idle;
    if (_hasTicks && total == 0) {
        static BOOL logged;
        if (!logged) {
            logged = YES;
            NSLog(@"MonitorSampler: no CPU ticks in a second (get_cpu_usage: user %llu system %llu idle %llu nice %llu, "
                  @"host_statistics failed?): the CPU is not counted",
                  ticks.user_ticks, ticks.system_ticks, ticks.idle_ticks, ticks.nice_ticks);
        }
    }
    reading.counted = _hasTicks && total != 0;
    if (reading.counted) {
        reading.cpuUser = (double) all.user / total;
        reading.cpuSystem = (double) all.system / total;
        reading.cpuBusy = (double) (all.user + all.system) / total;
        [_cpuHistory addObject:@(reading.cpuBusy)];
        if (_cpuHistory.count > kMonitorHistoryLength)
            [_cpuHistory removeObjectAtIndex:0];
    }
    _lastTicks = ticks;
    _hasTicks = YES;

    // Each CPU's, the same way; the first reading has none before it
    // (_lastCoreCount 0). A CPU with no ticks in the second did no work.
    struct cpu_usage cores[MAX_CORES];
    unsigned count = get_per_cpu_usage(cores, MAX_CORES);
    NSMutableArray<NSNumber *> *coreBusy = [NSMutableArray new];
    if (count == _lastCoreCount) {
        for (unsigned i = 0; i < count; i++) {
            struct ticks_between core = TicksBetween(_lastCores[i], cores[i]);
            uint32_t coreTotal = core.user + core.system + core.idle;
            [coreBusy addObject:@(coreTotal == 0 ? 0 : (double) (core.user + core.system) / coreTotal)];
        }
    }
    memcpy(_lastCores, cores, count * sizeof(*cores));
    _lastCoreCount = count;
    reading.coreBusy = coreBusy;

    // 16-bit fixed point, as sysinfo's (/proc/loadavg)
    struct uptime_info uptime = get_uptime();
    reading.load1 = uptime.load_1m / 65536.0;
    reading.load5 = uptime.load_5m / 65536.0;
    reading.load15 = uptime.load_15m / 65536.0;

    struct mem_usage memory = get_mem_usage();
    reading.memoryUsed = memory.total - memory.free;
    reading.memoryTotal = memory.total;
    reading.appLimit = memory.app_limit;
    reading.memoryFraction = (double) reading.memoryUsed / memory.total;
    reading.memoryBraked = ish_mem_braked();
    reading.thermalState = NSProcessInfo.processInfo.thermalState;

    // The native share of the blocks compiled in the window; held while none
    // were (the code run on is what was compiled before)
    uint64_t blocks, native;
    jit_block_counts(&blocks, &native);
    if (_hasBlocks) {
        _windowBlocks[_windowNext] = blocks - _lastBlocks;
        _windowNative[_windowNext] = native - _lastNative;
        _windowNext = (_windowNext + 1) % kMonitorNativeWindow;
        uint64_t windowBlocks = 0, windowNative = 0;
        for (unsigned i = 0; i < kMonitorNativeWindow; i++) {
            windowBlocks += _windowBlocks[i];
            windowNative += _windowNative[i];
        }
        if (windowBlocks != 0) {
            _nativeKnown = YES;
            _nativeShare = (double) windowNative / windowBlocks;
        }
    }
    _lastBlocks = blocks;
    _lastNative = native;
    _hasBlocks = YES;
    reading.nativeKnown = _nativeKnown;
    reading.nativeShare = _nativeShare;
    reading.codeRegionFull = jit_region_is_full();
    size_t used, size;
    reading.jitTranslating = jit_region_usage(&used, &size);
    reading.codeRegionUsed = used;
    reading.codeRegionSize = size;

    _reading = reading;
    [NSNotificationCenter.defaultCenter postNotificationName:MonitorSamplerDidReadNotification object:self];
}

@end
