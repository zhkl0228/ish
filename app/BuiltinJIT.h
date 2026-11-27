//
//  BuiltinJIT.h
//  iSH
//
//  The JIT, enabled from within the app, with no other app or computer at
//  each start: a loopback tunnel to the device's own services
//  (app/LoopbackTunnel, unless one is up already, such as LocalDevVPN's) and a
//  helper extension (app/JITHelper) that attaches StikJIT's debugger to iSH,
//  gives it the code region (asbestos/guest-arm64/jit.c, jit_region_now()),
//  and lets it go. iSH runs from its start as it does without (AOT images and
//  gadgets); blocks compiled once the region is there are translated.
//
//  Once, beforehand: a pairing file of the device (imported here, made on a
//  computer with idevice_pair), Developer Mode, and iSH signed with
//  get-task-allow (a development profile) and the Network Extensions
//  capability.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

// Posted on the main thread when its status, log or settings change
extern NSNotificationName const BuiltinJITChangedNotification;

@interface BuiltinJIT : NSObject

+ (instancetype)shared;

// What it is doing, or what came of the last attempt: one line
@property (readonly) NSString *status;
// The last attempt's steps, and what the helper's StikJIT said
@property (readonly) NSArray<NSString *> *log;
@property (readonly) BOOL busy;
// Whether the JIT translates now (it has its code region)
@property (readonly) BOOL translating;

// Enabled as iSH starts (Settings)
@property (nonatomic) BOOL enabledAtStart;

// The size of the code region the JIT asks for, in MB (Settings), and the
// sizes to choose from. It applies when the region is made: under TXM the
// debugger makes it once, as the JIT is first enabled, so a new size waits
// for iSH's next start once the JIT translates. Each MB of it is memory the
// app takes as the region is made (the debugger writes in every page).
@property (class, nonatomic) NSUInteger codeRegionMB;
@property (class, readonly) NSArray<NSNumber *> *codeRegionChoices;
// Gives the JIT the size set, before anything starts it (AppDelegate, first)
+ (void)applyCodeRegionSize;
// The size of the region the JIT has now, in MB; 0 if it has none
@property (readonly) NSUInteger codeRegionMBNow;

// Documents/StikJIT/pairingFile.plist, which Finder's file sharing reaches too
@property (readonly) NSURL *pairingFileURL;
@property (readonly) BOOL hasPairingFile;
// Copies it in, replacing one there
- (BOOL)importPairingFileFromURL:(NSURL *)url error:(NSError **)error;
// Writes one made on the device (OnDevicePairing), replacing one there
- (BOOL)savePairingFile:(NSData *)data error:(NSError **)error;

// Enables the JIT now, if it does not translate already
- (void)enable;
// At the start, if enabledAtStart
- (void)startIfEnabled;
// Reaches the device's services and mounts the developer disk image (which
// enable does too), to see that all is there
- (void)prepareDevice;
// Removes the helper's copy of the developer disk image (downloaded again
// when it is next needed)
- (void)resetDeveloperDiskImage;

@end

NS_ASSUME_NONNULL_END
