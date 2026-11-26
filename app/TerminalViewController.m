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
#import "ClipboardStore.h"
#import "ClipboardShelfView.h"
#import "ClipboardStackView.h"
#import "ClipboardSettingsViewController.h"
#import "ClipboardUI.h"
#import "MonitorStripView.h"
#import "MonitorColumnView.h"
#import "MonitorDetailViewController.h"
#include "kernel/init.h"
#include "kernel/task.h"
#include "kernel/calls.h"
#include "kernel/signal.h"
#include "fs/devices.h"

@class FindBar;

// A column key held: what it sends then, and what its callout says it sends
// (columnKeyHeld:)
@interface ColumnKeyHold : UILongPressGestureRecognizer
@property (copy) NSString *callout;
@property (copy) void (^send)(void);
@end
@implementation ColumnKeyHold
@end

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

// The paste key: iOS's paste button lies over it (updatePasteControl), in the
// key's colors it was made in, but while the Paste Stack has items, when the
// key pastes the next one itself (pasteKeyPastesStack, pasteKey:).
@property (weak, nonatomic) IBOutlet BarButton *pasteButton;
@property (nonatomic, nullable) UIPasteControl *pasteControl;
@property (nullable) UIColor *pasteControlKeyColor;
@property (nullable) UIColor *pasteControlTextColor;
// After tab: /, and held, the other symbols (addSymbolKey)
@property BarButton *symbolKey;
// At the right end, after paste: the keyboard put down (addHideKeyboardKey)
@property BarButton *hideKeyboardKey;
@property (nullable) KeyPopupView *symbolPopup;

// Sessions shown in this window, in tab order. All of them are also in the TerminalSessionStore.
@property NSMutableArray<TerminalSession *> *tabs;
@property (nonatomic) TerminalSession *selectedSession;
// The selected tab's terminal (self.terminal differs from this while a console is shown).
@property (readonly) Terminal *sessionTerminal;

// The clipboard manager, created the first time it is shown.
@property (nullable) ClipboardShelfView *shelf;
@property BOOL shelfShown;
@property (nullable) ClipboardStackView *stackView;

// Find (⌘F), created the first time it is shown; hidden while not finding
@property (nullable) FindBar *findBar;

// The meters, in what the terminal leaves right of its last column, and in
// the window's column right of the terminal where there is one; one sampler
// reads for both
@property MonitorSampler *monitorSampler;
@property MonitorStripView *monitorStrip;
@property MonitorColumnView *monitorColumn;
// The column's meters under the status bar (top: updateMonitorColumn), and
// above the keyboard and the keys over it (the paste key's row), or the home
// indicator (aboveKeys: applyBottomPad)
@property NSLayoutConstraint *monitorColumnTop;
// Esc, tab, /, up, down and return at the bottom of the column, while it is
// shown and the keyboard is down (updateColumnKeys): what a program's menus
// take (claude's: / for its commands, esc to leave one), without the keyboard
// over the terminal. The meters stay above them. With the keyboard up, esc
// and tab only, at the column's bottom above the keyboard's bar
// (columnKeysAboveKeyboard), where the CPU and the memory leave the room
// (updateColumnKeys); the bar over the keyboard has them too.
@property UIStackView *columnKeys;
@property NSArray<UIView *> *columnKeysKeyboardDown;
@property NSLayoutConstraint *columnKeysAtBottom;
@property NSLayoutConstraint *columnKeysAboveKeyboard;
// A tap of one: the keyboard's click is heard only while a keyboard shows
@property UIImpactFeedbackGenerator *columnKeyFeedback;
// A column key held (columnKeyHeld:): the callout left of it, saying what
// letting go sends, or that it sends nothing, the finger moved off; a firmer
// tap as it shows, a tick as it changes
@property UILabel *holdCallout;
@property BOOL holdArmed;
@property UIImpactFeedbackGenerator *holdFeedback;
@property UISelectionFeedbackGenerator *holdChangeFeedback;
// The on-screen keyboard up (keyboardMoved:)
@property BOOL keyboardUp;
// The layout updateMonitorColumn last logged
@property (nullable) NSString *monitorColumnLayout;

@property BOOL ignoreKeyboardMotion;
// How much of the view's bottom the keyboard and the keys over it cover (0
// with them away): the terminal's bottom is above it, and above the home
// indicator (applyBottomPad)
@property CGFloat keyboardPad;
// The last keyboard notice not yet acted on, and whether a turn of the run
// loop is to act on it (keyboardDidSomething:)
@property (nullable) NSNotification *pendingKeyboardNotice;
@property BOOL keyboardNoticeScheduled;
@property (nonatomic) BOOL hasExternalKeyboard;

@end

// The bar's arrow keys carry their direction in the storyboard tag:
// 1 up, 2 down, 3 left, 4 right. These are the CSI final bytes.
static const char arrowKeyFinals[] = {[1] = 'A', [2] = 'B', [3] = 'D', [4] = 'C'};

// The symbol key: tapped, /; held, the symbols a terminal takes that the
// keyboard keeps on its 123 and #+= pages, the most used in the row nearest
// the key. Held and let go without moving, / still.
static NSString *const kSymbolKeySymbol = @"/";
static NSArray<NSArray<NSString *> *> *HeldSymbols(void) {
    return @[
        @[@"|", @"-", @"~", @"_", @"\\", @"*", @">", @"$"],
        @[@"&", @"<", @";", @":", @"=", @"+", @"\"", @"'"],
        @[@"`", @"{", @"}", @"[", @"]", @"#", @"%", @"^"],
    ];
}

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

#pragma mark - Find bar

// Find in what the terminal holds (⌘F, the commands menu's Find): a field
// over the terminal's top, at its right on a wide screen; which match of how
// many is found; the previous and the next; Done (or Esc) puts it away.
// Return finds the next, Shift-Return the previous.
@interface FindBar : UIVisualEffectView <UITextFieldDelegate>
@property (readonly) UITextField *field;
@property (readonly) UILabel *countLabel;
// What it asks of the terminal: a find, and to be put away
@property (copy) void (^find)(NSString *text, BOOL backwards, BOOL incremental);
@property (copy) void (^done)(void);
- (void)showIndex:(NSInteger)index count:(NSInteger)count;
@end

@implementation FindBar

- (instancetype)init {
    if (self = [super initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemMaterial]]) {
        self.layer.cornerRadius = 12;
        self.clipsToBounds = YES;

        _field = [UITextField new];
        _field.placeholder = @"Find";
        _field.autocapitalizationType = UITextAutocapitalizationTypeNone;
        _field.autocorrectionType = UITextAutocorrectionTypeNo;
        _field.spellCheckingType = UITextSpellCheckingTypeNo;
        _field.smartQuotesType = UITextSmartQuotesTypeNo;
        _field.smartDashesType = UITextSmartDashesTypeNo;
        _field.returnKeyType = UIReturnKeySearch;
        _field.clearButtonMode = UITextFieldViewModeWhileEditing;
        UIImageView *glass = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"magnifyingglass"]];
        glass.tintColor = UIColor.secondaryLabelColor;
        _field.leftView = glass;
        _field.leftViewMode = UITextFieldViewModeAlways;
        _field.delegate = self;
        [_field addTarget:self action:@selector(textChanged:) forControlEvents:UIControlEventEditingChanged];

        _countLabel = [UILabel new];
        _countLabel.font = [UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightRegular];
        _countLabel.textColor = UIColor.secondaryLabelColor;
        [_countLabel setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [_countLabel setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];

        UIButton *previous = [self buttonWithSymbol:@"chevron.up" label:@"Previous" action:@selector(previousPressed:)];
        UIButton *next = [self buttonWithSymbol:@"chevron.down" label:@"Next" action:@selector(nextPressed:)];
        UIButton *done = [UIButton buttonWithType:UIButtonTypeSystem];
        [done setTitle:@"Done" forState:UIControlStateNormal];
        done.titleLabel.font = [UIFont systemFontOfSize:17 weight:UIFontWeightSemibold];
        [done addTarget:self action:@selector(donePressed:) forControlEvents:UIControlEventPrimaryActionTriggered];
        [done setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        [done setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];

        UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[_field, _countLabel, previous, next, done]];
        row.alignment = UIStackViewAlignmentCenter;
        row.spacing = 4;
        [row setCustomSpacing:10 afterView:_countLabel];
        [row setCustomSpacing:10 afterView:next];
        row.translatesAutoresizingMaskIntoConstraints = NO;
        [self.contentView addSubview:row];
        [NSLayoutConstraint activateConstraints:@[
            [row.leadingAnchor constraintEqualToAnchor:self.contentView.leadingAnchor constant:12],
            [row.trailingAnchor constraintEqualToAnchor:self.contentView.trailingAnchor constant:-12],
            [row.topAnchor constraintEqualToAnchor:self.contentView.topAnchor],
            [row.bottomAnchor constraintEqualToAnchor:self.contentView.bottomAnchor],
            [row.heightAnchor constraintEqualToConstant:44],
            [_field.heightAnchor constraintEqualToAnchor:row.heightAnchor],
        ]];
    }
    return self;
}

- (UIButton *)buttonWithSymbol:(NSString *)symbol label:(NSString *)label action:(SEL)action {
    UIButton *button = [UIButton buttonWithType:UIButtonTypeSystem];
    [button setImage:[UIImage systemImageNamed:symbol] forState:UIControlStateNormal];
    button.accessibilityLabel = label;
    [button addTarget:self action:action forControlEvents:UIControlEventPrimaryActionTriggered];
    [NSLayoutConstraint activateConstraints:@[
        [button.widthAnchor constraintEqualToConstant:36],
        [button.heightAnchor constraintEqualToConstant:44],
    ]];
    return button;
}

- (void)textChanged:(UITextField *)field {
    self.find(field.text, NO, YES);
}
- (BOOL)textFieldShouldReturn:(UITextField *)field {
    self.find(field.text, NO, NO);
    return NO;
}
- (void)previousPressed:(id)sender {
    self.find(self.field.text, YES, NO);
}
- (void)nextPressed:(id)sender {
    self.find(self.field.text, NO, NO);
}
- (void)donePressed:(id)sender {
    self.done();
}

- (NSArray<UIKeyCommand *> *)keyCommands {
    UIKeyCommand *escape = [UIKeyCommand keyCommandWithInput:UIKeyInputEscape modifierFlags:0 action:@selector(donePressed:)];
    UIKeyCommand *previous = [UIKeyCommand keyCommandWithInput:@"\r" modifierFlags:UIKeyModifierShift action:@selector(previousPressed:)];
    escape.wantsPriorityOverSystemBehavior = YES;
    previous.wantsPriorityOverSystemBehavior = YES;
    return @[escape, previous];
}

// "3/12"; "0/0" for no match; nothing before there is text to find. The
// matches are counted up to the first 1000.
- (void)showIndex:(NSInteger)index count:(NSInteger)count {
    if (self.field.text.length == 0) {
        self.countLabel.text = nil;
        return;
    }
    self.countLabel.text = [NSString stringWithFormat:@"%ld/%ld%@", (long) index + 1, (long) count, count >= 1000 ? @"+" : @""];
}

@end

#pragma mark - TerminalViewController

@implementation TerminalViewController

- (void)viewDidLoad {
    [super viewDidLoad];

    int bootError = [AppDelegate bootError];
    if (bootError < 0) {
        NSString *message = [NSString stringWithFormat:@"could not boot"];
        NSString *subtitle = [NSString stringWithFormat:@"error code %d", bootError];
        if (bootError == _EINVAL)
            subtitle = [subtitle stringByAppendingString:@"\n(try reinstalling the app, see release notes for details)"];
        [self showMessage:message subtitle:subtitle];
        NSLog(@"boot failed with code %d", bootError);
    }

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

    // The meters, while Settings or the View menu have them shown: in the
    // column right of the terminal where the window has one, or else in what
    // the terminal leaves right of its last column (updateMonitorColumn). A
    // tap on them (VoiceOver's activation) shows their details.
    self.monitorSampler = [MonitorSampler new];
    self.monitorStrip = [[MonitorStripView alloc] initWithSampler:self.monitorSampler];
    MonitorColumnView *column = [[MonitorColumnView alloc] initWithSampler:self.monitorSampler];
    column.translatesAutoresizingMaskIntoConstraints = NO;
    column.hidden = YES;
    [self.view addSubview:column];
    self.monitorColumn = column;
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    self.monitorColumnTop = [column.topAnchor constraintEqualToAnchor:self.view.topAnchor];
    [NSLayoutConstraint activateConstraints:@[
        [column.leadingAnchor constraintEqualToAnchor:safe.trailingAnchor],
        [column.trailingAnchor constraintEqualToAnchor:self.view.trailingAnchor],
        self.monitorColumnTop,
    ]];
    [self addColumnKeys];
    __weak typeof(self) weakSelf = self;
    self.termView.marginTapped = self.monitorStrip.activated = ^{
        [weakSelf showMeterDetailsFrom:weakSelf.monitorStrip];
    };
    column.activated = ^{
        [weakSelf showMeterDetailsFrom:weakSelf.monitorColumn];
    };
    [self applyShowMeters];

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


    [self addSymbolKey];
    [self addHideKeyboardKey];
    [self _updateStyleFromPreferences:NO];
    
    // Two fingers across the terminal: the next tab, or the previous one
    for (NSNumber *direction in @[@(UISwipeGestureRecognizerDirectionLeft), @(UISwipeGestureRecognizerDirectionRight)]) {
        UISwipeGestureRecognizer *swipe = [[UISwipeGestureRecognizer alloc] initWithTarget:self action:@selector(swipedAcrossTerminal:)];
        swipe.direction = direction.unsignedIntegerValue;
        swipe.numberOfTouchesRequired = 2;
        swipe.delegate = self;
        [self.termView addGestureRecognizer:swipe];
    }

    // The bar is a row of flat keys, a point apart: esc, ctrl, tab and / from
    // the left edge, the four arrows, paste and hide keyboard to the right one,
    // the room a wide screen leaves between them (the spacer, in the
    // storyboard). 42 points tall on a phone (50 on an iPad) and about as wide
    // (resizeBar). esc and ctrl are named in lowercase; tab, paste, hide
    // keyboard and the arrows are drawn.
    bool phone = UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone;
    self.barHeight.constant = phone ? 42 : 50;
    CGFloat keyFontSize = phone ? 18 : 21;

    // A key is 39 points wide on a 320-point screen: a name shrinks to fit.
    NSDictionary<NSString *, UIButton *> *namedKeys = @{
        @"esc": self.escapeKey,
        @"ctrl": self.controlKey,
        kSymbolKeySymbol: self.symbolKey,
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
        @"doc.on.clipboard": self.pasteButton,
        @"keyboard.chevron.compact.down": self.hideKeyboardKey,
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
            // the column's meters under the status bar, or at its top
            dispatch_async(dispatch_get_main_queue(), ^{
                [self updateMonitorColumn];
            });
        });
    }];
    [UserPreferences.shared observe:@[@"showMeters"] options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self applyShowMeters];
        });
    }];
    [UserPreferences.shared observe:@[@"colorScheme", @"theme", @"hideExtraKeysWithExternalKeyboard"]
                            options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateStyleFromPreferences:YES];
        });
    }];
    [self _updateBadge];

    self.termView.pasteInterceptor = ^BOOL {
        return [weakSelf pasteFromStack];
    };
    // The paste key is iOS's paste button laid over it (updatePasteControl),
    // which pastes through pasteItemProviders:. While the Paste Stack has
    // items the button is hidden and the key pastes the next one itself: the
    // stack is iSH's own, and iOS's button is enabled only while the clipboard
    // has text, which it need not have then (pasteKeyPastesStack).
    self.pasteConfiguration = [[UIPasteConfiguration alloc] initWithTypeIdentifiersForAcceptingClass:NSString.class];
    self.pasteButton.accessibilityLabel = @"Paste";
    [self.pasteButton addTarget:self action:@selector(pasteKey:) forControlEvents:UIControlEventPrimaryActionTriggered];
    [center addObserver:self selector:@selector(pasteStackDidChange:) name:ClipboardStackDidChangeNotification object:nil];
    [self pasteStackDidChange:nil];

    // Tab held: Shift-Tab
    UILongPressGestureRecognizer *holdTab = [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(tabKeyHeld:)];
    [self.tabKey addGestureRecognizer:holdTab];
    self.tabKey.accessibilityCustomActions = @[[[UIAccessibilityCustomAction alloc] initWithName:@"Shift-Tab" actionHandler:^BOOL(UIAccessibilityCustomAction *_) {
        [weakSelf.termView insertSequence:@"\x1b[Z"];
        return YES;
    }]];
    // A key clicks as it goes down, as the keyboard's do with their clicks on
    // in Settings (BarView's enableInputClicksWhenVisible)
    for (UIButton *key in self.barButtons)
        [key addTarget:self action:@selector(barKeyDown:) forControlEvents:UIControlEventTouchDown];
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
}

- (void)viewDidAppear:(BOOL)animated {
    [AppDelegate maybeOfferRootResetOnViewController:self];
    [super viewDidAppear:animated];
    // the status bar's frame is the window's scene's, which it has now
    [self updateMonitorColumn];
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

- (BOOL)showSessionWithUUID:(NSUUID *)uuid {
    for (TerminalSession *session in self.tabs) {
        if ([session.uuid isEqual:uuid]) {
            self.selectedSession = session;
            return YES;
        }
    }
    return NO;
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

// Another tab, the keyboard as it was: put down, it stays down (a tap on the
// terminal brings it back); up, it stays up. A new shell (a new tab, New Tab
// Here, Restart) brings it up, to type in.
- (void)tabBar:(TabBarView *)tabBar didSelectTabAtIndex:(NSUInteger)index {
    [self selectTabAtIndex:index];
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
    // the keyboard back after, if it was up before
    BOOL typing = self.termView.isFirstResponder;
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
        if (typing)
            [weakSelf.termView becomeFirstResponder];
    }];
    [alert addAction:rename];
    alert.preferredAction = rename; // Return in the text field confirms
    [self presentViewController:alert animated:YES completion:nil];
}


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
        self.termView.keyboardAppearance = UserPreferences.shared.keyboardAppearance;
        // The keys in the theme's colors, as the tab strip is: its background
        // nudged toward its foreground, more so pressed
        UIColor *background = self.view.backgroundColor;
        UIColor *foreground = [[UIColor alloc] ish_initWithHexString:UserPreferences.shared.palette.foregroundColor];
        for (BarButton *button in self.barButtons) {
            [button setKeyColor:[background ish_blendedWithColor:foreground fraction:0.18]
                   pressedColor:[background ish_blendedWithColor:foreground fraction:0.36]
                      textColor:foreground];
        }
        self.monitorStrip.foregroundColor = foreground;
        self.monitorColumn.foregroundColor = foreground;
        for (BarButton *key in self.columnKeys.arrangedSubviews) {
            [key setKeyColor:[background ish_blendedWithColor:foreground fraction:0.18]
                pressedColor:[background ish_blendedWithColor:foreground fraction:0.36]
                   textColor:foreground];
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
    self.keyboardPad = pad;
    // The on-screen keyboard up (100 points of it or more over the terminal,
    // as hasExternalKeyboard tells it from the bar of extra keys alone, or a
    // floating keyboard's): the column keeps the CPU and the memory only, and
    // of its keys esc and tab, over the keyboard where the memory leaves them
    // the room; the bar has them all
    self.keyboardUp = pad >= 100;
    self.monitorColumn.keyboardUp = self.keyboardUp;
    // and the meters' details, open, the CPUs and the memory only then
    if ([self.presentedViewController isKindOfClass:MonitorDetailViewController.class])
        ((MonitorDetailViewController *) self.presentedViewController).keyboardUp = self.keyboardUp;
    [self updateColumnKeys];
    // Nothing to move: no layout, which would lay out the keyboard's window
    if (![self applyBottomPad])
        return;

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

// The terminal's bottom: above the keyboard, and with it away (put down, or
// none on screen) above the home indicator, whose bar would be over the last
// line. Whether that moved it. The column's keys, the keyboard up, are 8
// points above the same (the keyboard's window over them hid them), the
// meters over them; they move with the terminal, in the layout pass that
// moves it.
- (BOOL)applyBottomPad {
    CGFloat pad = MAX(self.keyboardPad, self.view.safeAreaInsets.bottom);
    if (self.columnKeysAboveKeyboard.constant != -(pad + 8))
        self.columnKeysAboveKeyboard.constant = -(pad + 8);
    if (self.bottomConstraint.constant == pad)
        return NO;
    self.bottomConstraint.constant = pad;
    return YES;
}

// A rotation, or the phone folded or unfolded, changes the home indicator's room
- (void)viewSafeAreaInsetsDidChange {
    [super viewSafeAreaInsetsDidChange];
    [self applyBottomPad];
    [self updateMonitorColumn];
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
    [super traitCollectionDidChange:previousTraitCollection];
    [self updateMonitorColumn];
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
    // as narrow as a phone; narrower if the row is (38 on a 393-point iPhone,
    // 31 on a 320-point iPhone SE). Every arranged view but the spacer between
    // / and the arrows is a key.
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

// A key of the bar's made here rather than in the storyboard, at index of the
// row, as wide as the others
- (BarButton *)addBarKeyAtIndex:(NSUInteger)index {
    BarButton *key = [[BarButton alloc] initWithFrame:CGRectZero];
    key.translatesAutoresizingMaskIntoConstraints = NO;
    [self.bar insertArrangedSubview:key atIndex:index];
    [key.widthAnchor constraintEqualToAnchor:self.pasteButton.widthAnchor].active = YES;
    self.barButtons = [self.barButtons arrayByAddingObject:key];
    return key;
}

- (void)addSymbolKey {
    BarButton *key = [self addBarKeyAtIndex:[self.bar.arrangedSubviews indexOfObject:self.tabKey] + 1];
    key.accessibilityLabel = @"Slash";
    [key addTarget:self action:@selector(pressSymbolKey:) forControlEvents:UIControlEventPrimaryActionTriggered];
    UILongPressGestureRecognizer *hold = [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(symbolKeyHeld:)];
    hold.minimumPressDuration = 0.4;
    [key addGestureRecognizer:hold];
    // VoiceOver: each symbol an action of the key
    __weak typeof(self) weakSelf = self;
    NSMutableArray<UIAccessibilityCustomAction *> *actions = [NSMutableArray new];
    for (NSArray<NSString *> *row in HeldSymbols()) {
        for (NSString *symbol in row) {
            [actions addObject:[[UIAccessibilityCustomAction alloc] initWithName:symbol actionHandler:^BOOL(UIAccessibilityCustomAction *_) {
                [weakSelf pressKey:symbol];
                return YES;
            }]];
        }
    }
    key.accessibilityCustomActions = actions;
    self.symbolKey = key;
}

// The keyboard put down, these keys with it; a tap on the terminal brings
// them back
- (void)addHideKeyboardKey {
    BarButton *key = [self addBarKeyAtIndex:self.bar.arrangedSubviews.count];
    key.accessibilityLabel = @"Hide Keyboard";
    [key addTarget:self.termView action:@selector(loseFocus:) forControlEvents:UIControlEventPrimaryActionTriggered];
    self.hideKeyboardKey = key;
}

- (void)pressSymbolKey:(id)sender {
    [self pressKey:kSymbolKeySymbol];
}

- (void)symbolKeyHeld:(UILongPressGestureRecognizer *)hold {
    switch (hold.state) {
        case UIGestureRecognizerStateBegan:
            self.symbolPopup = [[KeyPopupView alloc] initWithRows:HeldSymbols()];
            [self.symbolPopup showOverKey:self.symbolKey within:self.bar];
            [self.symbolPopup track:[hold locationInView:self.symbolPopup.superview]];
            break;
        case UIGestureRecognizerStateChanged:
            [self.symbolPopup track:[hold locationInView:self.symbolPopup.superview]];
            break;
        case UIGestureRecognizerStateEnded: {
            KeyPopupView *popup = self.symbolPopup;
            NSString *symbol = popup.moved ? popup.chosen : kSymbolKeySymbol;
            [popup removeFromSuperview];
            self.symbolPopup = nil;
            if (symbol != nil) {
                [UIDevice.currentDevice playInputClick];
                [self pressKey:symbol];
            }
            break;
        }
        default:
            [self.symbolPopup removeFromSuperview];
            self.symbolPopup = nil;
            break;
    }
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

- (void)barKeyDown:(UIButton *)key {
    [UIDevice.currentDevice playInputClick];
}

// Shift-Tab, as xterm sends it: claude's next mode, a completion menu's
// previous entry. The key's own tab is not sent: the hold took its touch.
- (void)tabKeyHeld:(UILongPressGestureRecognizer *)recognizer {
    if (recognizer.state != UIGestureRecognizerStateBegan)
        return;
    [UIDevice.currentDevice playInputClick];
    [self.termView insertSequence:@"\x1b[Z"];
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

- (IBAction)pressReturn:(id)sender {
    [self pressKey:@"\r"];
}

#pragma mark The column's keys

// Esc, tab, /, up and down (held: repeating) and return, as the bar's keys
// send them and in its order, one over another at the bottom of the column,
// under the meters' middle, above the home indicator. Esc and / are named as
// on the bar. Held, esc is backspace, tab shift-tab and / Ctrl-U (clearing
// the line), each said in a callout before it is let go (columnKeyHeld:);
// the bar's / keeps its held symbols, whose popup opens over its row.
- (void)addColumnKeys {
    __weak typeof(self) weakSelf = self;
    BarButton *escape = [self columnKeyNamed:@"esc" name:NSLocalizedString(@"Escape", @"The column's escape key, read aloud")];
    [escape addTarget:self action:@selector(pressEscape:) forControlEvents:UIControlEventTouchUpInside];
    [self holdKey:escape
          callout:[@"⌫ " stringByAppendingString:NSLocalizedString(@"Backspace", @"The column's esc key held: what it sends")]
             send:^{ [weakSelf.termView deleteBackward]; }];
    BarButton *tab = [self columnKey:@"arrow.right.to.line" name:NSLocalizedString(@"Tab", @"The column's tab key, read aloud")];
    [tab addTarget:self action:@selector(pressTab:) forControlEvents:UIControlEventTouchUpInside];
    [self holdKey:tab
          callout:[@"⇧⇥ " stringByAppendingString:NSLocalizedString(@"Shift-Tab", @"The column's tab key held: what it sends")]
             send:^{ [weakSelf.termView insertSequence:@"\x1b[Z"]; }];
    BarButton *slash = [self columnKeyNamed:kSymbolKeySymbol name:NSLocalizedString(@"Slash", @"The column's slash key, read aloud")];
    [slash addTarget:self action:@selector(pressSymbolKey:) forControlEvents:UIControlEventTouchUpInside];
    [self holdKey:slash
          callout:[@"⌃U " stringByAppendingString:NSLocalizedString(@"Clear the line", @"The column's / key held: what it sends (Ctrl-U)")]
             send:^{ [weakSelf.termView insertSequence:@"\x15"]; }];
    BarButton *up = [self columnKey:@"chevron.up" name:NSLocalizedString(@"Up", @"The column's up arrow key, read aloud")];
    up.tag = 1; // arrowKeyFinals
    BarButton *down = [self columnKey:@"chevron.down" name:NSLocalizedString(@"Down", @"The column's down arrow key, read aloud")];
    down.tag = 2;
    for (BarButton *arrow in @[up, down]) {
        [arrow addTarget:self action:@selector(arrowKeyDown:) forControlEvents:UIControlEventTouchDown];
        [arrow addTarget:self action:@selector(arrowKeyUp:)
            forControlEvents:UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];
    }
    BarButton *enter = [self columnKey:@"return" name:NSLocalizedString(@"Return", @"The column's return key, read aloud")];
    [enter addTarget:self action:@selector(pressReturn:) forControlEvents:UIControlEventTouchUpInside];

    UIStackView *keys = [[UIStackView alloc] initWithArrangedSubviews:@[escape, tab, slash, up, down, enter]];
    keys.axis = UILayoutConstraintAxisVertical;
    keys.spacing = 8;
    keys.translatesAutoresizingMaskIntoConstraints = NO;
    keys.hidden = YES;
    [self.view addSubview:keys];
    self.columnKeys = keys;
    self.columnKeysKeyboardDown = @[slash, up, down, enter];
    // The meters' room is the column's, down to the keys: the rows that fit
    // in it show (MonitorColumnView's rowsForRoom:)
    [self.monitorColumn.bottomAnchor constraintEqualToAnchor:keys.topAnchor constant:-8].active = YES;
    self.columnKeysAtBottom = [keys.bottomAnchor constraintEqualToAnchor:self.view.safeAreaLayoutGuide.bottomAnchor
                                                                constant:-8];
    // as the terminal's bottom is (applyBottomPad)
    self.columnKeysAboveKeyboard = [keys.bottomAnchor constraintEqualToAnchor:self.view.bottomAnchor];
    [keys.centerXAnchor constraintEqualToAnchor:self.monitorColumn.metersCenterXAnchor].active = YES;
    self.columnKeyFeedback = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
    self.holdFeedback = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleMedium];
    self.holdChangeFeedback = [UISelectionFeedbackGenerator new];
}

// A column key's second key, sent by holding it (columnKeyHeld:) and, with
// VoiceOver, by its action of that name
- (void)holdKey:(BarButton *)key callout:(NSString *)callout send:(void (^)(void))send {
    ColumnKeyHold *hold = [[ColumnKeyHold alloc] initWithTarget:self action:@selector(columnKeyHeld:)];
    hold.minimumPressDuration = 0.4;
    hold.callout = callout;
    hold.send = send;
    [key addGestureRecognizer:hold];
    key.accessibilityCustomActions = @[[[UIAccessibilityCustomAction alloc] initWithName:callout actionHandler:^BOOL(UIAccessibilityCustomAction *_) {
        send();
        return YES;
    }]];
}

// Held, a key's callout shows left of it what letting go sends; the key's
// own key is not sent (the hold took its touch). Moved off the key and the
// callout, the finger lets go of nothing ("Release to cancel"), and back on
// them of the second key again: a hold that was a mistake is undone.
- (void)columnKeyHeld:(ColumnKeyHold *)hold {
    UIView *key = hold.view;
    CGRect keyFrame = [self.view convertRect:key.bounds fromView:key];
    switch (hold.state) {
        case UIGestureRecognizerStateBegan: {
            UILabel *callout = [UILabel new];
            callout.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
            callout.textAlignment = NSTextAlignmentCenter;
            callout.backgroundColor = UIColor.secondarySystemBackgroundColor;
            callout.layer.cornerRadius = 10;
            callout.layer.masksToBounds = YES;
            [self.view addSubview:callout];
            self.holdCallout = callout;
            self.holdArmed = YES;
            [self showHoldCallout:hold.callout armed:YES besideKey:keyFrame];
            [self.holdFeedback impactOccurred];
            break;
        }
        case UIGestureRecognizerStateChanged: {
            CGPoint at = [hold locationInView:self.view];
            BOOL armed = CGRectContainsPoint(CGRectInset(keyFrame, -24, -24), at) ||
                CGRectContainsPoint(CGRectInset(self.holdCallout.frame, -16, -16), at);
            if (armed == self.holdArmed)
                break;
            self.holdArmed = armed;
            [self showHoldCallout:armed ? hold.callout : NSLocalizedString(@"Release to cancel", @"A held column key's callout, the finger moved off it: nothing is sent")
                            armed:armed besideKey:keyFrame];
            [self.holdChangeFeedback selectionChanged];
            break;
        }
        case UIGestureRecognizerStateEnded:
            if (self.holdArmed) {
                [UIDevice.currentDevice playInputClick];
                hold.send();
            }
            [self.holdCallout removeFromSuperview];
            self.holdCallout = nil;
            break;
        default:
            [self.holdCallout removeFromSuperview];
            self.holdCallout = nil;
            break;
    }
}

// The callout 8 points left of the key, level with it, as wide as its text
- (void)showHoldCallout:(NSString *)text armed:(BOOL)armed besideKey:(CGRect)keyFrame {
    UILabel *callout = self.holdCallout;
    callout.text = text;
    callout.textColor = armed ? UIColor.labelColor : UIColor.secondaryLabelColor;
    CGSize size = [callout sizeThatFits:CGSizeMake(CGFLOAT_MAX, CGFLOAT_MAX)];
    CGFloat width = size.width + 24, height = 36;
    callout.frame = CGRectMake(CGRectGetMinX(keyFrame) - 8 - width, CGRectGetMidY(keyFrame) - height / 2, width, height);
}

- (BarButton *)columnKey:(NSString *)symbol name:(NSString *)name {
    BarButton *key = [self columnKeyNamed:nil name:name];
    [key setImage:[UIImage systemImageNamed:symbol] forState:UIControlStateNormal];
    [key setPreferredSymbolConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:15
                                                                                         weight:UIImageSymbolWeightMedium]
                         forImageInState:UIControlStateNormal];
    return key;
}

// A key with its name on it (title, as the bar's esc and /), or nil: drawn
- (BarButton *)columnKeyNamed:(NSString *)title name:(NSString *)name {
    BarButton *key = [[BarButton alloc] initWithFrame:CGRectZero];
    key.translatesAutoresizingMaskIntoConstraints = NO;
    if (title != nil) {
        [key setTitle:title forState:UIControlStateNormal];
        key.titleLabel.font = [UIFont systemFontOfSize:15 weight:UIFontWeightMedium];
    }
    key.accessibilityLabel = name;
    [key addTarget:self action:@selector(columnKeyDown:) forControlEvents:UIControlEventTouchDown];
    // Round, as the meters over them are rings: 40 points across, a filled
    // circle looking as large as their 44-point rings
    key.layer.cornerRadius = 20;
    [NSLayoutConstraint activateConstraints:@[
        [key.widthAnchor constraintEqualToConstant:40],
        [key.heightAnchor constraintEqualToConstant:40],
    ]];
    return key;
}

- (void)columnKeyDown:(UIButton *)key {
    [self.columnKeyFeedback impactOccurred];
}

// Shown while the column is, at its bottom, the meters over them: with the
// keyboard down all six, above the home indicator; up esc and tab, above the
// keyboard's bar, while the CPU and the memory have the room over them (the
// bar has them too, folded or with the column hidden their only place; it
// keeps them, so that its keys do not move as the phone unfolds). Up, they
// were under the memory, the column's last meter then, as far from the
// keyboard as the meters had room for. Folded, the column has not the room
// for both from the status bar's region (174) to the bar (371): it squashed
// the memory, and the bar's own esc and tab are under the left thumb there;
// unfolded, the bar's esc is the width of the screen from the right one.
- (void)updateColumnKeys {
    BOOL shown = !self.monitorColumn.hidden;
    BOOL up = self.keyboardUp;
    if (self.columnKeys.hidden == shown)
        self.columnKeys.hidden = !shown;
    NSMutableArray<UIView *> *upKeys = [self.columnKeys.arrangedSubviews mutableCopy];
    [upKeys removeObjectsInArray:self.columnKeysKeyboardDown];
    BOOL upKeysFit = up && [self columnHasRoomForKeys:upKeys];
    for (UIView *key in self.columnKeys.arrangedSubviews) {
        BOOL hidden = up && !(upKeysFit && [upKeys containsObject:key]);
        if (key.hidden != hidden)
            key.hidden = hidden;
    }
    // Hidden, at the bottom still (a place of its own). Only what changes, as
    // everything updateMonitorColumn sets, the one to go first, so that both
    // never hold at once: it runs in updateViewConstraints, where a constraint
    // set may ask for the constraints again (814 set them every time).
    NSLayoutConstraint *on = shown && up ? self.columnKeysAboveKeyboard : self.columnKeysAtBottom;
    NSLayoutConstraint *off = on == self.columnKeysAtBottom ? self.columnKeysAboveKeyboard : self.columnKeysAtBottom;
    if (off.active)
        off.active = NO;
    if (!on.active)
        on.active = YES;
}

// Whether the column, the keyboard up, has the room for these keys under its
// CPU and memory: from where the meters begin (updateMonitorColumn) to 8
// points over the keyboard (applyBottomPad), 8 between the meters and the
// keys (addColumnKeys)
- (BOOL)columnHasRoomForKeys:(NSArray<UIView *> *)keys {
    CGFloat height = self.monitorColumn.leastHeight + 8 - self.columnKeys.spacing;
    for (UIView *key in keys)
        height += self.columnKeys.spacing + [key systemLayoutSizeFittingSize:UILayoutFittingCompressedSize].height;
    CGFloat bottom = self.view.bounds.size.height - MAX(self.keyboardPad, self.view.safeAreaInsets.bottom) - 8;
    return self.monitorColumnTop.constant + height <= bottom;
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
    [self.termView changeFontSize:UserPreferences.shared.fontSize.intValue + 1];
}
- (void)decreaseFontSize:(id)sender {
    [self.termView changeFontSize:UserPreferences.shared.fontSize.intValue - 1];
}
- (void)resetFontSize:(id)sender {
    [self.termView changeFontSize:kDefaultFontSize];
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
        // ⌘9 the last tab: listed when there are more than eight, else
        // the last listed is it
        NSUInteger number = [command.propertyList unsignedIntegerValue];
        NSUInteger index = number == 8 && count > 8 ? count - 1 : number;
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
    } else if (action == @selector(toggleMeters:)) {
        command.state = prefs.showMeters ? UIMenuElementStateOn : UIMenuElementStateOff;
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

- (BOOL)finding {
    return self.findBar != nil && !self.findBar.hidden;
}

// The find bar, its field taking the keyboard; its text selected, so that
// what is typed finds anew
- (void)showFind:(id)sender {
    if (self.findBar == nil) {
        FindBar *bar = [FindBar new];
        bar.translatesAutoresizingMaskIntoConstraints = NO;
        __weak typeof(self) weakSelf = self;
        bar.find = ^(NSString *text, BOOL backwards, BOOL incremental) {
            [weakSelf.termView find:text backwards:backwards incremental:incremental];
        };
        bar.done = ^{
            [weakSelf hideFind];
        };
        self.termView.findResults = ^(NSInteger index, NSInteger count) {
            [weakSelf.findBar showIndex:index count:count];
        };
        [self.view addSubview:bar];
        UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
        // The width of the screen less a margin on a phone; at the right, no
        // wider than a phone's, on a wide one (unfolded, an iPad)
        NSLayoutConstraint *width = [bar.widthAnchor constraintEqualToConstant:420];
        width.priority = UILayoutPriorityDefaultHigh;
        [NSLayoutConstraint activateConstraints:@[
            [bar.topAnchor constraintEqualToAnchor:self.tabBar.bottomAnchor constant:8],
            [bar.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-8],
            [bar.leadingAnchor constraintGreaterThanOrEqualToAnchor:safe.leadingAnchor constant:8],
            width,
        ]];
        self.findBar = bar;
    }
    FindBar *bar = self.findBar;
    bar.overrideUserInterfaceStyle = UserPreferences.shared.requestingDarkAppearance ? UIUserInterfaceStyleDark : UIUserInterfaceStyleLight;
    bar.hidden = NO;
    [self.view bringSubviewToFront:bar];
    [bar.field becomeFirstResponder];
    dispatch_async(dispatch_get_main_queue(), ^{
        [bar.field selectAll:nil];
    });
}

// ⌘G and ⇧⌘G: again, with what the find bar has; the find bar if it has nothing
- (void)findNextMatch:(id)sender {
    [self findAgainBackwards:NO];
}
- (void)findPreviousMatch:(id)sender {
    [self findAgainBackwards:YES];
}
- (void)findAgainBackwards:(BOOL)backwards {
    if (!self.finding || self.findBar.field.text.length == 0) {
        [self showFind:nil];
        return;
    }
    [self.termView find:self.findBar.field.text backwards:backwards incremental:NO];
}

// Done, or another tab: the highlights go, and the keyboard, if the find
// bar had it, goes back to the terminal; put down, it stays down
- (void)hideFind {
    if (!self.finding)
        return;
    BOOL typing = self.findBar.field.isFirstResponder;
    [self.termView clearFind];
    self.findBar.hidden = YES;
    if (typing)
        [self.termView becomeFirstResponder];
}

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
    if (self.pasteControl != nil && [self.pasteControlKeyColor isEqual:key.keyColor] &&
            [self.pasteControlTextColor isEqual:key.tintColor])
        return;
    [self.pasteControl removeFromSuperview];
    UIPasteControlConfiguration *config = [UIPasteControlConfiguration new];
    config.displayMode = UIPasteControlDisplayModeIconOnly;
    // flat and square, in the key's colors
    config.cornerStyle = UIButtonConfigurationCornerStyleFixed;
    config.cornerRadius = 0;
    config.baseBackgroundColor = key.keyColor;
    config.baseForegroundColor = key.tintColor;
    UIPasteControl *control = [[UIPasteControl alloc] initWithConfiguration:config];
    control.target = self;
    control.hidden = [self pasteKeyPastesStack];
    control.translatesAutoresizingMaskIntoConstraints = NO;
    [key addSubview:control];
    [NSLayoutConstraint activateConstraints:@[
        [control.leadingAnchor constraintEqualToAnchor:key.leadingAnchor],
        [control.trailingAnchor constraintEqualToAnchor:key.trailingAnchor],
        [control.topAnchor constraintEqualToAnchor:key.topAnchor],
        [control.bottomAnchor constraintEqualToAnchor:key.bottomAnchor],
    ]];
    self.pasteControl = control;
    self.pasteControlKeyColor = key.keyColor;
    self.pasteControlTextColor = key.tintColor;
}

// What iOS's paste button pastes (updatePasteControl): the first item's text,
// as paste: pastes the clipboard's string. The button is hidden while the
// Paste Stack has items (pasteKey:).
- (void)pasteItemProviders:(NSArray<NSItemProvider *> *)itemProviders {
    // another app's copy, taken into the clipboard's history as it is pasted
    [ClipboardStore.shared notePastedItemProviders:itemProviders];
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
- (void)toggleMeters:(id)sender {
    UserPreferences.shared.showMeters = !UserPreferences.shared.showMeters;
}

// Off, the meters leave the window, and read nothing (MonitorStripView). The
// room they had stays the scroll bar's: the terminal keeps its columns.
- (void)applyShowMeters {
    [self updateMonitorColumn];
}

// A phone that folds has the window leave a column right of the terminal for
// its status bar, folded or not, its room under the status bar empty: the
// safe area's right inset, wide, with little or none on the left (a phone on
// its side leaves the same room on both sides, round its camera). The meters
// there go right under what the system has in the column (its camera, the
// status bar's time, Wi-Fi and battery, and the indicators it shows then), as
// UIKit says where that ends (iOS 27.1): the occlusion regions at the column's
// top (reservedRegionsOfKind:), and where a bar on the right edge as wide as
// the column begins (layoutRegionForBarOnEdge:extent:), the lowest of them,
// and 8 points under the status bar's frame where that is the column's. The
// regions' frames have their margins in them: 8 more under them were 31
// points under the status bar's dots folded (the column's region {{382, 0},
// {84, 174}}, the dots ending at 151, the bar's insets' top 174 too). Not
// under the dots: the room under them is the indicators' the status bar shows
// there, which a status bar end measured from its screenshots (151 points
// folded) had the meters cover. The status bar's frame alone says nothing: it
// is the window's top, 2 points high, on that phone. With the status bar
// hidden, or nothing said of where it ends, 8 points under the tab bar. Each
// layout is logged, and kept for
// /proc/ish/defaults/meters_column, with all it was laid out by, to place them
// by on the phone. Called as the safe area, the traits and the status bar
// change, and from updateViewConstraints, where UIKit notes the reserved
// regions read and updates the constraints again as they change (an indicator
// come or gone); never from a layout pass: a change it made laid out again,
// which called it again.
- (void)updateMonitorColumn {
    UIView *view = self.view;
    UIEdgeInsets insets = view.safeAreaInsets;
    CGSize size = view.bounds.size;
    BOOL column = UserPreferences.shared.showMeters && insets.right >= 44 && insets.left < insets.right / 2;

    UIStatusBarManager *statusBar = view.window.windowScene.statusBarManager;
    BOOL statusHidden = statusBar == nil || statusBar.statusBarHidden;
    CGRect status = statusHidden ? CGRectNull : [view convertRect:statusBar.statusBarFrame fromView:nil];
    CGFloat columnLeft = size.width - insets.right;
    BOOL statusInColumn = !statusHidden && CGRectGetMinX(status) >= columnLeft - 8 &&
        CGRectGetMaxX(status) >= size.width - 1 && CGRectGetMaxY(status) > 0 && CGRectGetMaxY(status) < size.height / 2;
    // where the meters begin
    CGFloat top = 0;
    NSMutableArray<NSString *> *by = [NSMutableArray array];
    if (!statusHidden) {
        if (statusInColumn) {
            top = CGRectGetMaxY(status) + 8;
            [by addObject:[NSString stringWithFormat:@"the status bar's frame, to %.0f", CGRectGetMaxY(status)]];
        }
        // UIKit's say of it is iOS 27.1's: an SDK before has none of it (Xcode
        // 16.4 failed to compile it), where the status bar's frame is all
#if defined(__IPHONE_27_1)
        if (@available(iOS 27.1, *)) {
            for (UIViewReservedRegion *region in [view reservedRegionsOfKind:[UIViewReservedRegionKind occlusionRegionKind]]) {
                CGRect frame = region.frame;
                BOOL columnTop = CGRectGetMaxX(frame) > columnLeft && CGRectGetMinY(frame) < size.height / 2;
                if (columnTop)
                    top = MAX(top, CGRectGetMaxY(frame));
                [by addObject:[NSString stringWithFormat:@"occlusion %@ %@, its margins %@%@%@", region.identifier,
                               NSStringFromCGRect(frame), NSStringFromUIEdgeInsets(region.margins),
                               region.active ? @"" : @" (inactive)", columnTop ? @"" : @" (not at the column's top)"]];
            }
            UIViewLayoutRegion *bar = [UIViewLayoutRegion layoutRegionForBarOnEdge:UIRectEdgeRight extent:insets.right];
            UIEdgeInsets barInsets = [view edgeInsetsForLayoutRegion:bar];
            top = MAX(top, barInsets.top);
            [by addObject:[NSString stringWithFormat:@"a bar on the right edge, its insets %@",
                           NSStringFromUIEdgeInsets(barInsets)]];
        }
#endif
    }
    if (top == 0) {
        top = MAX(insets.top, CGRectGetMaxY(self.tabBar.frame)) + 8;
        [by addObject:statusHidden ? @"the status bar hidden: under the tab bar"
                                   : @"nothing said of where the status bar ends: under the tab bar"];
    }

    NSString *layout = [NSString stringWithFormat:@"%@ (%@); safe area insets %@, view %@, status bar %@",
                        column ? [NSString stringWithFormat:@"from %.0f", top] : @"none",
                        [by componentsJoinedByString:@"; "],
                        NSStringFromUIEdgeInsets(insets), NSStringFromCGSize(size),
                        statusHidden ? @"hidden" : NSStringFromCGRect(status)];
    if (![layout isEqualToString:self.monitorColumnLayout]) {
        self.monitorColumnLayout = layout;
        NSLog(@"Meters' column: %@", layout);
        [NSUserDefaults.standardUserDefaults setObject:layout forKey:kMetersColumnLayoutKey];
    }

    if (self.monitorColumn.hidden != !column)
        self.monitorColumn.hidden = !column;
    // The strip in the terminal's margin only where the column is not: beside
    // it, its CPU, memory and heat were the column's again
    UIView *margin = UserPreferences.shared.showMeters && !column ? self.monitorStrip : nil;
    if (self.termView.marginView != margin)
        self.termView.marginView = margin;
    // before the keys, which go by it
    if (column && self.monitorColumnTop.constant != top)
        self.monitorColumnTop.constant = top;
    [self updateColumnKeys];
}

// The reserved regions updateMonitorColumn reads here are UIKit's to watch:
// it asks for the constraints again when they change
- (void)updateViewConstraints {
    [self updateMonitorColumn];
    [super updateViewConstraints];
}

// In a popover pointing at the meters it was asked from, as they read
- (void)showMeterDetailsFrom:(MonitorView *)meters {
    MonitorDetailViewController *details = [[MonitorDetailViewController alloc] initWithSampler:self.monitorSampler
                                                                                     sourceView:meters
                                                                                     sourceRect:meters.metersFrame];
    details.keyboardUp = self.keyboardUp;
    [self presentViewController:details animated:YES completion:nil];
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
// ⌘1 to ⌘8 the first eight tabs, ⌘9 the last, as in browsers
- (void)selectTabByNumber:(UICommand *)sender {
    NSUInteger number = [sender.propertyList unsignedIntegerValue];
    if (number == 8 && self.tabs.count > 0)
        [self selectTabAtIndex:self.tabs.count - 1];
    else
        [self selectTabAtIndex:number];
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
    // What was copied elsewhere shows: read as the clipboard is asked for, the
    // one time iOS may ask about it, not as iSH comes to the front
    [ClipboardStore.shared readPasteboard];
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

// Whether the paste key pastes the Paste Stack's next item itself, iOS's paste
// button hidden: while the stack is open and has items. Empty, as it is when
// just opened and once every item is pasted, the stack leaves the clipboard to
// iOS's button, as when it is closed: the key would read the clipboard itself,
// and iOS would ask "would like to paste from …" again.
- (BOOL)pasteKeyPastesStack {
    ClipboardStore *store = ClipboardStore.shared;
    return store.stackActive && store.stackItems.count > 0;
}

// The paste key itself, while the Paste Stack has items (pasteKeyPastesStack):
// the next one.
- (void)pasteKey:(id)sender {
    [self pasteFromStack];
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
    // The key pastes the stack's items, iOS's paste button the clipboard
    BOOL stacked = [self pasteKeyPastesStack];
    self.pasteControl.hidden = stacked;
    self.pasteButton.isAccessibilityElement = stacked;
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
    // what was found is the terminal's that is going
    if (terminal != _terminal)
        [self hideFind];
    _terminal = terminal;
    self.termView.terminal = self.terminal;
}

@end

@interface BarView : UIInputView <UIInputViewAudioFeedback>
@property (weak) IBOutlet TerminalViewController *terminalViewController;
@property (nonatomic) IBInspectable BOOL allowsSelfSizing;
@end
@implementation BarView
@dynamic allowsSelfSizing;

- (void)layoutSubviews {
    [self.terminalViewController resizeBar];
}

// The keys' clicks (playInputClick), which follow Settings' keyboard clicks
- (BOOL)enableInputClicksWhenVisible {
    return YES;
}

@end
