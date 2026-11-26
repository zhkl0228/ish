//
//  TabBarView.m
//  iSH
//

#import <GameController/GameController.h>
#import "TabBarView.h"
#import "Theme.h"

// As tall as a finger needs, at least: a tab takes the touches in all of it
static const CGFloat kMinStripHeight = 44;
// The + and ⋯ buttons' width
static const CGFloat kSlotWidth = 44;
// Room around the capsules: at the strip's ends, between them, above and
// below. A tab's cell takes the room between its capsule and the next and
// above and below it (TabItemView's capsuleFrame), so that a finger that
// misses the capsule still has the tab.
static const CGFloat kStripInset = 8;
static const CGFloat kTabSpacing = 4;
static const CGFloat kCapsuleInset = 5;
// A tab's capsule is as wide as what it shows, within these
static const CGFloat kMinTabWidth = 76;
static const CGFloat kMaxTabWidth = 220;
// Inside a capsule: its ends, and the room between what it shows
static const CGFloat kCapsulePadding = 12;
static const CGFloat kCapsuleSpacing = 6;
// The restart button, and the room it has around its glyph
static const CGFloat kActionSize = 22;
static const CGFloat kActionRoom = 6;
// The fill under the selected tab: how far toward the foreground
static const CGFloat kSelectedFill = 0.14;
static const CGFloat kHoverFill = 0.07;

static UIFont *TabFont(void) {
    return [UIFontMetrics.defaultMetrics scaledFontForFont:[UIFont systemFontOfSize:14 weight:UIFontWeightMedium]];
}

// The icon of what runs in a tab: a symbol for the kind of program, the
// shell's while it owns the terminal
static NSString *SymbolForProgram(NSString *_Nullable program) {
    if (program == nil)
        return @"terminal";
    static NSDictionary<NSString *, NSString *> *symbols;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSMutableDictionary<NSString *, NSString *> *map = [NSMutableDictionary new];
        void (^kind)(NSString *, NSArray<NSString *> *) = ^(NSString *symbol, NSArray<NSString *> *programs) {
            for (NSString *name in programs)
                map[name] = symbol;
        };
        kind(@"pencil", @[@"vi", @"vim", @"nvim", @"nano", @"emacs", @"micro", @"hx", @"ed"]);
        kind(@"sparkles", @[@"claude", @"kimi", @"codex", @"aider", @"gemini", @"opencode"]);
        kind(@"chevron.left.forwardslash.chevron.right",
             @[@"python", @"node", @"bun", @"deno", @"ruby", @"perl", @"php", @"lua", @"irb", @"java"]);
        kind(@"network", @[@"ssh", @"mosh", @"telnet", @"nc", @"curl", @"wget", @"ping", @"tcpdump", @"dig"]);
        kind(@"chart.bar", @[@"top", @"htop", @"btop", @"watch"]);
        kind(@"hammer", @[@"make", @"gcc", @"cc", @"clang", @"go", @"cargo", @"rustc", @"npm", @"pip", @"apk", @"ish-install"]);
        kind(@"doc.text", @[@"less", @"more", @"man", @"tail"]);
        kind(@"arrow.triangle.branch", @[@"git"]);
        symbols = map;
    });
    // python3.12, pip3: the name without its version
    NSString *name = [program stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"0123456789."]];
    return symbols[program] ?: symbols[name] ?: @"gearshape";
}

#pragma mark - TabItemView

@interface TabItemView : UIControl
// What a tab shows of itself: a capsule inside its cell, which the hover's
// fill and the selected tab's are drawn as
@property (readonly) UIView *capsule;
@property (readonly) CGRect capsuleFrame;
@property (readonly) UIImageView *icon;
@property (readonly) UILabel *label;
// What it did unseen: output, a job that ended, or a bell (a program's
// notification too)
@property (readonly) UIView *dot;
@property (readonly) UIImageView *check;
@property (readonly) UIImageView *bell;
// ⌘1…⌘9, with a hardware keyboard
@property (readonly) UILabel *hintLabel;
// Restart, once its shell exited
@property (readonly) UIButton *actionButton;
@property (readonly) NSLayoutConstraint *trailingConstraint;
@property NSUUID *sessionUUID;
@property NSUInteger index;
@property BOOL tabSelected;
@property BOOL hovered;
@property BOOL exited;
@property TerminalSessionActivity activity;
@end

@implementation TabItemView

- (instancetype)initWithFrame:(CGRect)frame {
    if (self = [super initWithFrame:frame]) {
        _capsule = [UIView new];
        _capsule.userInteractionEnabled = NO;
        [self addSubview:_capsule];

        _icon = [UIImageView new];
        _icon.preferredSymbolConfiguration =
            [UIImageSymbolConfiguration configurationWithPointSize:12 weight:UIImageSymbolWeightSemibold];
        _icon.contentMode = UIViewContentModeCenter;
        _icon.isAccessibilityElement = NO;

        _label = [UILabel new];
        _label.font = TabFont();
        _label.adjustsFontForContentSizeCategory = YES;
        _label.lineBreakMode = NSLineBreakByTruncatingTail;
        [_label setContentCompressionResistancePriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];

        _dot = [UIView new];
        _dot.layer.cornerRadius = 3;

        _check = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"checkmark"]];
        _check.preferredSymbolConfiguration =
            [UIImageSymbolConfiguration configurationWithPointSize:10 weight:UIImageSymbolWeightBold];
        _check.contentMode = UIViewContentModeCenter;

        _bell = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"bell.fill"]];
        _bell.preferredSymbolConfiguration =
            [UIImageSymbolConfiguration configurationWithPointSize:10 weight:UIImageSymbolWeightBold];
        _bell.contentMode = UIViewContentModeCenter;

        _hintLabel = [UILabel new];
        _hintLabel.font = TabFont();
        _hintLabel.adjustsFontForContentSizeCategory = YES;
        _hintLabel.isAccessibilityElement = NO;

        _actionButton = [UIButton buttonWithType:UIButtonTypeSystem];
        _actionButton.pointerInteractionEnabled = YES;

        for (UIView *view in @[_icon, _dot, _check, _bell, _hintLabel, _actionButton]) {
            [view setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
            [view setContentCompressionResistancePriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
        }

        UIStackView *content = [[UIStackView alloc] initWithArrangedSubviews:@[_icon, _label, _dot, _check, _bell, _hintLabel, _actionButton]];
        content.axis = UILayoutConstraintAxisHorizontal;
        content.alignment = UIStackViewAlignmentCenter;
        content.spacing = kCapsuleSpacing;
        content.translatesAutoresizingMaskIntoConstraints = NO;
        [self addSubview:content];

        _trailingConstraint = [content.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-kCapsulePadding - kTabSpacing / 2];
        [NSLayoutConstraint activateConstraints:@[
            [content.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:kCapsulePadding + kTabSpacing / 2],
            _trailingConstraint,
            [content.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
            [self.widthAnchor constraintGreaterThanOrEqualToConstant:kMinTabWidth + kTabSpacing],
            [self.widthAnchor constraintLessThanOrEqualToConstant:kMaxTabWidth + kTabSpacing],
            [_dot.widthAnchor constraintEqualToConstant:6],
            [_dot.heightAnchor constraintEqualToConstant:6],
            [_actionButton.widthAnchor constraintEqualToConstant:kActionSize],
            [_actionButton.heightAnchor constraintEqualToConstant:kActionSize],
        ]];
    }
    return self;
}

// The action button takes the touches on it and a finger's room around it;
// anywhere else, the tab does
- (UIView *)hitTest:(CGPoint)point withEvent:(UIEvent *)event {
    if (!self.userInteractionEnabled || self.hidden || ![self pointInside:point withEvent:event])
        return nil;
    if (!self.actionButton.hidden) {
        CGRect target = CGRectInset([self convertRect:self.actionButton.bounds fromView:self.actionButton], -kActionRoom, -kActionRoom);
        if (CGRectContainsPoint(target, point))
            return self.actionButton;
    }
    return self;
}

- (CGRect)capsuleFrame {
    return CGRectInset(self.bounds, kTabSpacing / 2, kCapsuleInset);
}

- (void)layoutSubviews {
    [super layoutSubviews];
    self.capsule.frame = self.capsuleFrame;
    self.capsule.layer.cornerRadius = self.capsuleFrame.size.height / 2;
}

@end

#pragma mark - TabScrollView

// Lets a drag that starts on a tab scroll the strip instead of getting stuck on the control.
@interface TabScrollView : UIScrollView
@end

@implementation TabScrollView
- (BOOL)touchesShouldCancelInContentView:(UIView *)view {
    return YES;
}
@end

#pragma mark - TabBarView

@interface TabBarView () <UIContextMenuInteractionDelegate, UIDragInteractionDelegate, UIDropInteractionDelegate>
@property NSArray<TerminalSession *> *sessions;
@property TabScrollView *scrollView;
@property UIStackView *stack;
// The fill under the selected tab
@property UIView *indicator;
@property UIButton *addTabButton;
@property UIButton *commandsButton;
@property UIView *commandsBadge;
@property UIView *bottomRule;
@property NSLayoutConstraint *heightConstraint;
@property BOOL hardwareKeyboard;
@property UIColor *foreground;
@property UIColor *background;
@property NSUInteger selectedIndex;
@property NSUUID *revealedUUID;
// The session the fill is under, and whether it slides to the selected one at
// the next layout
@property (nullable) NSUUID *indicatorUUID;
@property BOOL indicatorSlides;
@property CGFloat lastLayoutWidth;
@end

@implementation TabBarView

- (instancetype)initWithFrame:(CGRect)frame {
    if (self = [super initWithFrame:frame]) {
        self.foreground = UIColor.labelColor;
        self.background = UIColor.systemBackgroundColor;
        self.sessions = @[];
        self.selectedIndex = NSNotFound;
        self.accessibilityIdentifier = @"tab bar";
        __weak TabBarView *weakSelf = self;

        _bottomRule = [UIView new];
        _bottomRule.translatesAutoresizingMaskIntoConstraints = NO;
        _bottomRule.userInteractionEnabled = NO;
        [self addSubview:_bottomRule];

        _addTabButton = [UIButton buttonWithType:UIButtonTypeSystem];
        [_addTabButton setImage:[UIImage systemImageNamed:@"plus"
                                         withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:15 weight:UIImageSymbolWeightMedium]]
                       forState:UIControlStateNormal];
        _addTabButton.accessibilityLabel = @"New Tab";
        _addTabButton.accessibilityIdentifier = @"new tab";
        _addTabButton.translatesAutoresizingMaskIntoConstraints = NO;
        _addTabButton.pointerInteractionEnabled = YES;
        [_addTabButton addTarget:self action:@selector(newTabPressed:) forControlEvents:UIControlEventPrimaryActionTriggered];
        // Held: a new tab, or one in the selected tab's directory
        _addTabButton.menu = [UIMenu menuWithChildren:@[
            [UIAction actionWithTitle:@"New Tab" image:[UIImage systemImageNamed:@"plus"] identifier:nil handler:^(UIAction *action) {
                [weakSelf.delegate tabBarDidRequestNewTab:weakSelf];
            }],
            [UIAction actionWithTitle:@"New Tab Here" image:[UIImage systemImageNamed:@"folder.badge.plus"] identifier:nil handler:^(UIAction *action) {
                TabBarView *strongSelf = weakSelf;
                if (strongSelf != nil && strongSelf.selectedIndex < strongSelf.sessions.count)
                    [strongSelf.delegate tabBar:strongSelf didRequestDuplicateTabAtIndex:strongSelf.selectedIndex];
            }],
        ]];
        [self addSubview:_addTabButton];

        // Every command the app has, for people without a keyboard.
        _commandsButton = [UIButton buttonWithType:UIButtonTypeSystem];
        [_commandsButton setImage:[UIImage systemImageNamed:@"ellipsis"
                                           withConfiguration:[UIImageSymbolConfiguration configurationWithPointSize:15 weight:UIImageSymbolWeightMedium]]
                         forState:UIControlStateNormal];
        _commandsButton.accessibilityLabel = @"Commands";
        _commandsButton.accessibilityIdentifier = @"commands";
        _commandsButton.translatesAutoresizingMaskIntoConstraints = NO;
        _commandsButton.pointerInteractionEnabled = YES;
        _commandsButton.showsMenuAsPrimaryAction = YES;
        _commandsButton.menu = [UIMenu menuWithChildren:@[
            [UIDeferredMenuElement elementWithUncachedProvider:^(void (^completion)(NSArray<UIMenuElement *> *)) {
                completion([weakSelf.delegate commandsMenuForTabBar:weakSelf].children ?: @[]);
            }],
        ]];
        [self addSubview:_commandsButton];

        _commandsBadge = [UIView new];
        _commandsBadge.backgroundColor = UIColor.systemRedColor;
        _commandsBadge.layer.cornerRadius = 4;
        _commandsBadge.userInteractionEnabled = NO;
        _commandsBadge.hidden = YES;
        _commandsBadge.translatesAutoresizingMaskIntoConstraints = NO;
        [_commandsButton addSubview:_commandsBadge];

        _scrollView = [TabScrollView new];
        _scrollView.showsHorizontalScrollIndicator = NO;
        _scrollView.showsVerticalScrollIndicator = NO;
        _scrollView.alwaysBounceVertical = NO;
        _scrollView.delaysContentTouches = NO;
        // A tap on the status bar scrolls the terminal (TerminalView), which
        // takes the other scroll views on screen not having it
        _scrollView.scrollsToTop = NO;
        _scrollView.translatesAutoresizingMaskIntoConstraints = NO;
        [self addSubview:_scrollView];

        _stack = [UIStackView new];
        _stack.axis = UILayoutConstraintAxisHorizontal;
        _stack.distribution = UIStackViewDistributionFill;
        _stack.alignment = UIStackViewAlignmentFill;
        // the cells side by side, the room between capsules theirs
        _stack.spacing = 0;
        _stack.translatesAutoresizingMaskIntoConstraints = NO;
        [_scrollView addSubview:_stack];

        // Placed by frame under the selected tab (placeIndicator), behind the tabs
        _indicator = [UIView new];
        _indicator.userInteractionEnabled = NO;
        _indicator.hidden = YES;
        [_scrollView insertSubview:_indicator belowSubview:_stack];

        UILayoutGuide *content = _scrollView.contentLayoutGuide;
        UILayoutGuide *frameGuide = _scrollView.frameLayoutGuide;
        // The strip color extends under the status bar; the controls sit below the safe area.
        _heightConstraint = [_scrollView.heightAnchor constraintEqualToConstant:kMinStripHeight];
        [NSLayoutConstraint activateConstraints:@[
            _heightConstraint,

            [_bottomRule.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [_bottomRule.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
            [_bottomRule.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [_bottomRule.heightAnchor constraintEqualToConstant:1 / UIScreen.mainScreen.scale],

            [_scrollView.leadingAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.leadingAnchor],
            [_scrollView.topAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.topAnchor],
            [_scrollView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [_scrollView.trailingAnchor constraintEqualToAnchor:_addTabButton.leadingAnchor],

            [_addTabButton.trailingAnchor constraintEqualToAnchor:_commandsButton.leadingAnchor],
            [_addTabButton.topAnchor constraintEqualToAnchor:_scrollView.topAnchor],
            [_addTabButton.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [_addTabButton.widthAnchor constraintEqualToConstant:kSlotWidth],

            [_commandsButton.trailingAnchor constraintEqualToAnchor:self.safeAreaLayoutGuide.trailingAnchor],
            [_commandsButton.topAnchor constraintEqualToAnchor:_scrollView.topAnchor],
            [_commandsButton.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [_commandsButton.widthAnchor constraintEqualToConstant:kSlotWidth],
            [_commandsBadge.widthAnchor constraintEqualToConstant:8],
            [_commandsBadge.heightAnchor constraintEqualToConstant:8],
            [_commandsBadge.centerXAnchor constraintEqualToAnchor:_commandsButton.centerXAnchor constant:8],
            [_commandsBadge.centerYAnchor constraintEqualToAnchor:_commandsButton.centerYAnchor constant:-6],

            [_stack.leadingAnchor constraintEqualToAnchor:content.leadingAnchor constant:kStripInset - kTabSpacing / 2],
            [_stack.trailingAnchor constraintEqualToAnchor:content.trailingAnchor constant:-(kStripInset - kTabSpacing / 2)],
            [_stack.topAnchor constraintEqualToAnchor:content.topAnchor],
            [_stack.bottomAnchor constraintEqualToAnchor:content.bottomAnchor],
            [_stack.heightAnchor constraintEqualToAnchor:frameGuide.heightAnchor],
        ]];

        // A tab dragged along the strip lands where it is dropped
        [self addInteraction:[[UIDropInteraction alloc] initWithDelegate:self]];

        // ⌘1…⌘9 hints only make sense with a hardware keyboard attached.
        self.hardwareKeyboard = GCKeyboard.coalescedKeyboard != nil;
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(keyboardConnectionChanged:) name:GCKeyboardDidConnectNotification object:nil];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(keyboardConnectionChanged:) name:GCKeyboardDidDisconnectNotification object:nil];
        [self updateMetrics];
        NSArray *traits = [UITraitCollection.systemTraitsAffectingColorAppearance
            arrayByAddingObjectsFromArray:@[UITraitPreferredContentSizeCategory.class, UITraitHorizontalSizeClass.class]];
        [self registerForTraitChanges:traits withAction:@selector(traitsChanged)];
    }
    return self;
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self];
}

#pragma mark Content

- (TabItemView *)makeItemForSession:(TerminalSession *)session {
    TabItemView *item = [[TabItemView alloc] initWithFrame:CGRectZero];
    item.sessionUUID = session.uuid;
    [item addTarget:self action:@selector(tabPressed:) forControlEvents:UIControlEventTouchUpInside];
    [item addTarget:self action:@selector(tabHighlightChanged:) forControlEvents:UIControlEventTouchDown | UIControlEventTouchDragEnter | UIControlEventTouchDragExit | UIControlEventTouchUpInside | UIControlEventTouchUpOutside | UIControlEventTouchCancel];
    [item.actionButton addTarget:self action:@selector(actionPressed:) forControlEvents:UIControlEventPrimaryActionTriggered];
    [item addGestureRecognizer:[[UIHoverGestureRecognizer alloc] initWithTarget:self action:@selector(tabHovered:)]];
    [item addInteraction:[[UIContextMenuInteraction alloc] initWithDelegate:self]];
    // Off by default on an iPhone
    UIDragInteraction *drag = [[UIDragInteraction alloc] initWithDelegate:self];
    drag.enabled = YES;
    [item addInteraction:drag];
    return item;
}

- (void)setSessions:(NSArray<TerminalSession *> *)sessions selectedIndex:(NSUInteger)selectedIndex {
    BOOL grew = sessions.count > self.sessions.count;
    self.sessions = [sessions copy];
    self.selectedIndex = selectedIndex;
    // Item views follow their session, so a tap or press in flight on one tab can never
    // land on another one after tabs close or move.
    NSMutableDictionary<NSUUID *, TabItemView *> *existing = [NSMutableDictionary new];
    for (TabItemView *item in self.stack.arrangedSubviews) {
        existing[item.sessionUUID] = item;
        [self.stack removeArrangedSubview:item];
    }
    NSMutableArray<TabItemView *> *items = [NSMutableArray new];
    for (TerminalSession *session in sessions) {
        TabItemView *item = existing[session.uuid];
        if (item != nil)
            [existing removeObjectForKey:session.uuid];
        else
            item = [self makeItemForSession:session];
        [self.stack addArrangedSubview:item];
        [items addObject:item];
    }
    for (TabItemView *gone in existing.allValues)
        [gone removeFromSuperview];
    [sessions enumerateObjectsUsingBlock:^(TerminalSession *session, NSUInteger i, BOOL *stop) {
        TabItemView *item = items[i];
        item.index = i;
        item.tabSelected = i == selectedIndex;
        item.exited = session.state == TerminalSessionStateExited;
        item.activity = session.activity;
        NSString *title = session.displayTitle;
        item.label.text = title;
        item.icon.image = [UIImage systemImageNamed:item.exited ? @"xmark.circle" : SymbolForProgram(session.foregroundProgram)];
        // ⌘1 to ⌘8 the first eight, ⌘9 the last (TerminalViewController)
        if (i < 8)
            item.hintLabel.text = [NSString stringWithFormat:@"⌘%lu", (unsigned long) i + 1];
        else
            item.hintLabel.text = i == sessions.count - 1 ? @"⌘9" : nil;
        item.accessibilityLabel = title;
        if (item.exited)
            item.accessibilityValue = @"exited";
        else if (!item.tabSelected && item.activity == TerminalSessionActivityBell)
            item.accessibilityValue = @"bell";
        else if (!item.tabSelected && item.activity == TerminalSessionActivityFinished)
            item.accessibilityValue = @"finished";
        else if (!item.tabSelected && item.activity == TerminalSessionActivityOutput)
            item.accessibilityValue = @"new output";
        else
            item.accessibilityValue = nil;
        item.accessibilityIdentifier = [NSString stringWithFormat:@"tab %lu", (unsigned long) i + 1];
        item.accessibilityTraits = item.tabSelected ? UIAccessibilityTraitButton | UIAccessibilityTraitSelected : UIAccessibilityTraitButton;
        item.accessibilityCustomActions = @[
            item.exited ? [[UIAccessibilityCustomAction alloc] initWithName:@"Restart" target:self selector:@selector(restartAction:)]
                        : [[UIAccessibilityCustomAction alloc] initWithName:@"Close" target:self selector:@selector(closeAction:)],
            [[UIAccessibilityCustomAction alloc] initWithName:@"Rename" target:self selector:@selector(renameAction:)],
        ];
    }];

    // The fill slides to a tab newly selected; it is put under one whose
    // width changed
    NSUUID *selectedUUID = selectedIndex < sessions.count ? sessions[selectedIndex].uuid : nil;
    if (selectedUUID != nil && self.indicatorUUID != nil && ![selectedUUID isEqual:self.indicatorUUID])
        self.indicatorSlides = YES;
    self.indicatorUUID = selectedUUID;
    [self applyColors];
    [self setNeedsLayout];

    // Only pull the strip to the selected tab when the selection changed or a tab was added,
    // so a title change does not yank the user away from the tabs they are browsing.
    if (grew || ![selectedUUID isEqual:self.revealedUUID]) {
        self.revealedUUID = selectedUUID;
        [self scrollSelectedTabIntoView];
    }
}

- (NSUInteger)indexOfSessionUUID:(NSUUID *)uuid {
    return [self.sessions indexOfObjectPassingTest:^BOOL(TerminalSession *session, NSUInteger idx, BOOL *stop) {
        return [session.uuid isEqual:uuid];
    }];
}

- (void)setBackgroundColor:(UIColor *)background foregroundColor:(UIColor *)foreground {
    self.background = background;
    self.foreground = foreground;
    [self applyColors];
}

- (void)setShowsCommandsBadge:(BOOL)showsCommandsBadge {
    _showsCommandsBadge = showsCommandsBadge;
    self.commandsBadge.hidden = !showsCommandsBadge;
    self.commandsButton.accessibilityValue = showsCommandsBadge ? @"Update available" : nil;
}

- (UIColor *)selectedFill {
    return [self.background ish_blendedWithColor:self.foreground fraction:kSelectedFill];
}

- (void)applyColors {
    // The strip is the terminal's background, the selected tab on a fill of
    // it nudged toward the foreground, which follows the theme's hue
    UIColor *hoverFill = [self.background ish_blendedWithColor:self.foreground fraction:kHoverFill];
    UIColor *rule = [self.foreground colorWithAlphaComponent:0.12];
    UIColor *dimText = [self.foreground colorWithAlphaComponent:0.6];
    UIColor *exitedText = [self.foreground colorWithAlphaComponent:0.38];
    UIColor *hintText = [self.foreground colorWithAlphaComponent:0.45];
    self.backgroundColor = self.background;
    self.scrollView.backgroundColor = UIColor.clearColor;
    self.bottomRule.backgroundColor = rule;
    self.indicator.backgroundColor = self.selectedFill;
    self.addTabButton.tintColor = dimText;
    self.commandsButton.tintColor = dimText;
    BOOL showHints = self.hardwareKeyboard && self.traitCollection.horizontalSizeClass == UIUserInterfaceSizeClassRegular;
    [self.stack.arrangedSubviews enumerateObjectsUsingBlock:^(__kindof UIView *view, NSUInteger i, BOOL *stop) {
        TabItemView *item = view;
        BOOL active = !item.tabSelected && (item.hovered || item.highlighted);
        item.capsule.backgroundColor = active ? hoverFill : UIColor.clearColor;
        UIColor *text = item.exited ? exitedText : item.tabSelected ? self.foreground : dimText;
        item.label.textColor = text;
        item.icon.tintColor = text;
        item.dot.backgroundColor = UIColor.systemBlueColor;
        item.check.tintColor = UIColor.systemGreenColor;
        item.bell.tintColor = UIColor.systemOrangeColor;
        item.hintLabel.textColor = hintText;
        item.actionButton.tintColor = [self.foreground colorWithAlphaComponent:item.tabSelected ? 0.7 : 0.55];
        [self updateAccessoriesOfItem:item showingHint:showHints && item.hintLabel.text != nil];
    }];
}

// What a tab shows at its end: the restart arrow once its shell exited; else,
// not selected, what it did unseen, a bell before a job that ended before
// output (TerminalSession's activity); else its ⌘ number with a keyboard
- (void)updateAccessoriesOfItem:(TabItemView *)item showingHint:(BOOL)showHint {
    BOOL action = item.exited;
    BOOL unseen = !action && !item.tabSelected;
    BOOL bell = unseen && item.activity == TerminalSessionActivityBell;
    BOOL check = unseen && item.activity == TerminalSessionActivityFinished;
    BOOL dot = unseen && item.activity == TerminalSessionActivityOutput;
    BOOL hint = !action && !bell && !check && !dot && showHint;
    item.actionButton.hidden = !action;
    item.bell.hidden = !bell;
    item.check.hidden = !check;
    item.dot.hidden = !dot;
    item.hintLabel.hidden = !hint;
    if (action) {
        UIImageSymbolConfiguration *config =
            [UIImageSymbolConfiguration configurationWithPointSize:10 weight:UIImageSymbolWeightBold];
        [item.actionButton setImage:[UIImage systemImageNamed:@"arrow.clockwise" withConfiguration:config]
                           forState:UIControlStateNormal];
        item.actionButton.accessibilityLabel = @"Restart";
    }
    // the button has room of its own around its glyph
    CGFloat padding = action ? kCapsulePadding - kActionRoom : kCapsulePadding;
    item.trailingConstraint.constant = -(padding + kTabSpacing / 2);
}

#pragma mark Layout

- (void)updateMetrics {
    UIFont *font = TabFont();
    self.heightConstraint.constant = MAX(kMinStripHeight, ceil(font.lineHeight) + 2 * kCapsuleInset + 16);
    for (TabItemView *item in self.stack.arrangedSubviews) {
        item.label.font = font;
        item.hintLabel.font = font;
    }
    [self setNeedsLayout];
}

- (void)layoutSubviews {
    [super layoutSubviews];
    [self.scrollView layoutIfNeeded];
    [self placeIndicator];
    // A narrower window can push the selected tab out of view; bring it back.
    CGFloat width = self.bounds.size.width;
    if (width != self.lastLayoutWidth) {
        self.lastLayoutWidth = width;
        [self scrollSelectedTabIntoView];
    }
}

// The fill under the selected tab: slid there from the last one selected, or
// put there (a title that changed its width, a strip laid out anew)
- (void)placeIndicator {
    if (self.selectedIndex >= self.stack.arrangedSubviews.count) {
        self.indicator.hidden = YES;
        return;
    }
    TabItemView *item = self.stack.arrangedSubviews[self.selectedIndex];
    CGRect frame = [self.scrollView convertRect:item.capsuleFrame fromView:item];
    BOOL slide = self.indicatorSlides && !self.indicator.hidden && !UIAccessibilityIsReduceMotionEnabled();
    self.indicatorSlides = NO;
    self.indicator.hidden = NO;
    self.indicator.layer.cornerRadius = frame.size.height / 2;
    if (CGRectEqualToRect(self.indicator.frame, frame))
        return;
    if (slide) {
        [UIView animateWithDuration:0.32 delay:0 usingSpringWithDamping:0.82 initialSpringVelocity:0
                            options:UIViewAnimationOptionBeginFromCurrentState | UIViewAnimationOptionAllowUserInteraction
                         animations:^{
            self.indicator.frame = frame;
        } completion:nil];
    } else {
        [UIView performWithoutAnimation:^{
            self.indicator.frame = frame;
        }];
    }
}

- (void)traitsChanged {
    [self updateMetrics];
    [self applyColors];
    [self scrollSelectedTabIntoView];
}

- (void)scrollSelectedTabIntoView {
    // Resolve the selected item when the block runs: tabs may have closed by then.
    dispatch_async(dispatch_get_main_queue(), ^{
        if (self.selectedIndex >= self.stack.arrangedSubviews.count)
            return;
        [self layoutIfNeeded];
        TabItemView *item = self.stack.arrangedSubviews[self.selectedIndex];
        CGRect frame = [self.scrollView convertRect:item.capsuleFrame fromView:item];
        [self.scrollView scrollRectToVisible:CGRectInset(frame, -kStripInset, 0) animated:YES];
    });
}

- (void)keyboardConnectionChanged:(NSNotification *)notif {
    dispatch_async(dispatch_get_main_queue(), ^{
        self.hardwareKeyboard = GCKeyboard.coalescedKeyboard != nil;
        [self applyColors];
    });
}

#pragma mark Actions

- (void)tabPressed:(TabItemView *)item {
    [self.delegate tabBar:self didSelectTabAtIndex:item.index];
}

- (void)tabHighlightChanged:(TabItemView *)item {
    [self applyColors];
}

- (void)tabHovered:(UIHoverGestureRecognizer *)recognizer {
    TabItemView *item = (TabItemView *) recognizer.view;
    BOOL hovered = recognizer.state == UIGestureRecognizerStateBegan || recognizer.state == UIGestureRecognizerStateChanged;
    if (hovered == item.hovered)
        return;
    item.hovered = hovered;
    [self applyColors];
}

// The arrow of a tab that exited restarts it
- (void)actionPressed:(UIButton *)button {
    UIView *view = button.superview;
    while (view != nil && ![view isKindOfClass:TabItemView.class])
        view = view.superview;
    TabItemView *item = (TabItemView *) view;
    if (item != nil)
        [self.delegate tabBar:self didRequestRestartTabAtIndex:item.index];
}

- (TabItemView *)itemForAccessibilityAction:(UIAccessibilityCustomAction *)action {
    for (TabItemView *item in self.stack.arrangedSubviews) {
        if ([item.accessibilityCustomActions containsObject:action])
            return item;
    }
    return nil;
}

- (BOOL)renameAction:(UIAccessibilityCustomAction *)action {
    TabItemView *item = [self itemForAccessibilityAction:action];
    if (item == nil)
        return NO;
    [self.delegate tabBar:self didRequestRenameTabAtIndex:item.index];
    return YES;
}

- (BOOL)closeAction:(UIAccessibilityCustomAction *)action {
    TabItemView *item = [self itemForAccessibilityAction:action];
    if (item == nil)
        return NO;
    [self.delegate tabBar:self didRequestCloseTabAtIndex:item.index];
    return YES;
}

- (BOOL)restartAction:(UIAccessibilityCustomAction *)action {
    TabItemView *item = [self itemForAccessibilityAction:action];
    if (item == nil)
        return NO;
    [self.delegate tabBar:self didRequestRestartTabAtIndex:item.index];
    return YES;
}

- (void)newTabPressed:(UIButton *)button {
    [self.delegate tabBarDidRequestNewTab:self];
}

#pragma mark Context menu

// A tab lifted, for its menu or a drag: its capsule, on the selected tab's fill
- (UIPreviewParameters *)capsuleParametersOfItem:(TabItemView *)item parameters:(UIPreviewParameters *)parameters {
    CGRect capsule = item.capsuleFrame;
    parameters.visiblePath = [UIBezierPath bezierPathWithRoundedRect:capsule cornerRadius:capsule.size.height / 2];
    parameters.backgroundColor = self.selectedFill;
    return parameters;
}

- (UITargetedPreview *)contextMenuInteraction:(UIContextMenuInteraction *)interaction
                                configuration:(UIContextMenuConfiguration *)configuration
                highlightPreviewForItemWithIdentifier:(id<NSCopying>)identifier {
    TabItemView *item = (TabItemView *) interaction.view;
    return [[UITargetedPreview alloc] initWithView:item parameters:[self capsuleParametersOfItem:item parameters:[UIPreviewParameters new]]];
}

- (UIContextMenuConfiguration *)contextMenuInteraction:(UIContextMenuInteraction *)interaction configurationForMenuAtLocation:(CGPoint)location {
    TabItemView *item = (TabItemView *) interaction.view;
    // Item views are reused by position, so remember the session, not the index: tabs can
    // close while the menu is open.
    NSUUID *uuid = item.sessionUUID;
    BOOL exited = item.exited;
    __weak TabBarView *weakSelf = self;
    return [UIContextMenuConfiguration configurationWithIdentifier:nil previewProvider:nil actionProvider:^UIMenu *(NSArray<UIMenuElement *> *suggested) {
        UIAction *rename = [UIAction actionWithTitle:@"Rename" image:[UIImage systemImageNamed:@"pencil"] identifier:nil handler:^(UIAction *action) {
            NSUInteger index = [weakSelf indexOfSessionUUID:uuid];
            if (index != NSNotFound)
                [weakSelf.delegate tabBar:weakSelf didRequestRenameTabAtIndex:index];
        }];
        UIAction *here = [UIAction actionWithTitle:@"New Tab Here" image:[UIImage systemImageNamed:@"folder.badge.plus"] identifier:nil handler:^(UIAction *action) {
            NSUInteger index = [weakSelf indexOfSessionUUID:uuid];
            if (index != NSNotFound)
                [weakSelf.delegate tabBar:weakSelf didRequestDuplicateTabAtIndex:index];
        }];
        UIAction *newTab = [UIAction actionWithTitle:@"New Tab" image:[UIImage systemImageNamed:@"plus"] identifier:nil handler:^(UIAction *action) {
            [weakSelf.delegate tabBarDidRequestNewTab:weakSelf];
        }];
        UIAction *restart = [UIAction actionWithTitle:@"Restart" image:[UIImage systemImageNamed:@"arrow.clockwise"] identifier:nil handler:^(UIAction *action) {
            NSUInteger index = [weakSelf indexOfSessionUUID:uuid];
            if (index != NSNotFound)
                [weakSelf.delegate tabBar:weakSelf didRequestRestartTabAtIndex:index];
        }];
        UIAction *close = [UIAction actionWithTitle:@"Close Tab" image:[UIImage systemImageNamed:@"xmark"] identifier:nil handler:^(UIAction *action) {
            NSUInteger index = [weakSelf indexOfSessionUUID:uuid];
            if (index != NSNotFound)
                [weakSelf.delegate tabBar:weakSelf didRequestCloseTabAtIndex:index];
        }];
        close.attributes = UIMenuElementAttributesDestructive;
        UIAction *closeOthers = [UIAction actionWithTitle:@"Close Other Tabs" image:nil identifier:nil handler:^(UIAction *action) {
            NSUInteger index = [weakSelf indexOfSessionUUID:uuid];
            if (index != NSNotFound)
                [weakSelf.delegate tabBar:weakSelf didRequestCloseOtherTabsAtIndex:index];
        }];
        closeOthers.attributes = UIMenuElementAttributesDestructive;
        if (weakSelf.sessions.count < 2)
            closeOthers.attributes |= UIMenuElementAttributesDisabled;
        NSMutableArray<UIMenuElement *> *children = [NSMutableArray arrayWithObjects:rename, here, newTab, nil];
        if (exited)
            [children addObject:restart];
        [children addObject:close];
        [children addObject:closeOthers];
        return [UIMenu menuWithTitle:@"" children:children];
    }];
}

#pragma mark Dragging

- (NSArray<UIDragItem *> *)dragInteraction:(UIDragInteraction *)interaction itemsForBeginningSession:(id<UIDragSession>)session {
    if (self.sessions.count < 2)
        return @[];
    TabItemView *item = (TabItemView *) interaction.view;
    UIDragItem *dragItem = [[UIDragItem alloc] initWithItemProvider:[NSItemProvider new]];
    dragItem.localObject = item.sessionUUID;
    return @[dragItem];
}

- (BOOL)dragInteraction:(UIDragInteraction *)interaction sessionIsRestrictedToDraggingApplication:(id<UIDragSession>)session {
    return YES;
}

- (UITargetedDragPreview *)dragInteraction:(UIDragInteraction *)interaction previewForLiftingItem:(UIDragItem *)dragItem session:(id<UIDragSession>)session {
    TabItemView *item = (TabItemView *) interaction.view;
    UIDragPreviewParameters *parameters = (UIDragPreviewParameters *) [self capsuleParametersOfItem:item parameters:[UIDragPreviewParameters new]];
    return [[UITargetedDragPreview alloc] initWithView:item parameters:parameters];
}

- (BOOL)dropInteraction:(UIDropInteraction *)interaction canHandleSession:(id<UIDropSession>)session {
    id object = session.localDragSession.items.firstObject.localObject;
    return [object isKindOfClass:NSUUID.class] && [self indexOfSessionUUID:object] != NSNotFound;
}

- (UIDropProposal *)dropInteraction:(UIDropInteraction *)interaction sessionDidUpdate:(id<UIDropSession>)session {
    return [[UIDropProposal alloc] initWithDropOperation:UIDropOperationMove];
}

- (void)dropInteraction:(UIDropInteraction *)interaction performDrop:(id<UIDropSession>)session {
    NSUUID *uuid = session.localDragSession.items.firstObject.localObject;
    NSUInteger from = [self indexOfSessionUUID:uuid];
    if (from == NSNotFound)
        return;
    // Where it lands: after each other tab whose middle is left of the drop
    CGFloat x = [session locationInView:self.stack].x;
    NSUInteger to = 0;
    for (TabItemView *item in self.stack.arrangedSubviews) {
        if (![item.sessionUUID isEqual:uuid] && CGRectGetMidX(item.frame) < x)
            to++;
    }
    if (to != from)
        [self.delegate tabBar:self didMoveTabAtIndex:from toIndex:to];
}

@end
