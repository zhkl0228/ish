//
//  OnDevicePairing.h
//  iSH
//
//  The device's pairing file, made on the device, with no computer: from iOS
//  27 a device pairs with a host it finds advertised on the network
//  (_remotepairing-pairable-host._tcp), as with a Mac. iSH advertises itself
//  as one, the user taps "Pair with iSH" in Settings › Privacy & Security ›
//  Developer Mode and types the code iSH shows, and the pairing file that
//  comes of it is the built-in JIT's (BuiltinJIT). idevice's pairing, which
//  app/idevice/build.sh builds (pairable_host_prepare, pairable_host_accept_fd).
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@interface OnDevicePairing : NSObject

// iOS 27 or later
@property (class, readonly) BOOL available;

// The background task that keeps iSH running while the user is in Settings
// (BGContinuedProcessingTask, iOS 26): registered as the app starts
+ (void)registerBackgroundTask;

// Advertises iSH until the device connects, then pairs. On the main thread:
// onPIN with the code to show, then done once, with the pairing file's bytes,
// or why there is none (both nil: cancelled).
- (void)startWithPIN:(void (^)(NSString *pin))onPIN
                done:(void (^)(NSData *_Nullable pairingFile, NSString *_Nullable failure))done;
- (void)cancel;

@end

NS_ASSUME_NONNULL_END
