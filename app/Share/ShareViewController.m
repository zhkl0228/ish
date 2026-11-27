//
//  ShareViewController.m
//  iSH
//
//  The share extension (Share → iSH): what another app shares, left in the
//  app group for the app to move into /root/Documents/inbox (ShareInbox.h).
//  A file (an image, a PDF, a movie) as itself, under the name it comes
//  with; a web page's link as a .txt of its URL; text as a .txt. Says what it
//  saved, and closes; what it could not save it says and leaves open.
//

#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import "AppGroup.h"
#import "ShareInbox.h"

@interface ShareViewController : UIViewController
@end

@implementation ShareViewController {
    UILabel *_label;
    UIButton *_close;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.view.backgroundColor = UIColor.systemBackgroundColor;
    _label = [UILabel new];
    _label.numberOfLines = 0;
    _label.textAlignment = NSTextAlignmentCenter;
    _label.font = [UIFont preferredFontForTextStyle:UIFontTextStyleBody];
    _label.text = @"Saving to iSH…";
    _close = [UIButton buttonWithType:UIButtonTypeSystem];
    [_close setTitle:@"Close" forState:UIControlStateNormal];
    [_close addTarget:self action:@selector(finish) forControlEvents:UIControlEventTouchUpInside];
    _close.hidden = YES;
    UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[_label, _close]];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 16;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    UILayoutGuide *margins = self.view.layoutMarginsGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.centerYAnchor constraintEqualToAnchor:self.view.centerYAnchor],
        [stack.leadingAnchor constraintEqualToAnchor:margins.leadingAnchor],
        [stack.trailingAnchor constraintEqualToAnchor:margins.trailingAnchor],
    ]];
}

- (void)viewDidAppear:(BOOL)animated {
    [super viewDidAppear:animated];
    [self saveAll];
}

- (void)finish {
    [self.extensionContext completeRequestReturningItems:@[] completionHandler:nil];
}

- (void)saveAll {
    NSURL *staging = [ContainerURL() URLByAppendingPathComponent:SHARE_INBOX_STAGING isDirectory:YES];
    NSError *error;
    if (![NSFileManager.defaultManager createDirectoryAtURL:staging withIntermediateDirectories:YES attributes:nil error:&error]) {
        [self saidSaved:@[] failed:@[[NSString stringWithFormat:@"%@ not made: %@", staging.path, error.localizedDescription]]];
        return;
    }
    NSMutableArray<NSString *> *saved = [NSMutableArray new];
    NSMutableArray<NSString *> *failed = [NSMutableArray new];
    dispatch_group_t group = dispatch_group_create();
    for (NSExtensionItem *item in self.extensionContext.inputItems) {
        for (NSItemProvider *provider in item.attachments) {
            dispatch_group_enter(group);
            [self save:provider into:staging done:^(NSString *name, NSString *problem) {
                dispatch_async(dispatch_get_main_queue(), ^{
                    if (name != nil)
                        [saved addObject:name];
                    else
                        [failed addObject:problem];
                    dispatch_group_leave(group);
                });
            }];
        }
    }
    dispatch_group_notify(group, dispatch_get_main_queue(), ^{
        if (saved.count > 0)
            CFNotificationCenterPostNotification(CFNotificationCenterGetDarwinNotifyCenter(),
                                                 CFSTR(SHARE_INBOX_NOTIFIED), NULL, NULL, true);
        [self saidSaved:saved failed:failed];
    });
}

- (void)saidSaved:(NSArray<NSString *> *)saved failed:(NSArray<NSString *> *)failed {
    NSMutableString *text = [NSMutableString new];
    if (saved.count > 0)
        [text appendFormat:@"Saved to ~/Documents/inbox:\n%@", [saved componentsJoinedByString:@"\n"]];
    if (failed.count > 0)
        [text appendFormat:@"%@Not saved:\n%@", text.length > 0 ? @"\n\n" : @"", [failed componentsJoinedByString:@"\n"]];
    _label.text = text;
    if (failed.count > 0) {
        _close.hidden = NO;
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (1.2 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [self finish];
    });
}

// done with the name it saved it as, or what kept it from being saved; on
// any queue
- (void)save:(NSItemProvider *)provider into:(NSURL *)staging
        done:(void (^)(NSString *name, NSString *problem))done {
    // A web page's link (a file's URL is a file's, below)
    if ([provider hasItemConformingToTypeIdentifier:UTTypeURL.identifier] &&
        ![provider hasItemConformingToTypeIdentifier:UTTypeFileURL.identifier]) {
        [provider loadObjectOfClass:NSURL.class completionHandler:^(NSURL *url, NSError *error) {
            if (url == nil) {
                done(nil, [NSString stringWithFormat:@"a link: %@", error.localizedDescription]);
                return;
            }
            NSString *name = [NSString stringWithFormat:@"%@.txt", url.host.length > 0 ? url.host : @"link"];
            [self write:[[url.absoluteString stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding]
                   name:name into:staging done:done];
        }];
        return;
    }
    // Text
    if ([provider hasItemConformingToTypeIdentifier:UTTypePlainText.identifier]) {
        [provider loadObjectOfClass:NSString.class completionHandler:^(NSString *string, NSError *error) {
            if (string == nil) {
                done(nil, [NSString stringWithFormat:@"text: %@", error.localizedDescription]);
                return;
            }
            [self write:[string dataUsingEncoding:NSUTF8StringEncoding] name:@"text.txt" into:staging done:done];
        }];
        return;
    }
    // A file, as the type it has first (registeredTypeIdentifiers come in
    // the order of their fidelity, the highest first)
    for (NSString *type in provider.registeredTypeIdentifiers) {
        if (![[UTType typeWithIdentifier:type] conformsToType:UTTypeData])
            continue;
        [provider loadFileRepresentationForTypeIdentifier:type completionHandler:^(NSURL *url, NSError *error) {
            if (url == nil) {
                done(nil, [NSString stringWithFormat:@"%@: %@", type, error.localizedDescription]);
                return;
            }
            // the file is there till this returns
            NSString *name = provider.suggestedName.length > 0 && url.pathExtension.length > 0
                ? [provider.suggestedName stringByAppendingPathExtension:url.pathExtension]
                : url.lastPathComponent;
            NSURL *part = [staging URLByAppendingPathComponent:[@".part-" stringByAppendingString:NSUUID.UUID.UUIDString]];
            NSError *copyError;
            if (![NSFileManager.defaultManager copyItemAtURL:url toURL:part error:&copyError]) {
                done(nil, [NSString stringWithFormat:@"%@: %@", name, copyError.localizedDescription]);
                return;
            }
            [self rename:part name:name into:staging done:done];
        }];
        return;
    }
    done(nil, [NSString stringWithFormat:@"nothing it saves (%@)", [provider.registeredTypeIdentifiers componentsJoinedByString:@", "]]);
}

- (void)write:(NSData *)data name:(NSString *)name into:(NSURL *)staging
         done:(void (^)(NSString *name, NSString *problem))done {
    NSURL *part = [staging URLByAppendingPathComponent:[@".part-" stringByAppendingString:NSUUID.UUID.UUIDString]];
    NSError *error;
    if (![data writeToURL:part options:0 error:&error]) {
        done(nil, [NSString stringWithFormat:@"%@: %@", name, error.localizedDescription]);
        return;
    }
    [self rename:part name:name into:staging done:done];
}

// part, written whole, under name where the app moves it from (a name the
// app's inbox has is renumbered as it is moved)
- (void)rename:(NSURL *)part name:(NSString *)name into:(NSURL *)staging
          done:(void (^)(NSString *name, NSString *problem))done {
    @synchronized (self) {
        NSURL *to = ShareInboxFreeURL(staging, name);
        NSError *error;
        if (![NSFileManager.defaultManager moveItemAtURL:part toURL:to error:&error]) {
            done(nil, [NSString stringWithFormat:@"%@: %@", name, error.localizedDescription]);
            return;
        }
        done(to.lastPathComponent, nil);
    }
}

@end
