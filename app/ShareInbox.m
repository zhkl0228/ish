//
//  ShareInbox.m
//  iSH
//

#import <UIKit/UIKit.h>
#import "ShareInbox.h"
#import "AppGroup.h"
#include "debug.h"

// The main thread's: what the extension left, moved into Documents/inbox
static void MoveStaged(void) {
    NSFileManager *files = NSFileManager.defaultManager;
    NSURL *staging = [ContainerURL() URLByAppendingPathComponent:SHARE_INBOX_STAGING isDirectory:YES];
    NSError *error;
    NSArray<NSURL *> *items = [files contentsOfDirectoryAtURL:staging includingPropertiesForKeys:nil
                                                      options:NSDirectoryEnumerationSkipsHiddenFiles error:&error];
    // The extension makes the folder as it first saves something
    if (items == nil && [error.domain isEqualToString:NSCocoaErrorDomain] && error.code == NSFileReadNoSuchFileError)
        return;
    if (items == nil) {
        printk("share inbox: %s not read: %s\n", staging.path.UTF8String, error.description.UTF8String);
        return;
    }
    if (items.count == 0)
        return;
    NSURL *documents = [files URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
    NSURL *inbox = [documents URLByAppendingPathComponent:@"inbox" isDirectory:YES];
    if (![files createDirectoryAtURL:inbox withIntermediateDirectories:YES attributes:nil error:&error]) {
        printk("share inbox: %s not made: %s\n", inbox.path.UTF8String, error.description.UTF8String);
        return;
    }
    for (NSURL *item in items) {
        NSURL *to = ShareInboxFreeURL(inbox, item.lastPathComponent);
        if (![files moveItemAtURL:item toURL:to error:&error])
            printk("share inbox: %s not moved to %s: %s\n", item.path.UTF8String, to.path.UTF8String,
                   error.description.UTF8String);
    }
}

static void Notified(CFNotificationCenterRef center, void *observer, CFNotificationName name,
                     const void *object, CFDictionaryRef userInfo) {
    dispatch_async(dispatch_get_main_queue(), ^{
        MoveStaged();
    });
}

void ShareInboxStart(void) {
    CFNotificationCenterAddObserver(CFNotificationCenterGetDarwinNotifyCenter(), NULL, Notified,
                                    CFSTR(SHARE_INBOX_NOTIFIED), NULL,
                                    CFNotificationSuspensionBehaviorDeliverImmediately);
    [NSNotificationCenter.defaultCenter addObserverForName:UIApplicationDidBecomeActiveNotification object:nil
                                                     queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *_) {
        MoveStaged();
    }];
    MoveStaged();
}
