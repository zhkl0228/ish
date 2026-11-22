//
//  Roots.m
//  iSH
//
//  Created by Theodore Dubois on 6/7/20.
//

#import <FileProvider/FileProvider.h>
#import "Roots.h"
#import "AppGroup.h"
#import "NSObject+SaneKVO.h"
#include "tools/fakefs.h"

static NSURL *RootsDir(void) {
    static NSURL *rootsDir;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        rootsDir = [ContainerURL() URLByAppendingPathComponent:@"roots"];
        NSFileManager *manager = [NSFileManager defaultManager];
        [manager createDirectoryAtURL:rootsDir
          withIntermediateDirectories:YES
                           attributes:@{}
                                error:nil];
    });
    return rootsDir;
}

static NSString *kDefaultRoot = @"Default Root";

// Files in a root's directory, next to data and meta.db: the version of the
// built-in filesystem the root was set up from, and the one the user chose to
// keep it over. Roots set up before these were kept have neither.
static NSString *kRootVersionFile = @"root-version";
static NSString *kKeptRootVersionFile = @"kept-root-version";

// The version of the built-in filesystem: the SHA-256 of root.tar.gz, which
// the Copy Root (or Download Root) build phase writes to root.version.
static NSString *BundledRootVersion(void) {
    static NSString *version;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        NSURL *url = [NSBundle.mainBundle URLForResource:@"root" withExtension:@"version"];
        if (url == nil) {
            NSLog(@"root.version is missing from the app bundle; not checking the default filesystem against the built-in one");
            return;
        }
        NSError *error = nil;
        NSString *contents = [NSString stringWithContentsOfURL:url encoding:NSUTF8StringEncoding error:&error];
        if (contents == nil) {
            NSLog(@"Failed to read %@: %@; not checking the default filesystem against the built-in one", url.path, error);
            return;
        }
        version = [contents stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
    });
    return version;
}

@interface Roots ()
@property NSMutableOrderedSet<NSString *> *roots;
@property BOOL updatingDomains;
@property BOOL domainsNeedUpdate;
@property BOOL wantsVersionFile;
@end

@implementation Roots

- (instancetype)init {
    if (self = [super init]) {
        NSError *error = nil;
        NSArray<NSString *> *rootNames = [NSFileManager.defaultManager contentsOfDirectoryAtPath:RootsDir().path error:&error];
        NSAssert(error == nil, @"couldn't list roots: %@", error);
        self.roots = [rootNames mutableCopy];
        if (!self.roots.count) {
            // import default root
            NSError *error;
            if (![self importBundledRootNamed:@"default" error:&error]) {
                NSAssert(NO, @"failed to import default root, error %@", error);
            }
            _wantsVersionFile = YES;
        }
        [self observe:@[@"roots"] options:0 owner:self usingBlock:^(typeof(self) self) {
            if (self.defaultRoot == nil && self.roots.count)
                self.defaultRoot = self.roots[0];
            [self syncFileProviderDomains];
        }];
        [self syncFileProviderDomains];

        if ((!self.defaultRoot || ![self.roots containsObject:self.defaultRoot]) && self.roots.count)
            self.defaultRoot = self.roots.firstObject;
    }
    return self;
}

- (NSString *)defaultRoot {
    return [NSUserDefaults.standardUserDefaults stringForKey:kDefaultRoot];
}
- (void)setDefaultRoot:(NSString *)defaultRoot {
    [NSUserDefaults.standardUserDefaults setObject:defaultRoot forKey:kDefaultRoot];
}

- (NSURL *)rootUrl:(NSString *)name {
    return [RootsDir() URLByAppendingPathComponent:name];
}

- (void)syncFileProviderDomains {
    if (self.updatingDomains) {
        self.domainsNeedUpdate = YES;
        return;
    }
    self.updatingDomains = YES;
    self.domainsNeedUpdate = NO;

    [NSFileProviderManager getDomainsWithCompletionHandler:^(NSArray<NSFileProviderDomain *> *domains, NSError *error) {
        void (^onError)(NSError *error) = ^(NSError *error) {
            if (error != nil)
                NSLog(@"error adjusting domains: %@", error);
        };
        onError(error);
        NSMutableOrderedSet<NSString *> *missingRoots = [self.roots mutableCopy];
        for (NSFileProviderDomain *domain in domains) {
            if ([missingRoots containsObject:domain.identifier]) {
                [missingRoots removeObject:domain.identifier];
            } else {
                [NSFileManager.defaultManager removeItemAtURL:
                 [NSFileProviderManager.defaultManager.documentStorageURL
                  URLByAppendingPathComponent:domain.pathRelativeToDocumentStorage]
                                                        error:nil];
                [NSFileProviderManager removeDomain:domain completionHandler:onError];
            }
        }
        for (NSString *rootId in missingRoots) {
            [NSFileProviderManager addDomain:[[NSFileProviderDomain alloc] initWithIdentifier:rootId
                                                                                  displayName:rootId
                                                                pathRelativeToDocumentStorage:rootId]
                           completionHandler:onError];
        }
        if (self.domainsNeedUpdate)
            [self syncFileProviderDomains];
        self.updatingDomains = NO;
    }];
}

- (BOOL)accessInstanceVariablesDirectly {
    return YES;
}

void root_progress_callback(void *cookie, double progress, const char *message, bool *should_cancel) {
    id <ProgressReporter> reporter = (__bridge id<ProgressReporter>) cookie;
    [reporter updateProgress:progress message:[NSString stringWithUTF8String:message]];
    if ([reporter shouldCancel])
        *should_cancel = true;
}

- (BOOL)importRootFromArchive:(NSURL *)archive name:(NSString *)name error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
    NSAssert(![self.roots containsObject:name], @"root already exists: %@", name);
    struct fakefsify_error fs_err;
    NSURL *destination = [self rootUrl:name];
    NSURL *tempDestination = [NSFileManager.defaultManager.temporaryDirectory
                              URLByAppendingPathComponent:[NSProcessInfo.processInfo globallyUniqueString]];
    if (tempDestination == nil)
        return NO;
    if (!fakefs_import(archive.fileSystemRepresentation,
                       tempDestination.fileSystemRepresentation,
                       &fs_err, (struct progress) {(__bridge void *) progress, root_progress_callback})) {
        NSString *domain = NSPOSIXErrorDomain;
        if (fs_err.type == ERR_SQLITE)
            domain = @"SQLite";
        *error = [NSError errorWithDomain:domain
                                     code:fs_err.code
                                 userInfo:@{NSLocalizedDescriptionKey:
                                                [NSString stringWithFormat:@"%s, line %d", fs_err.message, fs_err.line]}];
        if (fs_err.type == ERR_CANCELLED)
            *error = nil;
        free(fs_err.message);
        [NSFileManager.defaultManager removeItemAtURL:tempDestination error:nil];
        return NO;
    }
    if (![NSFileManager.defaultManager moveItemAtURL:tempDestination toURL:destination error:error])
        return NO;

    void (^addRoot)(void) = ^{
        [[self mutableOrderedSetValueForKey:@"roots"] addObject:name];
    };
    if (!NSThread.isMainThread)
        dispatch_sync(dispatch_get_main_queue(), addRoot);
    else
        addRoot();
    return YES;
}

- (BOOL)importBundledRootNamed:(NSString *)name error:(NSError **)error {
    if (![self importRootFromArchive:[NSBundle.mainBundle URLForResource:@"root" withExtension:@"tar.gz"]
                                name:name
                               error:error
                    progressReporter:nil])
        return NO;
    NSString *version = BundledRootVersion();
    if (version == nil)
        return YES;
    return [version writeToURL:[[self rootUrl:name] URLByAppendingPathComponent:kRootVersionFile]
                    atomically:YES
                      encoding:NSUTF8StringEncoding
                         error:error];
}

// The contents of one of a root's version files, nil if it has none.
- (nullable NSString *)versionFile:(NSString *)file ofRoot:(NSString *)name {
    NSURL *url = [[self rootUrl:name] URLByAppendingPathComponent:file];
    NSString *contents = [NSString stringWithContentsOfURL:url encoding:NSUTF8StringEncoding error:nil];
    return [contents stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet];
}

- (BOOL)defaultRootIsOutdated {
    NSString *bundled = BundledRootVersion();
    if (bundled == nil)
        return NO;
    NSString *name = self.defaultRoot;
    return ![bundled isEqualToString:[self versionFile:kRootVersionFile ofRoot:name]] &&
        ![bundled isEqualToString:[self versionFile:kKeptRootVersionFile ofRoot:name]];
}

- (void)keepOutdatedDefaultRoot {
    NSURL *url = [[self rootUrl:self.defaultRoot] URLByAppendingPathComponent:kKeptRootVersionFile];
    NSError *error = nil;
    if (![BundledRootVersion() writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:&error])
        NSLog(@"Failed to write %@: %@", url.path, error);
}

- (BOOL)resetDefaultRoot:(NSError **)error {
    NSString *name = self.defaultRoot;
    if (![NSFileManager.defaultManager removeItemAtURL:[self rootUrl:name] error:error])
        return NO;
    // The import asserts that no root of that name exists
    [[self mutableOrderedSetValueForKey:@"roots"] removeObject:name];
    if (![self importBundledRootNamed:name error:error])
        return NO;
    self.defaultRoot = name;
    return YES;
}

- (BOOL)exportRootNamed:(NSString *)name toArchive:(NSURL *)archive error:(NSError **)error progressReporter:(id<ProgressReporter> _Nullable)progress {
    NSAssert([self.roots containsObject:name], @"trying to export a root that doesn't exist: %@", name);
    struct fakefsify_error fs_err;
    if (!fakefs_export([self rootUrl:name].fileSystemRepresentation,
                       archive.fileSystemRepresentation,
                       &fs_err, (struct progress) {(__bridge void *) progress, root_progress_callback})) {
        // TODO: dedup with above method
        NSString *domain = NSPOSIXErrorDomain;
        if (fs_err.type == ERR_SQLITE)
            domain = @"SQLite";
        *error = [NSError errorWithDomain:domain
                                     code:fs_err.code
                                 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithUTF8String:fs_err.message]}];
        if (fs_err.type == ERR_CANCELLED)
            *error = nil;
        free(fs_err.message);
        return NO;
    }
    return YES;
}

- (BOOL)destroyRootNamed:(NSString *)name error:(NSError **)error {
    if ([name isEqualToString:self.defaultRoot]) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Cannot delete the default filesystem"}];
        return NO;
    }
    NSAssert([self.roots containsObject:name], @"root does not exist: %@", name);
    if (![NSFileManager.defaultManager removeItemAtURL:[self rootUrl:name] error:error])
        return NO;
    [[self mutableOrderedSetValueForKey:@"roots"] removeObject:name];
    return YES;
}

- (BOOL)renameRoot:(NSString *)name toName:(NSString *)newName error:(NSError **)error {
    if (name.length == 0) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't be empty"}];
        return NO;
    }
    if ([name containsString:@"/"]) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't contain /"}];
        return NO;
    }
    if ([name isEqualToString:@"."] || [name isEqualToString:@".."]) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Filesystem name can't be . or .."}];
        return NO;
    }
    if ([name isEqualToString:self.defaultRoot]) {
        *error = [NSError errorWithDomain:@"iSH" code:0 userInfo:@{NSLocalizedDescriptionKey: @"Cannot rename the default filesystem"}];
        return NO;
    }
    NSAssert([self.roots containsObject:name], @"root does not exist: %@", name);
    
    if (![NSFileManager.defaultManager moveItemAtURL:[self rootUrl:name] toURL:[self rootUrl:newName] error:error])
        return NO;
    NSUInteger index = [self.roots indexOfObject:name];
    [[self mutableOrderedSetValueForKey:@"roots"] replaceObjectAtIndex:index withObject:newName];
    return YES;
}

+ (instancetype)instance {
    static Roots *instance;
    static dispatch_once_t token;
    dispatch_once(&token, ^{
        instance = [Roots new];
    });
    return instance;
}

@end
