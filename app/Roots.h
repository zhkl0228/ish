//
//  Roots.h
//  iSH
//
//  Created by Theodore Dubois on 6/7/20.
//

#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

@protocol ProgressReporter

- (void)updateProgress:(double)progressFraction message:(NSString *)progressMessage;
- (BOOL)shouldCancel;

@end

@interface Roots : NSObject

+ (instancetype)instance;

@property (readonly) NSOrderedSet<NSString *> *roots;
@property NSString *defaultRoot;
@property (readonly) BOOL wantsVersionFile;
- (NSURL *)rootUrl:(NSString *)name;
- (BOOL)importRootFromArchive:(NSURL *)archive name:(NSString *)name error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress;
/// Whether the default filesystem was not set up from the filesystem built into
/// this version of the app (root.tar.gz), and the user has not chosen to keep it.
@property (readonly) BOOL defaultRootIsOutdated;
/// Don't offer to reset the default filesystem again until the built-in one changes.
- (void)keepOutdatedDefaultRoot;
/// Replaces the default filesystem with the one built into the app.
- (BOOL)resetDefaultRoot:(NSError **)error;
- (BOOL)exportRootNamed:(NSString *)name toArchive:(NSURL *)archive error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress;
- (BOOL)destroyRootNamed:(NSString *)name error:(NSError **)error;
- (BOOL)renameRoot:(NSString *)name toName:(NSString *)newName error:(NSError **)error;

@end

NS_ASSUME_NONNULL_END
