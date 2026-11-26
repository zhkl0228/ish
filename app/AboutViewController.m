//
//  AboutViewController.m
//  iSH
//
//  Created by Theodore Dubois on 9/23/18.
//

#import "AboutViewController.h"
#import "AppDelegate.h"
#import "BuiltinJIT.h"
#import "BuiltinJITViewController.h"
#import "CurrentRoot.h"
#import "AppGroup.h"
#import "Roots.h"
#import "UserPreferences.h"
#import "iOSFS.h"
#import "UIApplication+OpenURL.h"
#import "NSObject+SaneKVO.h"
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

@interface AboutViewController () <UIDocumentPickerDelegate>
@property (weak, nonatomic) IBOutlet UITableViewCell *capsLockMappingCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *themeCell;
@property (weak, nonatomic) IBOutlet UISwitch *disableDimmingSwitch;
@property (weak, nonatomic) IBOutlet UITextField *launchCommandField;
@property (weak, nonatomic) IBOutlet UITextField *bootCommandField;
@property (weak, nonatomic) IBOutlet UITextField *claudeProxyField;
@property (weak, nonatomic) IBOutlet UITableViewCell *claudeMemoryCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *removeClaudeMemoryCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *jitCell;

@property (weak, nonatomic) IBOutlet UITableViewCell *sendFeedback;
@property (weak, nonatomic) IBOutlet UITableViewCell *openGithub;
@property (weak, nonatomic) IBOutlet UITableViewCell *openFediverse;
@property (weak, nonatomic) IBOutlet UITableViewCell *openDiscord;

@property (weak, nonatomic) IBOutlet UITableViewCell *upgradeApkCell;
@property (weak, nonatomic) IBOutlet UILabel *upgradeApkLabel;
@property (weak, nonatomic) IBOutlet UIView *upgradeApkBadge;
@property (weak, nonatomic) IBOutlet UITableViewCell *exportContainerCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *resetMountsCell;
@property (weak, nonatomic) IBOutlet UITableViewCell *resetRootfsCell;

@property (weak, nonatomic) IBOutlet UILabel *versionLabel;

@property (weak, nonatomic) IBOutlet NSLayoutConstraint *saddamHussein;

// What keyboardMoved: added to the table's bottom inset
@property CGFloat keyboardInset;

@end

@implementation AboutViewController

- (void)viewDidLoad {
    [super viewDidLoad];
    [self _updateUI];
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center addObserver:self selector:@selector(keyboardMoved:) name:UIKeyboardDidShowNotification object:nil];
    [center addObserver:self selector:@selector(keyboardMoved:) name:UIKeyboardDidChangeFrameNotification object:nil];
    [center addObserver:self selector:@selector(keyboardHidden:) name:UIKeyboardWillHideNotification object:nil];
    if (self.recoveryMode) {
        self.includeDebugPanel = YES;
        self.navigationItem.title = @"Recovery Mode";
        self.navigationItem.rightBarButtonItem = [[UIBarButtonItem alloc] initWithTitle:@"Exit"
                                                                                  style:UIBarButtonItemStyleDone
                                                                                 target:self
                                                                                 action:@selector(exitRecovery:)];
        self.navigationItem.leftBarButtonItem = nil;
    }
    _versionLabel.text = [NSString stringWithFormat:@"iSH %@ (Build %@)",
                          [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleShortVersionString"],
                          [[NSBundle mainBundle] objectForInfoDictionaryKey:@"CFBundleVersion"]];

    [UserPreferences.shared observe:@[@"capsLockMapping", @"fontSize", @"launchCommand", @"bootCommand", @"claudeProxy", @"claudeMemory"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateUI];
        });
    }];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(_updateUI:) name:FsUpdatedNotification object:nil];
    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(_updateUI:) name:BuiltinJITChangedNotification object:nil];
}

- (void)viewWillAppear:(BOOL)animated {
    [super viewWillAppear:animated];
    [self _updateUI];
}

- (void)updateViewConstraints {
    self.saddamHussein.constant = UIEdgeInsetsInsetRect(self.tableView.frame, self.tableView.adjustedContentInset).size.height;
    [super updateViewConstraints];
}

- (IBAction)dismiss:(id)sender {
    [self dismissViewControllerAnimated:self completion:nil];
}

- (void)exitRecovery:(id)sender {
    [NSUserDefaults.standardUserDefaults setBool:NO forKey:@"recovery"];
    exit(0);
}

- (void)_updateUI:(NSNotification *)notification {
    [self _updateUI];
}

- (void)_updateUI {
    NSAssert(NSThread.isMainThread, @"This method needs to be called on the main thread");
    self.disableDimmingSwitch.on = UserPreferences.shared.shouldDisableDimming;
    self.launchCommandField.text = [UserPreferences.shared.launchCommand componentsJoinedByString:@" "];
    self.bootCommandField.text = [UserPreferences.shared.bootCommand componentsJoinedByString:@" "];
    self.claudeProxyField.text = UserPreferences.shared.claudeProxy;
    self.claudeMemoryCell.detailTextLabel.text = UserPreferences.shared.claudeMemory[@"name"] ?: @"Not imported";
    self.removeClaudeMemoryCell.hidden = UserPreferences.shared.claudeMemory == nil;
    self.jitCell.detailTextLabel.text = BuiltinJIT.shared.translating ? @"Translating" : BuiltinJIT.shared.status;

    self.upgradeApkCell.userInteractionEnabled = FsNeedsRepositoryUpdate();
    self.upgradeApkLabel.enabled = FsNeedsRepositoryUpdate();
    self.upgradeApkBadge.hidden = !FsNeedsRepositoryUpdate();
    self.upgradeApkCell.hidden = !FsIsManaged();
    [self.tableView reloadData];
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [tableView cellForRowAtIndexPath:indexPath];
    if (cell == self.sendFeedback) {
        [UIApplication openURL:@"mailto:tblodt@icloud.com?subject=Feedback%20for%20iSH"];
    } else if (cell == self.openGithub) {
        [UIApplication openURL:@"https://github.com/zhkl0228/ish"];
    } else if (cell == self.openFediverse) {
        [UIApplication openURL:@"https://publ.ish.app/ish"];
    } else if (cell == self.openDiscord) {
        [UIApplication openURL:@"https://discord.gg/HFAXj44"];
    } else if (cell == self.claudeMemoryCell) {
        [self importClaudeMemory];
    } else if (cell == self.removeClaudeMemoryCell) {
        [self confirmRemoveClaudeMemory];
    } else if (cell == self.jitCell) {
        [self.navigationController pushViewController:[BuiltinJITViewController new] animated:YES];
    } else if (cell == self.exportContainerCell) {
        // copy the files to the app container so they can be extracted from iTunes file sharing
        NSURL *container = ContainerURL();
        NSURL *documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask][0];
        [NSFileManager.defaultManager removeItemAtURL:[documents URLByAppendingPathComponent:@"roots copy"] error:nil];
        [NSFileManager.defaultManager copyItemAtURL:[container URLByAppendingPathComponent:@"roots"]
                                              toURL:[documents URLByAppendingPathComponent:@"roots copy"]
                                              error:nil];
    } else if (cell == self.resetMountsCell) {
        iosfs_clear_all_bookmarks();
    } else if (cell == self.resetRootfsCell) {
        [self confirmResetRootfs];
    }
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
}

// Upgrade Repositories only applies to upstream's roots, whose repositories
// iSH keeps (/ish/version); for any other filesystem, the built-in arm64 one
// included, the row is hidden rather than left disabled without a reason.
- (CGFloat)tableView:(UITableView *)tableView heightForRowAtIndexPath:(NSIndexPath *)indexPath {
    UITableViewCell *cell = [super tableView:tableView cellForRowAtIndexPath:indexPath];
    if (cell == self.upgradeApkCell && !FsIsManaged())
        return 0;
    if (cell == self.removeClaudeMemoryCell && UserPreferences.shared.claudeMemory == nil)
        return 0;
    return [super tableView:tableView heightForRowAtIndexPath:indexPath];
}

// The section of Filesystems, Upgrade Repositories and Reset Rootfs, as
// About.storyboard orders them: after the general one and the JIT's
static const NSInteger kFilesystemsSection = 2;

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    if (section == kFilesystemsSection) {
        // Nothing to say about a filesystem whose repositories iSH does not
        // keep: its Upgrade Repositories row is hidden (heightForRow above).
        if (!FsIsManaged()) {
            return nil;
        } else if (!FsNeedsRepositoryUpdate()) {
            return [NSString stringWithFormat:@"The current filesystem is using %s, which is the latest version.", CURRENT_APK_VERSION_STRING];
        } else {
            return [NSString stringWithFormat:@"An upgrade to %s is available.", CURRENT_APK_VERSION_STRING];
        }
    }
    return [super tableView:tableView titleForFooterInSection:section];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    NSInteger sections = [super numberOfSectionsInTableView:tableView];
    if (!self.includeDebugPanel)
        sections--;
    return sections;
}

- (IBAction)disableDimmingChanged:(id)sender {
    UserPreferences.shared.shouldDisableDimming = self.disableDimmingSwitch.on;
}

// UITableViewController keeps the field being edited above the keyboard, but
// not presented as a form sheet on a large screen (an unfolded foldable, an
// iPad): the keyboard covered the rows at the bottom, the Claude proxy among
// them. What of the table the keyboard still covers is added to its bottom
// inset, and the field is scrolled into view.
- (void)keyboardMoved:(NSNotification *)notification {
    UIWindow *window = self.view.window;
    if (window == nil)
        return;
    CGRect keyboard = [window convertRect:[notification.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue]
                      fromCoordinateSpace:window.screen.coordinateSpace];
    CGRect table = [self.tableView convertRect:self.tableView.bounds toView:nil];
    CGFloat covered = CGRectGetHeight(CGRectIntersection(table, keyboard));
    // the bottom inset without this one's: the safe area's, and the table
    // controller's own for the keyboard when it gives one
    CGFloat others = self.tableView.adjustedContentInset.bottom - self.keyboardInset;
    CGFloat inset = MAX(0, covered - others);
    UIEdgeInsets contentInset = self.tableView.contentInset;
    contentInset.bottom += inset - self.keyboardInset;
    self.tableView.contentInset = contentInset;
    self.keyboardInset = inset;
    for (UITextField *field in @[self.claudeProxyField, self.launchCommandField, self.bootCommandField]) {
        if (field.isFirstResponder)
            [self.tableView scrollRectToVisible:[self.tableView convertRect:field.bounds fromView:field] animated:YES];
    }
}

- (void)keyboardHidden:(NSNotification *)notification {
    UIEdgeInsets contentInset = self.tableView.contentInset;
    contentInset.bottom -= self.keyboardInset;
    self.tableView.contentInset = contentInset;
    self.keyboardInset = 0;
}

- (IBAction)textBoxSubmit:(id)sender {
    [sender resignFirstResponder];
}

- (IBAction)launchCommandChanged:(id)sender {
    UserPreferences.shared.launchCommand = [self.launchCommandField.text componentsSeparatedByString:@" "];
}

- (IBAction)bootCommandChanged:(id)sender {
    UserPreferences.shared.bootCommand = [self.bootCommandField.text componentsSeparatedByString:@" "];
}

- (IBAction)claudeProxyChanged:(id)sender {
    UserPreferences.shared.claudeProxy = self.claudeProxyField.text;
}

// A CLAUDE.md is text, of a few KB: the import is stored base64 in the
// defaults, which stop keeping a value past 4 MB
static const long long kClaudeMemoryMaxBytes = 1024 * 1024;

- (void)importClaudeMemory {
    // a copy, as an import; markdown, which has no constant, by its extension
    // (nil only for an extension that is no valid one)
    UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc]
        initForOpeningContentTypes:@[UTTypeText, [UTType typeWithFilenameExtension:@"md"]] asCopy:YES];
    picker.delegate = self;
    [self presentViewController:picker animated:YES completion:nil];
}

- (void)showImportFailure:(NSString *)message {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Import Failed"
                                                                   message:message
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSURL *url = urls.firstObject;
    NSNumber *size;
    [url getResourceValue:&size forKey:NSURLFileSizeKey error:nil];
    if (size.longLongValue > kClaudeMemoryMaxBytes) {
        [self showImportFailure:[NSString stringWithFormat:@"%@ is %@; a CLAUDE.md of at most %@ can be imported.",
                                 url.lastPathComponent,
                                 [NSByteCountFormatter stringFromByteCount:size.longLongValue countStyle:NSByteCountFormatterCountStyleFile],
                                 [NSByteCountFormatter stringFromByteCount:kClaudeMemoryMaxBytes countStyle:NSByteCountFormatterCountStyleFile]]];
        return;
    }
    NSData *data = [NSData dataWithContentsOfURL:url];
    if (data == nil) {
        [self showImportFailure:@"The selected file could not be read."];
        return;
    }
    UserPreferences.shared.claudeMemory = @{
        @"name": url.lastPathComponent,
        @"content": [data base64EncodedStringWithOptions:0],
    };
}

- (void)confirmRemoveClaudeMemory {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Remove Claude Global Memory"
                                                                   message:@"The imported file will be removed. An unmodified guest copy will be removed the next time a shell starts."
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Remove" style:UIAlertActionStyleDestructive handler:^(UIAlertAction *_) {
        UserPreferences.shared.claudeMemory = nil;
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)confirmResetRootfs {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Reset Rootfs"
                                                                   message:@"This will replace the current filesystem with the built-in one. Your files in /root and /home are kept; everything else is lost, including the packages you installed."
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Reset"
                                              style:UIAlertActionStyleDestructive
                                            handler:^(UIAlertAction *_) {
        [self performResetRootfs];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)performResetRootfs {
    [AppDelegate resetDefaultRootAndQuitOnViewController:self];
}

@end
