//
//  ViewController.m
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import "TerminalViewController.h"
#import "AppDelegate.h"
#import "TerminalView.h"
#import "TerminalSession.h"
#import "TabBarView.h"
#import "MainMenu.h"
#import "BackgroundKeepAlive.h"
#import "SceneDelegate.h"
#import "BarButton.h"
#import "UserPreferences.h"
#import "AboutViewController.h"
#import "CurrentRoot.h"
#import "NSObject+SaneKVO.h"
#import "LinuxInterop.h"
#import "ClipboardStore.h"
#import "ClipboardShelfView.h"
#import "ClipboardStackView.h"
#import "ClipboardSettingsViewController.h"
#import "ClipboardUI.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "kernel/calls.h"
#include "kernel/signal.h"
#include "fs/devices.h"


@interface TerminalViewController () <UIGestureRecognizerDelegate, TabBarViewDelegate, ClipboardShelfDelegate>

@property UITapGestureRecognizer *tapRecognizer;
@property (weak, nonatomic) IBOutlet TerminalView *termView;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *bottomConstraint;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *termTop;
@property TabBarView *tabBar;

@property (weak, nonatomic) IBOutlet UIButton *tabKey;
@property (weak, nonatomic) IBOutlet UIButton *controlKey;
@property (weak, nonatomic) IBOutlet UIButton *escapeKey;
// Repeats the arrow key being held down.
@property NSTimer *arrowRepeat;
@property (strong, nonatomic) IBOutletCollection(id) NSArray *barButtons;

@property (weak, nonatomic) IBOutlet UIInputView *barView;
@property (weak, nonatomic) IBOutlet UIStackView *bar;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barButtonWidth;
@property (weak, nonatomic) IBOutlet NSLayoutConstraint *barHeight;

// The paste key's place in the row: iOS's paste button lies over it
// (updatePasteControl), in the key appearance its colors are for.
@property (weak, nonatomic) IBOutlet BarButton *pasteButton;
@property (nonatomic, nullable) UIPasteControl *pasteControl;
@property (nonatomic) UIKeyboardAppearance pasteControlAppearance;

// Sessions shown in this window, in tab order. All of them are also in the TerminalSessionStore.
@property NSMutableArray<TerminalSession *> *tabs;
@property (nonatomic) TerminalSession *selectedSession;
// The selected tab's terminal (self.terminal differs from this while a console is shown).
@property (readonly) Terminal *sessionTerminal;

// The clipboard manager, created the first time it is shown.
@property (nullable) ClipboardShelfView *shelf;
@property BOOL shelfShown;
@property (nullable) ClipboardStackView *stackView;

@property BOOL ignoreKeyboardMotion;
// The last keyboard notice not yet acted on, and whether a turn of the run
// loop is to act on it (keyboardDidSomething:)
@property (nullable) NSNotification *pendingKeyboardNotice;
@property BOOL keyboardNoticeScheduled;
@property (nonatomic) BOOL hasExternalKeyboard;

@end

// The bar's arrow keys carry their direction in the storyboard tag:
// 1 up, 2 down, 3 left, 4 right. These are the CSI final bytes.
static const char arrowKeyFinals[] = {[1] = 'A', [2] = 'B', [3] = 'D', [4] = 'C'};

// An arrow key's face: a filled triangle pointing its way, narrower than an
// equilateral one, `base` across and `base` * 14 / 11 from base to tip. nil
// for a key with another tag.
static UIImage *_Nullable arrowKeyImage(NSInteger tag, CGFloat base) {
    if (tag < 1 || tag > 4)
        return nil;
    CGFloat length = round(base * 14 / 11);
    bool vertical = tag <= 2;
    CGSize size = vertical ? CGSizeMake(base, length) : CGSizeMake(length, base);
    CGFloat w = size.width, h = size.height;
    CGPoint corners[5][3] = {
        [1] = {{w / 2, 0}, {w, h}, {0, h}},
        [2] = {{0, 0}, {w, 0}, {w / 2, h}},
        [3] = {{0, h / 2}, {w, 0}, {w, h}},
        [4] = {{0, 0}, {w, h / 2}, {0, h}},
    };
    UIBezierPath *path = [UIBezierPath bezierPath];
    [path moveToPoint:corners[tag][0]];
    [path addLineToPoint:corners[tag][1]];
    [path addLineToPoint:corners[tag][2]];
    [path closePath];
    UIGraphicsImageRenderer *renderer = [[UIGraphicsImageRenderer alloc] initWithSize:size];
    UIImage *image = [renderer imageWithActions:^(UIGraphicsImageRendererContext *_) {
        [UIColor.blackColor setFill];
        [path fill];
    }];
    // drawn in the key's tint, white or black with the keyboard
    return [image imageWithRenderingMode:UIImageRenderingModeAlwaysTemplate];
}

@implementation TerminalViewController

- (void)viewDidLoad {
    [super viewDidLoad];

#if !ISH_LINUX
    int bootError = [AppDelegate bootError];
    if (bootError < 0) {
        NSString *message = [NSString stringWithFormat:@"could not boot"];
        NSString *subtitle = [NSString stringWithFormat:@"error code %d", bootError];
        if (bootError == _EINVAL)
            subtitle = [subtitle stringByAppendingString:@"\n(try reinstalling the app, see release notes for details)"];
        [self showMessage:message subtitle:subtitle];
        NSLog(@"boot failed with code %d", bootError);
    }
#endif

    self.tabBar = [[TabBarView alloc] initWithFrame:CGRectZero];
    self.tabBar.delegate = self;
    self.tabBar.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:self.tabBar];
    self.termTop.active = NO;
    [NSLayoutConstraint activateConstraints:@[
        [self.tabBar.topAnchor constraintEqualToAnchor:self.view.topAnchor],
        [self.tabBar.leadingAnchor constraintEqualToAnchor:self.view.leadingAnchor],
        [self.tabBar.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        [self.termView.topAnchor constraintEqualToAnchor:self.tabBar.bottomAnchor],
    ]];
    [self tabsDidChange];

    self.terminal = self.terminal;
    [self.termView becomeFirstResponder];

    NSNotificationCenter *center = [NSNotificationCenter defaultCenter];
    [center addObserver:self
               selector:@selector(keyboardDidSomething:)
                   name:UIKeyboardWillChangeFrameNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(keyboardDidSomething:)
                   name:UIKeyboardDidChangeFrameNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(sceneDidActivate:)
                   name:UISceneDidActivateNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(_updateBadge)
                   name:FsUpdatedNotification
                 object:nil];


    [self _updateStyleFromPreferences:NO];
    
    // Two fingers across the terminal: the next tab, or the previous one
    for (NSNumber *direction in @[@(UISwipeGestureRecognizerDirectionLeft), @(UISwipeGestureRecognizerDirectionRight)]) {
        UISwipeGestureRecognizer *swipe = [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(swipedAcrossTerminal:)];
        swipe.direction = direction.unsignedIntegerValue;
        swipe.numberOfTouchesRequired = 2;
        swipe.delegate = self;
        [self.termView addGestureRecognizer:swipe];
    }

    self.barView.accessibilityCustomActions = @[[[UIAccessibilityCustomAction alloc] initWithName:@"Hide Keyboard" actionHandler:^BOOL(UIAccessibilityCustomAction *action) {
        return [self.termView resignFirstResponder];
    }]];
    // The bar is a row of flat keys, a point apart: esc, ctrl and tab from the
    // left edge, the four arrows and paste to the right one, the room a wide
    // screen leaves between them (the spacer, in the storyboard). 42 points
    // tall on a phone (50 on an iPad) and about as wide (resizeBar). esc and
    // ctrl are named in lowercase; tab, paste and the arrows are drawn.
    bool phone = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone;
    self.barHeight.constant = phone ? 42 : 50;
    CGFloat keyFontSize = phone ? 18 : 21;

    // A key is 39 points wide on a 320-point screen: a name shrinks to fit.
    NSDictionary<NSString *, UIButton *> *namedKeys = @{
        @"esc": self.escapeKey,
        @"ctrl": self.controlKey,
    };
    [namedKeys enumerateKeysAndObjectsUsingBlock:^(NSString *name, UIButton *key, BOOL *_) {
        [key setTitle:name forState:UIControlStateNormal];
        key.titleLabel.font = [UIFont systemFontOfSize:keyFontSize weight:UIFontWeightRegular];
        key.titleLabel.adjustsFontSizeToFitWidth = YES;
        key.titleLabel.minimumScaleFactor = 0.5;
    }];

    UIImageSymbolConfiguration *keySymbol =
        [UIImageSymbolConfiguration configurationWithPointSize:keyFontSize weight:UIImageSymbolWeightLight];
    NSDictionary<NSString *, UIButton *> *symbolKeys = @{
        @"arrow.right.to.line": self.tabKey,
    };
    [symbolKeys enumerateKeysAndObjectsUsingBlock:^(NSString *symbol, UIButton *key, BOOL *_) {
        [key setTitle:nil forState:UIControlStateNormal];
        [key setImage:[UIImage systemImageNamed:symbol] forState:UIControlStateNormal];
        [key setPreferredSymbolConfiguration:keySymbol forImageInState:UIControlStateNormal];
    }];
    for (UIButton *button in self.barButtons) {
        UIImage *arrow = arrowKeyImage(button.tag, round(keyFontSize * 11 / 18));
        if (arrow != nil) {
            [button setTitle:nil forState:UIControlStateNormal];
            [button setImage:arrow forState:UIControlStateNormal];
        }
    }
    
    [UserPreferences.shared observe:@[@"hideStatusBar"] options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self setNeedsStatusBarAppearanceUpdate];
        });
    }];
    [UserPreferences.shared observe:@[@"colorScheme", @"theme", @"hideExtraKeysWithExternalKeyboard"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateStyleFromPreferences:YES];
        });
    }];
    [self _updateBadge];

    __weak typeof(self) weakSelf = self;
    self.termView.pasteInterceptor = ^BOOL {
        return [weakSelf pasteFromStack];
    };
    // The paste key is iOS's paste button laid over it (updatePasteControl),
    // which pastes through pasteItemProviders:; the key only holds its place.
    self.pasteConfiguration = [[UIPasteConfiguration alloc] initWithTypeIdentifiersForAcceptingClass:NSString.class];
    self.pasteButton.isAccessibilityElement = NO;
    [center addObserver:self selector:@selector(pasteStackDidChange:) name:ClipboardStackDidChangeNotification object:nil];
    [self pasteStackDidChange:nil];
}

- (void)awakeFromNib {
    [super awakeFromNib];
    self.tabs = [NSMutableArray new];
    // Copies made in iSH are labeled with the tab they came from.
    ClipboardStore.shared.currentTabTitle = ^NSString *{
        TerminalViewController *current = currentTerminalViewController;
        if (current == nil)
            current = ConnectedTerminalViewControllers().firstObject;
        return current.selectedSession.displayTitle;
    };
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(sessionDidChange:)
                                               name:TerminalSessionDidChangeNotification
                                             object:nil];
#if ISH_LINUX
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(kernelPanicked:)
                                               name:KernelPanicNotification
                                             object:nil];
#endif
}

- (void)viewDidAppear:(BOOL)animated {
    [AppDelegate maybeOfferRootResetOnViewController:self];
    [super viewDidAppear:animated];
}

- (void)startNewSession {
    [self startNewSessionAtIndex:self.tabs.count];
}

// Opens a shell in a new tab at the given position and selects it. Returns nil (after
// telling the user) if the shell could not be started.
- (TerminalSession *)startNewSessionAtIndex:(NSUInteger)index {
    int err = 0;
    TerminalSession *session = [TerminalSessionStore.shared startSessionWithError:&err];
    if (session == nil) {
        [self showMessage:@"could not start session"
                 subtitle:[NSString stringWithFormat:@"error code %d", err]];
        return nil;
    }
    [self.tabs insertObject:session atIndex:MIN(index, self.tabs.count)];
    self.selectedSession = session;
    return session;
}

- (void)reconnectSessionsFromTerminalUUIDs:(NSArray<NSUUID *> *)uuids selected:(NSUUID *)selected {
    TerminalSessionStore *store = TerminalSessionStore.shared;
    TerminalSession *toSelect = nil;
    for (NSUUID *uuid in uuids) {
        TerminalSession *session = [store sessionWithUUID:uuid];
        if (session == nil || [self.tabs containsObject:session])
            continue;
        [self.tabs addObject:session];
        if ([uuid isEqual:selected])
            toSelect = session;
    }
    if (self.tabs.count == 0) {
        [self startNewSession];
        return;
    }
    self.selectedSession = toSelect ?: self.tabs.firstObject;
}

#pragma mark Saved tabs

// The tabs saved for the app's next start: their order, names and directories
// in the defaults ({"uuid", "title", "directory", "selected"}, the windows' one
// after another), and what each showed (term.js's serialize) in a file of
// Application Support/Saved Tabs named by its uuid
static NSString *const kSavedTabsKey = @"Saved Tabs";

static NSURL *SavedTabsDirectory(void) {
    NSURL *support = [NSFileManager.defaultManager URLsForDirectory:NSApplicationSupportDirectory
                                                          inDomains:NSUserDomainMask].firstObject;
    return [support URLByAppendingPathComponent:@"Saved Tabs" isDirectory:YES];
}

static NSURL *SavedTabOutputURL(NSString *uuid) {
    return [SavedTabsDirectory() URLByAppendingPathComponent:[uuid stringByAppendingPathExtension:@"txt"]];
}

+ (void)saveTabs {
    NSMutableArray<NSDictionary *> *saved = [NSMutableArray new];
    NSMutableArray<TerminalSession *> *sessions = [NSMutableArray new];
    for (TerminalViewController *vc in ConnectedTerminalViewControllers()) {
        for (TerminalSession *session in vc.tabs) {
            NSMutableDictionary *tab = [NSMutableDictionary dictionaryWithObject:session.uuid.UUIDString forKey:@"uuid"];
            tab[@"title"] = session.title;
            tab[@"directory"] = session.currentDirectory;
            if (session == vc.selectedSession)
                tab[@"selected"] = @YES;
            [saved addObject:tab];
            [sessions addObject:session];
        }
    }
    [NSUserDefaults.standardUserDefaults setObject:saved forKey:kSavedTabsKey];

    // What tabs closed since showed goes
    NSFileManager *manager = NSFileManager.defaultManager;
    NSURL *directory = SavedTabsDirectory();
    [manager createDirectoryAtURL:directory withIntermediateDirectories:YES attributes:nil error:nil];
    NSMutableSet<NSString *> *names = [NSMutableSet new];
    for (TerminalSession *session in sessions)
        [names addObject:SavedTabOutputURL(session.uuid.UUIDString).lastPathComponent];
    for (NSURL *file in [manager contentsOfDirectoryAtURL:directory includingPropertiesForKeys:nil options:0 error:nil]) {
        if (![names containsObject:file.lastPathComponent])
            [manager removeItemAtURL:file error:nil];
    }

    // Each terminal's page has its text; asking takes a moment, which the app
    // is given in the background until the last page answers
    if (sessions.count == 0)
        return;
    __block NSUInteger pending = sessions.count;
    __block UIBackgroundTaskIdentifier task = UIBackgroundTaskInvalid;
    void (^end)(void) = ^{
        if (task != UIBackgroundTaskInvalid) {
            [UIApplication.sharedApplication endBackgroundTask:task];
            task = UIBackgroundTaskInvalid;
        }
    };
    task = [UIApplication.sharedApplication beginBackgroundTaskWithName:@"save tabs" expirationHandler:end];
    for (TerminalSession *session in sessions) {
        NSURL *url = SavedTabOutputURL(session.uuid.UUIDString);
        [session.terminal.webView evaluateJavaScript:@"exports.serialize()" completionHandler:^(NSString *text, NSError *error) {
            if (error != nil)
                NSLog(@"saving what tab %@ showed: %@", session.uuid, error);
            else
                [[text dataUsingEncoding:NSUTF8StringEncoding] writeToURL:url atomically:YES];
            if (--pending == 0)
                end();
        }];
    }
}

- (BOOL)restoreSavedTabs {
    NSArray<NSDictionary *> *saved = [NSUserDefaults.standardUserDefaults arrayForKey:kSavedTabsKey];
    if (saved.count == 0)
        return NO;
    [NSUserDefaults.standardUserDefaults removeObjectForKey:kSavedTabsKey];
    NSData *mark = [@"\r\n\x1b[2m--- restored ---\x1b[0m\r\n" dataUsingEncoding:NSUTF8StringEncoding];
    TerminalSession *toSelect = nil;
    for (NSDictionary *tab in saved) {
        NSString *uuid = tab[@"uuid"];
        NSMutableData *output = [NSMutableData dataWithContentsOfURL:SavedTabOutputURL(uuid)];
        [output appendData:mark];
        int err = 0;
        TerminalSession *session = [TerminalSessionStore.shared startSessionInDirectory:tab[@"directory"]
                                                                         restoredOutput:output
                                                                                  error:&err];
        if (session == nil) {
            [self showMessage:@"could not restore a tab" subtitle:[NSString stringWithFormat:@"error code %d", err]];
            continue;
        }
        session.title = tab[@"title"];
        [self.tabs addObject:session];
        if (toSelect == nil && [tab[@"selected"] boolValue])
            toSelect = session;
    }
    [NSFileManager.defaultManager removeItemAtURL:SavedTabsDirectory() error:nil];
    if (self.tabs.count == 0)
        return NO;
    self.selectedSession = toSelect ?: self.tabs.firstObject;
    return YES;
}

- (NSArray<NSUUID *> *)tabTerminalUUIDs {
    NSMutableArray<NSUUID *> *uuids = [NSMutableArray new];
    for (TerminalSession *session in self.tabs)
        [uuids addObject:session.uuid];
    return uuids;
}

- (NSUUID *)sessionTerminalUUID {
    return self.selectedSession.uuid;
}

- (Terminal *)sessionTerminal {
    return self.selectedSession.terminal;
}

#pragma mark Tabs

- (void)setSelectedSession:(TerminalSession *)session {
    NSAssert(session == nil || [self.tabs containsObject:session], @"selecting a session that is not a tab");
    // What the tab left showed is seen, and what the one shown shows: its tab
    // marks what it does from now on, while it is not looked at
    TerminalSession *left = _selectedSession;
    _selectedSession = session;
    [left markSeen];
    [session markSeen];
    self.terminal = session.terminal;
    [self tabsDidChange];
}

- (void)selectTabAtIndex:(NSUInteger)index {
    if (index < self.tabs.count)
        self.selectedSession = self.tabs[index];
}

- (void)selectNeighborTab:(NSInteger)offset {
    if (self.tabs.count < 2)
        return;
    NSInteger index = (NSInteger) [self.tabs indexOfObject:self.selectedSession];
    NSInteger count = (NSInteger) self.tabs.count;
    [self selectTabAtIndex:(NSUInteger) (((index + offset) % count + count) % count)];
}

- (void)closeTab:(TerminalSession *)session {
    NSUInteger index = [self.tabs indexOfObject:session];
    if (index == NSNotFound)
        return;
    [self.tabs removeObjectAtIndex:index];
    [TerminalSessionStore.shared closeSession:session];
    if (self.tabs.count == 0) {
        // A window with no shell in it is useless, so start a fresh one in place.
        [self startNewSession];
    } else if (session == self.selectedSession) {
        [self selectTabAtIndex:MIN(index, self.tabs.count - 1)];
    } else {
        [self tabsDidChange];
    }
}

- (void)sessionDidChange:(NSNotification *)notif {
    TerminalSession *session = notif.object;
    if (![self.tabs containsObject:session])
        return;
    if (session.state == TerminalSessionStateClosed) {
        // Closed by someone else (another window, or the app delegate).
        [self closeTab:session];
        return;
    }
    if (session.state == TerminalSessionStateExited) {
        // The shell exited (`exit`, Ctrl-D): close its tab. If it was the only one, closeTab:
        // starts a fresh shell in its place, as the app has always done on iPhone. A shell
        // that died right after starting keeps its tab marked exited instead: its output
        // usually says why, and replacing the only one would just loop.
        if ([NSDate.date timeIntervalSinceDate:session.startDate] > 1) {
            [self closeTab:session];
            return;
        }
    }
    [self tabsDidChange];
}

// Called whenever the tab list, the selection or a tab's state/title changes.
- (void)tabsDidChange {
    NSUInteger selected = self.selectedSession ? [self.tabs indexOfObject:self.selectedSession] : NSNotFound;
    [self.tabBar setSessions:self.tabs selectedIndex:selected];
}

- (void)tabBar:(TabBarView *)tabBar didSelectTabAtIndex:(NSUInteger)index {
    [self selectTabAtIndex:index];
    [self.termView becomeFirstResponder];
}

// Closing a tab hangs up its terminal, and the job that owns it (claude, top,
// a script) ends. Asks first, as a desktop terminal does, then close; tabs
// whose shell is at its prompt go without asking.
- (void)confirmClosingSessions:(NSArray<TerminalSession *> *)sessions then:(void (^)(void))close {
    NSMutableOrderedSet<NSString *> *programs = [NSMutableOrderedSet orderedSet];
    for (TerminalSession *session in sessions)
        [programs addObjectsFromArray:session.foregroundJobPrograms];
    if (programs.count == 0) {
        close();
        return;
    }
    NSString *list = [programs.array componentsJoinedByString:@", "];
    // "top is still running. Closing the tab ends it."
    NSString *message = [NSString stringWithFormat:@"%@ %@ still running. Closing %@ ends %@.", list,
                         programs.count == 1 ? @"is" : @"are", sessions.count == 1 ? @"the tab" : @"the tabs",
                         programs.count == 1 ? @"it" : @"them"];
    UIAlertController *alert = [UIAlertController
        alertControllerWithTitle:sessions.count == 1 ? @"Close Tab?" : @"Close Tabs?"
                         message:message
                  preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Close" style:UIAlertActionStyleDestructive
                                            handler:^(UIAlertAction *_) {
        close();
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)tabBar:(TabBarView *)tabBar didRequestCloseTabAtIndex:(NSUInteger)index {
    if (index >= self.tabs.count)
        return;
    TerminalSession *session = self.tabs[index];
    __weak typeof(self) weakSelf = self;
    [self confirmClosingSessions:@[session] then:^{
        [weakSelf closeTab:session];
    }];
}

- (void)tabBar:(TabBarView *)tabBar didRequestCloseOtherTabsAtIndex:(NSUInteger)index {
    if (index >= self.tabs.count)
        return;
    TerminalSession *keep = self.tabs[index];
    NSMutableArray<TerminalSession *> *others = [self.tabs mutableCopy];
    [others removeObject:keep];
    __weak typeof(self) weakSelf = self;
    [self confirmClosingSessions:others then:^{
        typeof(self) self = weakSelf;
        self.selectedSession = keep;
        for (TerminalSession *session in others)
            [self closeTab:session];
        [self.termView becomeFirstResponder];
    }];
}

- (void)tabBarDidRequestNewTab:(TabBarView *)tabBar {
    [self startNewSession];
    [self.termView becomeFirstResponder];
}

- (void)tabBar:(TabBarView *)tabBar didRequestDuplicateTabAtIndex:(NSUInteger)index {
    if (index >= self.tabs.count)
        return;
    [self duplicateSession:self.tabs[index]];
    [self.termView becomeFirstResponder];
}

- (void)tabBar:(TabBarView *)tabBar didRequestRestartTabAtIndex:(NSUInteger)index {
    if (index >= self.tabs.count)
        return;
    [self restartSession:self.tabs[index]];
    [self.termView becomeFirstResponder];
}

- (void)tabBar:(TabBarView *)tabBar didMoveTabAtIndex:(NSUInteger)from toIndex:(NSUInteger)to {
    if (from >= self.tabs.count || to >= self.tabs.count || from == to)
        return;
    TerminalSession *session = self.tabs[from];
    [self.tabs removeObjectAtIndex:from];
    [self.tabs insertObject:session atIndex:to];
    [self tabsDidChange];
}

- (UIMenu *)commandsMenuForTabBar:(TabBarView *)tabBar {
    return [MainMenu commandsMenu];
}

- (void)tabBar:(TabBarView *)tabBar didRequestRenameTabAtIndex:(NSUInteger)index {
    if (index >= self.tabs.count)
        return;
    TerminalSession *session = self.tabs[index];
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Rename Tab" message:nil preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = session.title;
        textField.placeholder = session.displayTitle;
        textField.clearButtonMode = UITextFieldViewModeWhileEditing;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak UIAlertController *weakAlert = alert;
    __weak TerminalViewController *weakSelf = self;
    UIAlertAction *rename = [UIAlertAction actionWithTitle:@"Rename" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        NSString *title = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        session.title = title.length > 0 ? title : nil;
        [weakSelf.termView becomeFirstResponder];
    }];
    [alert addAction:rename];
    alert.preferredAction = rename; // Return in the text field confirms
    [self presentViewController:alert animated:YES completion:nil];
}

#if ISH_LINUX
- (void)kernelPanicked:(NSNotification *)notif {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"panik" message:notif.userInfo[@"message"] preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"k" style:UIAlertActionStyleDefault handler:nil]];
    [self presentViewController:alert animated:YES completion:nil];
}
#endif

- (void)showMessage:(NSString *)message subtitle:(NSString *)subtitle {
    dispatch_async(dispatch_get_main_queue(), ^{
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:message message:subtitle preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"k"
                                                  style:UIAlertActionStyleDefault
                                                handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
    });
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary *)change context:(void *)context {
    if (object == [UserPreferences shared]) {
        [self _updateStyleFromPreferences:YES];
    } else {
        [super observeValueForKeyPath:keyPath ofObject:object change:change context:context];
    }
}

- (void)_updateStyleFromPreferences:(BOOL)animated {
    NSAssert(NSThread.isMainThread, @"This method needs to be called on the main thread");
    NSTimeInterval duration = animated ? 0.1 : 0;
    [UIView animateWithDuration:duration animations:^{
        self.view.backgroundColor = [[UIColor alloc] ish_initWithHexString:UserPreferences.shared.palette.backgroundColor];
        [self.tabBar setBackgroundColor:self.view.backgroundColor
                        foregroundColor:[[UIColor alloc] ish_initWithHexString:UserPreferences.shared.palette.foregroundColor]];
        UIKeyboardAppearance keyAppearance = UserPreferences.shared.keyboardAppearance;
        self.termView.keyboardAppearance = keyAppearance;
        for (BarButton *button in self.barButtons) {
            button.keyAppearance = keyAppearance;
        }
        // the gaps between the keys and the rest of the row
        self.barView.backgroundColor = self.view.backgroundColor;
    }];
    [self updatePasteControl];
    UIView *oldBarView = self.termView.inputAccessoryView;
    if (UserPreferences.shared.hideExtraKeysWithExternalKeyboard && self.hasExternalKeyboard) {
        self.termView.inputAccessoryView = nil;
    } else {
        self.termView.inputAccessoryView = self.barView;
    }
    if (self.termView.inputAccessoryView != oldBarView && self.termView.isFirstResponder) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self.ignoreKeyboardMotion = YES; // avoid infinite recursion
            [self.termView reloadInputViews];
            self.ignoreKeyboardMotion = NO;
        });
    }
}
- (void)_updateStyleAnimated {
    [self _updateStyleFromPreferences:YES];
}

- (void)_updateBadge {
    self.tabBar.showsCommandsBadge = FsNeedsRepositoryUpdate();
}

- (UIStatusBarStyle)preferredStatusBarStyle {
    return UserPreferences.shared.statusBarStyle;
}

- (BOOL)prefersStatusBarHidden {
    return UserPreferences.shared.hideStatusBar;
}

// A keyboard notice comes from the keyboard's window laying itself out: acted
// on there, the view's layout made that window's layout dirty again, and the
// next pass told of another frame, in the same commit of the layer tree for
// as long as the two went back and forth. It started with the snapshots for
// the app switcher, the app gone to the background or the screen locked; the
// app, suspended, went on with it when it came back, deaf to touches, until
// the watchdog killed it (0x8BADF00D, scene-update; build/ish-crash-reports
// of 2026-11-22 174953 and 180445, of 2026-11-23 162024, 193015, 204457 and
// 204518, hterm's and xterm.js's alike). So the last of the notices of a
// turn of the run loop is acted on after it, once, and in the background not
// at all: the last of them when the scene is active again.
- (void)keyboardDidSomething:(NSNotification *)notification {
    if (self.ignoreKeyboardMotion)
        return;
    self.pendingKeyboardNotice = notification;
    if (!self.keyboardNoticeScheduled)
        [self scheduleKeyboardNotice];
}

- (void)scheduleKeyboardNotice {
    self.keyboardNoticeScheduled = YES;
    dispatch_async(dispatch_get_main_queue(), ^{
        self.keyboardNoticeScheduled = NO;
        // kept for sceneDidActivate:
        if (self.view.window.windowScene.activationState == UISceneActivationStateBackground)
            return;
        NSNotification *notice = self.pendingKeyboardNotice;
        self.pendingKeyboardNotice = nil;
        [self keyboardMoved:notice];
    });
}

- (void)sceneDidActivate:(NSNotification *)notification {
    if (notification.object != self.view.window.windowScene)
        return;
    if (self.pendingKeyboardNotice != nil && !self.keyboardNoticeScheduled)
        [self scheduleKeyboardNotice];
}

- (void)keyboardMoved:(NSNotification *)notification {
    CGRect screenKeyboardFrame = [notification.userInfo[UIKeyboardFrameEndUserInfoKey] CGRectValue];
    UIScreen *screen = UIScreen.mainScreen;
    // notification.object is nil before iOS 16.1 and the correct UIScreen after iOS 16.1
    if (notification.object != nil)
        screen = notification.object;
    CGRect keyboardFrame = [self.view convertRect:screenKeyboardFrame fromCoordinateSpace:screen.coordinateSpace];
    if (CGRectEqualToRect(keyboardFrame, CGRectZero))
        return;
    CGRect intersection = CGRectIntersection(keyboardFrame, self.view.bounds);
    // None of it over the terminal (put away, on another screen, or the
    // scene resized under it): no pad. Its height then says nothing of an
    // external keyboard. CGRectNull's origin is infinite, and the pad was.
    if (CGRectIsNull(intersection))
        intersection = CGRectMake(0, CGRectGetMaxY(self.view.bounds), self.view.bounds.size.width, 0);
    else
        self.hasExternalKeyboard = intersection.size.height < 100;
    keyboardFrame = intersection;
    CGFloat pad = CGRectGetMaxY(self.view.bounds) - CGRectGetMinY(keyboardFrame);
    // The keyboard appears to be undocked. This means it can either be split or
    // truly floating. In the former case we want to keep the pad, but in the
    // latter we should fall back to the input accessory view instead of the
    // keyboard. Docked, it is as wide as the scene (its part over the view
    // is), which a resized scene's (Split View, a foldable's screens) is not
    // the main screen's width.
    if (pad != keyboardFrame.size.height && keyboardFrame.size.width < self.view.bounds.size.width) {
        pad = MAX(self.view.safeAreaInsets.bottom, self.termView.inputAccessoryView.frame.size.height);
    }
    // What the terminal's bottom is set from, and where the extra keys are:
    // if they cover its last line, these say which was off.
    UIView *extraKeys = self.termView.inputAccessoryView;
    NSLog(@"%@ keyboard %@ -> pad %.1f; extra keys at %@ (window %@)", notification.name, @(keyboardFrame), pad,
          @([self.view convertRect:extraKeys.bounds fromView:extraKeys]), extraKeys.window != nil ? @"yes" : @"no");
    // Nothing to move: no layout, which would lay out the keyboard's window
    if (self.bottomConstraint.constant == pad)
        return;
    self.bottomConstraint.constant = pad;

    BOOL initialLayout = self.termView.needsUpdateConstraints;
    [self.view setNeedsUpdateConstraints];
    if (!initialLayout) {
        // if initial layout hasn't happened yet, the terminal view is going to be at a really weird place, so animating it is going to look really bad
        NSNumber *interval = notification.userInfo[UIKeyboardAnimationDurationUserInfoKey];
        NSNumber *curve = notification.userInfo[UIKeyboardAnimationCurveUserInfoKey];
        [UIView animateWithDuration:interval.doubleValue
                              delay:0
                            options:curve.integerValue << 16
                         animations:^{
                             [self.view layoutIfNeeded];
                         }
                         completion:nil];
    }
}

- (void)setHasExternalKeyboard:(BOOL)hasExternalKeyboard {
    // Each keyboard notice sets it: restyling the keys for none that changed
    // it laid out the keyboard's window, which told of its frame again
    if (hasExternalKeyboard == _hasExternalKeyboard)
        return;
    _hasExternalKeyboard = hasExternalKeyboard;
    [self _updateStyleFromPreferences:YES];
}

- (void)prepareForSegue:(UIStoryboardSegue *)segue sender:(id)sender {
    if ([segue.identifier isEqualToString:@"embed"]) {
        // You might want to check if this is your embed segue here
        // in case there are other segues triggered from this view controller.
        segue.destinationViewController.view.translatesAutoresizingMaskIntoConstraints = NO;
    }
}

- (void)traitCollectionDidChange:(UITraitCollection *)previousTraitCollection {
    // Hack to resolve a layering mismatch between the UI and preferences.
    if (@available(iOS 12.0, *)) {
        if (previousTraitCollection.userInterfaceStyle != self.traitCollection.userInterfaceStyle) {
            // Ensure that the relevant things listening for this will update.
            UserPreferences.shared.colorScheme = UserPreferences.shared.colorScheme;
        }
    }
}

#pragma mark Bar

- (IBAction)showAbout:(id)sender {
    UINavigationController *navigationController = [[UIStoryboard storyboardWithName:@"About" bundle:nil] instantiateInitialViewController];
    if ([sender isKindOfClass:[UIGestureRecognizer class]]) {
        UIGestureRecognizer *recognizer = sender;
        if (recognizer.state == UIGestureRecognizerStateBegan) {
            AboutViewController *aboutViewController = (AboutViewController *) navigationController.topViewController;
            aboutViewController.includeDebugPanel = YES;
        } else {
            return;
        }
    }
    [self presentViewController:navigationController animated:YES completion:nil];
    [self.termView resignFirstResponder];
}

- (void)resizeBar {
    // Keys 41 points wide on a phone and 50 on an iPad, which in Slide Over is
    // as narrow as a phone; narrower if the row is (39 on a 320-point iPhone
    // SE). Every arranged view but the spacer between tab and the arrows is a
    // key.
    bool phone = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone;
    NSUInteger views = self.bar.arrangedSubviews.count;
    NSUInteger keys = views - 1;
    CGFloat width = UIEdgeInsetsInsetRect(self.barView.bounds, self.barView.safeAreaInsets).size.width;
    CGFloat fit = (width - self.bar.spacing * (views - 1)) / keys;
    self.barButtonWidth.constant = MIN(phone ? 41 : 50, floor(fit));
    [UIView performWithoutAnimation:^{
        [self.barView layoutIfNeeded];
    }];
}

- (IBAction)pressEscape:(id)sender {
    [self pressKey:@"\x1b"];
}
- (IBAction)pressTab:(id)sender {
    [self pressKey:@"\t"];
}
- (void)pressKey:(NSString *)key {
    [self.termView insertText:key];
}

- (IBAction)pressControl:(id)sender {
    self.controlKey.selected = !self.controlKey.selected;
}

// An arrow key sends its sequence on touch down and, while held, repeats it
// after half a second, ten times a second, like a hardware key. Ctrl from the
// bar is taken once and applies to the whole press.
- (IBAction)arrowKeyDown:(UIButton *)sender {
    NSString *sequence = [self.terminal arrow:arrowKeyFinals[sender.tag]
                                    modifiers:[self.termView takeBarModifiers]];
    [self.termView insertSequence:sequence];
    [self.arrowRepeat invalidate];
    __weak typeof(self) weakSelf = self;
    self.arrowRepeat = [NSTimer scheduledTimerWithTimeInterval:0.5 repeats:NO block:^(NSTimer *_) {
        [weakSelf.termView insertSequence:sequence];
        weakSelf.arrowRepeat = [NSTimer scheduledTimerWithTimeInterval:0.1 repeats:YES block:^(NSTimer *_) {
            [weakSelf.termView insertSequence:sequence];
        }];
    }];
}

// Touch up inside or outside, or cancelled (the keyboard went away).
- (IBAction)arrowKeyUp:(id)sender {
    [self.arrowRepeat invalidate];
    self.arrowRepeat = nil;
}

#pragma mark Commands

static NSString *ShellQuoted(NSString *string) {
    return [NSString stringWithFormat:@"'%@'", [string stringByReplacingOccurrencesOfString:@"'" withString:@"'\\''"]];
}

// Reached from the main menu, the tab strip's ⋯ menu and key equivalents (see MainMenu).

- (void)switchTerminal:(UICommand *)sender {
    int i = [sender.propertyList intValue];
    if (i == 7)
        self.terminal = self.sessionTerminal;
    else
        self.terminal = [Terminal terminalWithType:TTY_CONSOLE_MAJOR number:i];
}

- (void)increaseFontSize:(id)sender {
    self.termView.overrideFontSize = self.termView.effectiveFontSize + 1;
}
- (void)decreaseFontSize:(id)sender {
    self.termView.overrideFontSize = self.termView.effectiveFontSize - 1;
}
- (void)resetFontSize:(id)sender {
    self.termView.overrideFontSize = 0;
}

// Shortcuts that are not menu items: a second binding for Bigger (the unshifted key)
// and Ctrl+Tab for cycling tabs, as in browsers.
- (NSArray<UIKeyCommand *> *)keyCommands {
    static NSArray<UIKeyCommand *> *commands = nil;
    if (commands == nil) {
        commands = @[
            [UIKeyCommand keyCommandWithInput:@"=" modifierFlags:UIKeyModifierCommand action:@selector(increaseFontSize:)],
            [UIKeyCommand keyCommandWithInput:@"\t" modifierFlags:UIKeyModifierControl action:@selector(nextTab:)],
            [UIKeyCommand keyCommandWithInput:@"\t" modifierFlags:UIKeyModifierControl | UIKeyModifierShift action:@selector(previousTab:)],
        ];
        for (UIKeyCommand *command in commands)
            command.wantsPriorityOverSystemBehavior = YES;
    }
    return commands;
}

- (BOOL)canPerformAction:(SEL)action withSender:(id)sender {
    if (action == @selector(paste:))
        return UIPasteboard.generalPasteboard.hasStrings;
    return [super canPerformAction:action withSender:sender];
}

// Menu state that depends on the tabs, windows and preferences of the moment.
- (void)validateCommand:(UICommand *)command {
    SEL action = command.action;
    NSUInteger count = self.tabs.count;
    BOOL running = self.selectedSession.state == TerminalSessionStateRunning;
    UserPreferences *prefs = UserPreferences.shared;
    if (action == @selector(selectTabByNumber:)) {
        NSUInteger index = [command.propertyList unsignedIntegerValue];
        if (index < count) {
            command.title = self.tabs[index].displayTitle;
            command.state = self.tabs[index] == self.selectedSession ? UIMenuElementStateOn : UIMenuElementStateOff;
            command.attributes = 0;
        } else {
            command.attributes = UIMenuElementAttributesHidden;
        }
    } else if (action == @selector(closeOtherTabs:) || action == @selector(nextTab:) || action == @selector(previousTab:) ||
               action == @selector(moveTabLeft:) || action == @selector(moveTabRight:)) {
        command.attributes = count < 2 ? UIMenuElementAttributesDisabled : 0;
    } else if (action == @selector(newWindow:) || action == @selector(closeWindow:) ||
               action == @selector(moveTabToNewWindow:) || action == @selector(mergeAllWindows:)) {
        if (!UIApplication.sharedApplication.supportsMultipleScenes) {
            command.attributes = UIMenuElementAttributesHidden;
        } else {
            BOOL others = ConnectedTerminalViewControllers().count > 1;
            BOOL enabled = action == @selector(newWindow:) ||
                (action == @selector(moveTabToNewWindow:) && count > 1) ||
                (action == @selector(mergeAllWindows:) && others) ||
                (action == @selector(closeWindow:) && UIApplication.sharedApplication.openSessions.count > 1);
            command.attributes = enabled ? 0 : UIMenuElementAttributesDisabled;
        }
    } else if (action == @selector(duplicateTab:) || action == @selector(copyCurrentDirectory:) ||
               action == @selector(mountFolder:) || action == @selector(runSavedCommand:)) {
        command.attributes = running ? 0 : UIMenuElementAttributesDisabled;
    } else if (action == @selector(selectTheme:)) {
        command.state = [command.propertyList isEqual:prefs.theme.name] ? UIMenuElementStateOn : UIMenuElementStateOff;
    } else if (action == @selector(selectColorScheme:)) {
        command.state = [command.propertyList integerValue] == prefs.colorScheme ? UIMenuElementStateOn : UIMenuElementStateOff;
    } else if (action == @selector(toggleExtraKeys:)) {
        command.state = prefs.hideExtraKeysWithExternalKeyboard ? UIMenuElementStateOff : UIMenuElementStateOn;
    } else if (action == @selector(toggleStatusBar:)) {
        command.state = prefs.hideStatusBar ? UIMenuElementStateOn : UIMenuElementStateOff;
    } else if (action == @selector(toggleKeepAlive:)) {
        BackgroundKeepAlive *keepAlive = BackgroundKeepAlive.shared;
        command.state = keepAlive.enabled && !keepAlive.denied ? UIMenuElementStateOn : UIMenuElementStateOff;
    } else if (action == @selector(toggleClipboard:)) {
        command.state = self.shelfShown ? UIMenuElementStateOn : UIMenuElementStateOff;
    } else if (action == @selector(togglePasteStack:)) {
        command.state = ClipboardStore.shared.stackActive ? UIMenuElementStateOn : UIMenuElementStateOff;
    } else if (action == @selector(toggleClipboardCapture:)) {
        BOOL paused = ClipboardStore.shared.paused;
        command.title = paused ? @"Resume Capture" : @"Pause Capture";
        command.image = [UIImage systemImageNamed:paused ? @"play" : @"pause"];
    } else if (action == @selector(switchTerminal:)) {
        int number = [command.propertyList intValue];
        BOOL current = number == 7 ? self.terminal == self.sessionTerminal
                                   : self.terminal == [Terminal terminalWithType:TTY_CONSOLE_MAJOR number:number];
        command.state = current ? UIMenuElementStateOn : UIMenuElementStateOff;
    }
}

#pragma mark Shell menu

- (void)newTab:(id)sender {
    [self startNewSession];
}

- (void)newWindow:(id)sender {
    [UIApplication.sharedApplication requestSceneSessionActivation:nil userActivity:nil options:nil errorHandler:nil];
}

- (void)duplicateTab:(id)sender {
    if (self.selectedSession != nil)
        [self duplicateSession:self.selectedSession];
}

// A new tab after session's, in its directory. The new shell logs in at home
// like any other; it is then told to cd to where the duplicated one was,
// visibly, since login leaves no other way to pick a directory.
- (void)duplicateSession:(TerminalSession *)session {
    NSString *directory = session.currentDirectory;
    TerminalSession *copy = [self startNewSessionAtIndex:[self.tabs indexOfObject:session] + 1];
    if (copy != nil && directory != nil)
        [self typeCommand:[@"cd " stringByAppendingString:ShellQuoted(directory)] intoSession:copy attempts:50];
}

- (void)renameCurrentTab:(id)sender {
    NSUInteger index = [self.tabs indexOfObject:self.selectedSession];
    if (index != NSNotFound)
        [self tabBar:self.tabBar didRequestRenameTabAtIndex:index];
}

- (void)restartShell:(id)sender {
    if (self.selectedSession != nil)
        [self restartSession:self.selectedSession];
}

// A fresh shell in old's tab: same position and name, selected.
- (void)restartSession:(TerminalSession *)old {
    NSUInteger index = [self.tabs indexOfObject:old];
    if (index == NSNotFound)
        return;
    int err = 0;
    TerminalSession *session = [TerminalSessionStore.shared startSessionWithError:&err];
    if (session == nil) {
        [self showMessage:@"could not start session" subtitle:[NSString stringWithFormat:@"error code %d", err]];
        return;
    }
    session.title = old.title;
    [self.tabs replaceObjectAtIndex:index withObject:session];
    [TerminalSessionStore.shared closeSession:old];
    self.selectedSession = session;
}

- (void)resetTerminal:(id)sender {
    [self.termView resetTerminal];
}

- (void)sendBytes:(const char *)bytes {
    [self.terminal sendInput:[NSData dataWithBytes:bytes length:strlen(bytes)]];
}
- (void)sendInterrupt:(id)sender {
    [self sendBytes:"\x03"];
}
- (void)sendEndOfFile:(id)sender {
    [self sendBytes:"\x04"];
}
- (void)sendSuspend:(id)sender {
    [self sendBytes:"\x1a"];
}
- (void)killForegroundJob:(id)sender {
    [self.terminal sendSignalToForegroundJob:SIGKILL_];
}

- (void)exportScrollback:(id)sender {
    // the title as a file's name: it can be a path (/)
    NSString *title = [self.selectedSession.displayTitle stringByReplacingOccurrencesOfString:@"/" withString:@"-"];
    NSString *name = [title stringByAppendingPathExtension:@"txt"] ?: @"scrollback.txt";
    [self.termView fetchTextWithCompletion:^(NSString *text) {
        // The text ends with the screen's empty rows below the cursor; drop them.
        NSString *trimmed = [text stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet];
        NSURL *url = [NSURL fileURLWithPath:[NSTemporaryDirectory() stringByAppendingPathComponent:name]];
        NSError *error = nil;
        if (![[trimmed stringByAppendingString:@"\n"] writeToURL:url atomically:YES encoding:NSUTF8StringEncoding error:&error]) {
            [self showMessage:@"could not export scrollback" subtitle:error.localizedDescription];
            return;
        }
        UIDocumentPickerViewController *picker = [[UIDocumentPickerViewController alloc] initForExportingURLs:@[url] asCopy:YES];
        [self presentViewController:picker animated:YES completion:nil];
    }];
}

- (void)closeCurrentTab:(id)sender {
    TerminalSession *session = self.selectedSession;
    if (session == nil)
        return;
    __weak typeof(self) weakSelf = self;
    [self confirmClosingSessions:@[session] then:^{
        [weakSelf closeTab:session];
    }];
}

- (void)closeOtherTabs:(id)sender {
    NSUInteger index = [self.tabs indexOfObject:self.selectedSession];
    if (index != NSNotFound)
        [self tabBar:self.tabBar didRequestCloseOtherTabsAtIndex:index];
}

- (void)closeWindow:(id)sender {
    if (self.sceneSession != nil)
        [UIApplication.sharedApplication requestSceneSessionDestruction:self.sceneSession options:nil errorHandler:nil];
}

#pragma mark Edit menu

- (void)paste:(id)sender {
    [self.termView paste:sender];
}

// The bar's paste key is iOS's own paste button (UIPasteControl) laid over the
// key: what that pastes iSH has without iOS asking. The key read the clipboard
// itself, and iOS asked "would like to paste from …" at every press whenever
// another app or device (a Mac, by Universal Clipboard) had copied what was
// there. A paste button's colors are fixed when it is made, so it is made
// again when the keys' change.
- (void)updatePasteControl {
    BarButton *key = self.pasteButton;
    if (self.pasteControl != nil && self.pasteControlAppearance == key.keyAppearance)
        return;
    [self.pasteControl removeFromSuperview];
    UIPasteControlConfiguration *config = [UIPasteControlConfiguration new];
    config.displayMode = UIPasteControlDisplayModeIconOnly;
    // flat and square, in the key's colors
    config.cornerStyle = UIButtonConfigurationCornerStyleFixed;
    config.cornerRadius = 0;
    config.baseBackgroundColor = key.defaultColor;
    config.baseForegroundColor = key.tintColor;
    UIPasteControl *control = [[UIPasteControl alloc] initWithConfiguration:config];
    control.target = self;
    control.translatesAutoresizingMaskIntoConstraints = NO;
    [key addSubview:control];
    [NSLayoutConstraint activateConstraints:@[
        [control.leadingAnchor constraintEqualToAnchor:key.leadingAnchor],
        [control.trailingAnchor constraintEqualToAnchor:key.trailingAnchor],
        [control.topAnchor constraintEqualToAnchor:key.topAnchor],
        [control.bottomAnchor constraintEqualToAnchor:key.bottomAnchor],
    ]];
    self.pasteControl = control;
    self.pasteControlAppearance = key.keyAppearance;
}

// What iOS's paste button pastes (updatePasteControl): the first item's text,
// as paste: pastes the clipboard's string; the Paste Stack's next item instead
// while it is open.
- (void)pasteItemProviders:(NSArray<NSItemProvider *> *)itemProviders {
    if ([self pasteFromStack])
        return;
    __weak typeof(self) weakSelf = self;
    [itemProviders.firstObject loadObjectOfClass:NSString.class
                               completionHandler:^(id<NSItemProviderReading> object, NSError *error) {
        if (object == nil) {
            NSLog(@"Paste button: no text from the clipboard: %@", error);
            return;
        }
        NSString *string = (NSString *) object;
        dispatch_async(dispatch_get_main_queue(), ^{
            [weakSelf.termView.terminal pasteText:string];
        });
    }];
}

- (void)clearScreen:(id)sender {
    [self.termView clearScreen];
}
- (void)clearScrollback:(id)sender {
    [self.termView clearScrollback];
}

#pragma mark View menu

- (void)selectTheme:(UICommand *)sender {
    Theme *theme = [Theme themeForName:sender.propertyList includingDefaultThemes:YES];
    if (theme != nil)
        UserPreferences.shared.theme = theme;
}
- (void)selectColorScheme:(UICommand *)sender {
    UserPreferences.shared.colorScheme = [sender.propertyList integerValue];
}
- (void)toggleExtraKeys:(id)sender {
    UserPreferences.shared.hideExtraKeysWithExternalKeyboard = !UserPreferences.shared.hideExtraKeysWithExternalKeyboard;
}
- (void)toggleStatusBar:(id)sender {
    UserPreferences.shared.hideStatusBar = !UserPreferences.shared.hideStatusBar;
}

- (void)toggleKeepAlive:(id)sender {
    BackgroundKeepAlive *keepAlive = BackgroundKeepAlive.shared;
    if (keepAlive.denied) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Location Access Needed"
                                                                       message:@"iSH stays running in the background by receiving coarse location updates. Allow location access for iSH in Settings."
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"Open Settings" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
            [UIApplication.sharedApplication openURL:[NSURL URLWithString:UIApplicationOpenSettingsURLString] options:@{} completionHandler:nil];
        }]];
        [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
        keepAlive.enabled = YES;
        return;
    }
    keepAlive.enabled = !keepAlive.enabled;
}

#pragma mark Tabs menu

- (void)nextTab:(id)sender {
    [self selectNeighborTab:1];
}

- (void)swipedAcrossTerminal:(UISwipeGestureRecognizer *)swipe {
    [self selectNeighborTab:swipe.direction == UISwipeGestureRecognizerDirectionLeft ? 1 : -1];
}

// The tab swipes go along with the terminal's own gestures (its scrolling)
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)otherGestureRecognizer {
    return [gestureRecognizer isKindOfClass:UISwipeGestureRecognizer.class] && gestureRecognizer.view == self.termView;
}
- (void)previousTab:(id)sender {
    [self selectNeighborTab:-1];
}
- (void)selectTabByNumber:(UICommand *)sender {
    [self selectTabAtIndex:[sender.propertyList unsignedIntegerValue]];
}

- (void)moveSelectedTabBy:(NSInteger)offset {
    NSInteger index = (NSInteger) [self.tabs indexOfObject:self.selectedSession];
    NSInteger target = index + offset;
    if (self.selectedSession == nil || target < 0 || target >= (NSInteger) self.tabs.count)
        return;
    [self.tabs exchangeObjectAtIndex:(NSUInteger) index withObjectAtIndex:(NSUInteger) target];
    [self tabsDidChange];
}
- (void)moveTabLeft:(id)sender {
    [self moveSelectedTabBy:-1];
}
- (void)moveTabRight:(id)sender {
    [self moveSelectedTabBy:1];
}

// Takes every tab out of this window without closing the shells, for another window to adopt.
- (NSArray<TerminalSession *> *)detachAllTabs {
    NSArray<TerminalSession *> *detached = [self.tabs copy];
    [self.tabs removeAllObjects];
    self.selectedSession = nil;
    return detached;
}

- (void)moveTabToNewWindow:(id)sender {
    TerminalSession *session = self.selectedSession;
    NSUInteger index = [self.tabs indexOfObject:session];
    if (index == NSNotFound || self.tabs.count < 2)
        return;
    [self.tabs removeObjectAtIndex:index];
    [self selectTabAtIndex:MIN(index, self.tabs.count - 1)];
    NSUserActivity *activity = [[NSUserActivity alloc] initWithActivityType:SceneActivityType];
    [activity addUserInfoEntriesFromDictionary:@{SceneTerminalUUIDKey: session.uuid.UUIDString,
                                                 SceneTerminalUUIDsKey: @[session.uuid.UUIDString]}];
    [UIApplication.sharedApplication requestSceneSessionActivation:nil userActivity:activity options:nil errorHandler:^(NSError *error) {
        // No new window, so the tab comes back here rather than being lost.
        dispatch_async(dispatch_get_main_queue(), ^{
            if (session.state != TerminalSessionStateClosed && ![self.tabs containsObject:session]) {
                [self.tabs addObject:session];
                self.selectedSession = session;
            }
        });
    }];
}

- (void)mergeAllWindows:(id)sender {
    for (TerminalViewController *other in ConnectedTerminalViewControllers()) {
        if (other == self)
            continue;
        [self.tabs addObjectsFromArray:[other detachAllTabs]];
        [UIApplication.sharedApplication requestSceneSessionDestruction:other.sceneSession options:nil errorHandler:^(NSError *error) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (other.tabs.count == 0)
                    [other startNewSession];
            });
        }];
    }
    [self tabsDidChange];
}

#pragma mark Tools menu

// Types a command line into the selected shell, as if at the keyboard. If something
// else owns the terminal (a job in the foreground, or a console being shown), offers
// to run it in a new tab instead of typing into whatever is running.
- (void)runInShell:(NSString *)command {
    TerminalSession *session = self.selectedSession;
    if (session.state == TerminalSessionStateRunning && session.shellIsForeground && self.terminal == session.terminal) {
        [session.terminal sendInput:[[command stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding]];
        return;
    }
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:[NSString stringWithFormat:@"%@ is busy", session.displayTitle]
                                                                   message:command
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    [alert addAction:[UIAlertAction actionWithTitle:@"Run in New Tab" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        [self runInNewTab:command];
    }]];
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)runInNewTab:(NSString *)command {
    TerminalSession *session = [self startNewSessionAtIndex:self.tabs.count];
    if (session != nil)
        [self typeCommand:command intoSession:session attempts:50];
}

// The new shell needs a moment to take the terminal; typing before that would be
// thrown away by login's terminal setup. Once the shell is there, a beat more lets
// it finish its own setup, so the text lands after its prompt.
- (void)typeCommand:(NSString *)command intoSession:(TerminalSession *)session attempts:(int)attempts {
    if (session.state != TerminalSessionStateRunning)
        return;
    if (session.shellIsForeground) {
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (0.25 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
            if (session.state == TerminalSessionStateRunning)
                [session.terminal sendInput:[[command stringByAppendingString:@"\n"] dataUsingEncoding:NSUTF8StringEncoding]];
        });
        return;
    }
    if (attempts <= 0)
        return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t) (0.1 * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
        [self typeCommand:command intoSession:session attempts:attempts - 1];
    });
}

- (void)mountFolder:(id)sender {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Mount iOS Folder"
                                                                   message:@"A folder picker opens once the mount point is set. The mount is remembered across launches; use Unmount to forget it."
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.text = @"/mnt/ios";
        textField.placeholder = @"Mount point";
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak UIAlertController *weakAlert = alert;
    UIAlertAction *mount = [UIAlertAction actionWithTitle:@"Mount" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        NSString *path = [weakAlert.textFields.firstObject.text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (path.length == 0)
            return;
        [self runInShell:[NSString stringWithFormat:@"mkdir -p %@ && mount -t ios . %@", ShellQuoted(path), ShellQuoted(path)]];
    }];
    [alert addAction:mount];
    alert.preferredAction = mount;
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)unmountFolder:(UICommand *)sender {
    NSString *path = sender.propertyList;
    if ([path isKindOfClass:NSString.class])
        [self runInShell:[@"umount " stringByAppendingString:ShellQuoted(path)]];
}

- (void)copyCurrentDirectory:(id)sender {
    NSString *directory = self.selectedSession.currentDirectory;
    if (directory != nil)
        UIPasteboard.generalPasteboard.string = directory;
}

- (void)runSavedCommand:(UICommand *)sender {
    if ([sender.propertyList isKindOfClass:NSString.class])
        [self runInShell:sender.propertyList];
}

- (void)addSavedCommand:(id)sender {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Add Saved Command" message:nil preferredStyle:UIAlertControllerStyleAlert];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.placeholder = @"Name";
    }];
    [alert addTextFieldWithConfigurationHandler:^(UITextField *textField) {
        textField.placeholder = @"Command";
        textField.autocapitalizationType = UITextAutocapitalizationTypeNone;
        textField.autocorrectionType = UITextAutocorrectionTypeNo;
        textField.font = [UIFont monospacedSystemFontOfSize:UIFont.systemFontSize weight:UIFontWeightRegular];
    }];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    __weak UIAlertController *weakAlert = alert;
    UIAlertAction *add = [UIAlertAction actionWithTitle:@"Add" style:UIAlertActionStyleDefault handler:^(UIAlertAction *action) {
        NSString *name = [weakAlert.textFields[0].text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        NSString *command = [weakAlert.textFields[1].text stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
        if (command.length == 0)
            return;
        [SavedCommand addWithName:name.length > 0 ? name : command command:command];
    }];
    [alert addAction:add];
    alert.preferredAction = add;
    [self presentViewController:alert animated:YES completion:nil];
}

- (void)removeSavedCommand:(UICommand *)sender {
    [SavedCommand removeAtIndex:[sender.propertyList unsignedIntegerValue]];
}

#pragma mark Clipboard menu

- (void)toggleClipboard:(id)sender {
    if (self.shelfShown)
        [self hideClipboard];
    else
        [self showClipboard];
}

- (void)showClipboard {
    if (self.shelfShown)
        return;
    if (self.shelf == nil) {
        ClipboardShelfView *shelf = [ClipboardShelfView new];
        shelf.delegate = self;
        shelf.translatesAutoresizingMaskIntoConstraints = NO;
        shelf.hidden = YES;
        [self.view addSubview:shelf];
        UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
        [NSLayoutConstraint activateConstraints:@[
            [shelf.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:10],
            [shelf.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-10],
            [shelf.bottomAnchor constraintEqualToAnchor:self.termView.bottomAnchor constant:-10],
            [shelf.topAnchor constraintGreaterThanOrEqualToAnchor:self.tabBar.bottomAnchor constant:10],
        ]];
        self.shelf = shelf;
    }
    ClipboardShelfView *shelf = self.shelf;
    // Anything copied in another app while iSH was in the background shows up now.
    [ClipboardStore.shared checkPasteboard];
    shelf.overrideUserInterfaceStyle = UserPreferences.shared.requestingDarkAppearance ? UIUserInterfaceStyleDark : UIUserInterfaceStyleLight;
    [shelf prepareToShow];
    self.shelfShown = YES;
    shelf.hidden = NO;
    [self.view bringSubviewToFront:shelf];
    [self.view layoutIfNeeded];
    shelf.transform = CGAffineTransformMakeTranslation(0, shelf.bounds.size.height / 2);
    shelf.alpha = 0;
    [UIView animateWithDuration:0.35 delay:0 usingSpringWithDamping:0.85 initialSpringVelocity:0 options:0 animations:^{
        shelf.transform = CGAffineTransformIdentity;
        shelf.alpha = 1;
    } completion:nil];
    [shelf becomeFirstResponder];
    UIAccessibilityPostNotification(UIAccessibilityScreenChangedNotification, shelf);
}

- (void)hideClipboard {
    if (!self.shelfShown)
        return;
    self.shelfShown = NO;
    ClipboardShelfView *shelf = self.shelf;
    [UIView animateWithDuration:0.2 animations:^{
        shelf.transform = CGAffineTransformMakeTranslation(0, shelf.bounds.size.height / 3);
        shelf.alpha = 0;
    } completion:^(BOOL finished) {
        if (!self.shelfShown) {
            shelf.hidden = YES;
            shelf.transform = CGAffineTransformIdentity;
        }
    }];
    [self.termView becomeFirstResponder];
}

- (void)togglePasteStack:(id)sender {
    ClipboardStore.shared.stackActive = !ClipboardStore.shared.stackActive;
}

- (void)toggleClipboardCapture:(id)sender {
    ClipboardStore *store = ClipboardStore.shared;
    if (store.paused)
        [store resume];
    else
        [store pauseFor:0];
    ClipShowToast(self.view, store.paused ? @"Clipboard Capture Paused" : @"Clipboard Capture Resumed", store.paused ? @"pause.fill" : @"play.fill");
}

- (void)newPinboard:(id)sender {
    [self showClipboard];
    [self.shelf promptForNewPinboard];
}

- (void)showClipboardSettings:(id)sender {
    __weak typeof(self) weakSelf = self;
    UIViewController *settings = [ClipboardSettingsViewController navigationControllerWithDismissHandler:^{
        if (weakSelf.shelfShown)
            [weakSelf.shelf becomeFirstResponder];
    }];
    UIViewController *presenter = self;
    while (presenter.presentedViewController != nil)
        presenter = presenter.presentedViewController;
    [presenter presentViewController:settings animated:YES completion:nil];
}

// The stack panel follows the store, so every window shows it while it is open.
- (void)pasteStackDidChange:(NSNotification *)notif {
    BOOL active = ClipboardStore.shared.stackActive;
    if (active && self.stackView == nil) {
        ClipboardStackView *stack = [ClipboardStackView new];
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        stack.closeHandler = ^{
            ClipboardStore.shared.stackActive = NO;
        };
        [self.view addSubview:stack];
        [NSLayoutConstraint activateConstraints:@[
            [stack.topAnchor constraintEqualToAnchor:self.tabBar.bottomAnchor constant:10],
            [stack.trailingAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.trailingAnchor constant:-12],
        ]];
        self.stackView = stack;
        stack.alpha = 0;
        [UIView animateWithDuration:0.2 animations:^{
            stack.alpha = 1;
        }];
    } else if (!active && self.stackView != nil) {
        ClipboardStackView *stack = self.stackView;
        self.stackView = nil;
        [UIView animateWithDuration:0.2 animations:^{
            stack.alpha = 0;
        } completion:^(BOOL finished) {
            [stack removeFromSuperview];
        }];
    }
}

// While the Paste Stack is open, paste takes the next item off it.
- (BOOL)pasteFromStack {
    ClipboardStore *store = ClipboardStore.shared;
    if (!store.stackActive)
        return NO;
    ClipItem *item = [store popStackItem];
    if (item == nil)
        return NO;
    [store copyItems:@[item] plainText:ClipboardPreferences.shared.alwaysPlainText];
    NSString *text = [store textForItems:@[item]];
    if (text == nil) {
        ClipShowToast(self.view, @"Image Copied", @"photo");
        return YES;
    }
    [self.termView insertText:text];
    return YES;
}

#pragma mark ClipboardShelfDelegate

- (void)shelf:(ClipboardShelfView *)shelf pasteText:(NSString *)text {
    [self.termView insertText:text];
}

- (void)shelfDidRequestClose:(ClipboardShelfView *)shelf {
    [self hideClipboard];
}

- (UIViewController *)presentingViewControllerForShelf:(ClipboardShelfView *)shelf {
    return self;
}

- (void)shelfDidRequestSettings:(ClipboardShelfView *)shelf {
    [self showClipboardSettings:shelf];
}

- (void)shelfDidRequestPasteStack:(ClipboardShelfView *)shelf {
    [self togglePasteStack:shelf];
}

- (void)setTerminal:(Terminal *)terminal {
    _terminal = terminal;
    self.termView.terminal = self.terminal;
}

@end

@interface BarView : UIInputView
@property (weak) IBOutlet TerminalViewController *terminalViewController;
@property (nonatomic) IBInspectable BOOL allowsSelfSizing;
@end
@implementation BarView
@dynamic allowsSelfSizing;

- (void)layoutSubviews {
    [self.terminalViewController resizeBar];
}

@end
