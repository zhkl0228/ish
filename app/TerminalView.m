//
//  TerminalView.m
//  iSH
//
//  Created by Theodore Dubois on 11/3/17.
//

#import "ScrollbarView.h"
#import "TerminalView.h"
#import "UserPreferences.h"
#import "UIApplication+OpenURL.h"
#import "NSObject+SaneKVO.h"

struct rowcol {
    int row;
    int col;
};

@interface TerminalView () <UIGestureRecognizerDelegate, UIEditMenuInteractionDelegate>

@property (nonatomic) NSMutableArray<UIKeyCommand *> *keyCommands;
@property ScrollbarView *scrollbarView;
@property (nonatomic) BOOL terminalFocused;
// While the native scrollbar is being updated from the terminal's own scroll
// state, its scroll events must not be sent back to the terminal.
@property BOOL syncingScroll;
// How far the terminal scrolls, in points: the lines above its last screen
// (term.js's newScrollRange). The scroll view's content is that and its own
// height, so that its bottom is the terminal's.
@property CGFloat scrollRange;
// In a full-screen program's screen (the alternate one), which has no lines
// above it to scroll, the drags are the program's mouse wheel (term.js's
// newWheelScrolling and wheel). wheelSent: the drag's way already sent.
@property BOOL wheelScrolling;
@property CGFloat wheelSent;
// A drag let go while moving: the wheel goes on, slowing as a scroll view does
@property (nullable) CADisplayLink *wheelMomentum;
@property CGFloat wheelVelocity;
@property CGPoint wheelPoint;

// A pinch's font size: the size it started from, and whether it has gone
// far enough to change it (a two-finger swipe to another tab does not)
@property int pinchStartSize;
@property BOOL pinchZooms;
// Down to the terminal's bottom, shown while it is scrolled up more than a
// screen (updateBottomButton)
@property UIButton *bottomButton;
// The size changed here: the next style shows it (sizeLabel)
@property BOOL showsSizeAtNextStyle;
@property (nullable) UIVisualEffectView *sizeView;
@property (nullable) UILabel *sizeLabel;
// Where the page's cells end (term.js's screenEdge), 0 before it says: the
// margin view's left
@property CGFloat screenEdge;
@property (nullable) NSLayoutConstraint *marginLeading;
@property UITapGestureRecognizer *marginTap;
// The long press that selects (selectByPress:)
@property UILongPressGestureRecognizer *selectPress;

// What an input method is putting together, drawn at the cursor (term.js's
// setComposition)
@property (nonatomic, nullable) NSString *markedText;
// Where the cursor is in the web view (term.js's cursorRect): an input
// method's candidates go by it
@property CGRect cursorRect;
@property (nullable) NSString *selectedText;
@property UITextRange *markedRange;
@property UITextRange *selectedRange;

// The selection's menu (Copy, and Open Link for a link), and where it points:
// the selection's rows
@property UIEditMenuInteraction *editMenu;
@property CGRect selectionRect;
@property (nullable) NSString *selectionLink;
@property UIImpactFeedbackGenerator *pressFeedback;

@property struct rowcol floatingCursor;
@property CGSize floatingCursorSensitivity;
@property CGSize actualFloatingCursorSensitivity;

@end

@implementation TerminalView
@synthesize inputDelegate;
@synthesize tokenizer;
@synthesize canBecomeFirstResponder;

- (void)awakeFromNib {
    [super awakeFromNib];
    self.inputAssistantItem.leadingBarButtonGroups = @[];
    self.inputAssistantItem.trailingBarButtonGroups = @[];

    ScrollbarView *scrollbarView = self.scrollbarView = [[ScrollbarView alloc] initWithFrame:self.bounds];
    scrollbarView.delegate = self;
    scrollbarView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    // Dragging the terminal down into the keyboard pulls the keyboard down with
    // the finger, as in Messages. That needs the drag to start even with nothing
    // to scroll (no scrollback, or a full-screen program like tmux), which takes
    // bouncing. The bounce doesn't show: the web view stays pinned in place and
    // the page clamps the out-of-range scroll positions it is sent.
    scrollbarView.alwaysBounceVertical = YES;
    scrollbarView.keyboardDismissMode = UIScrollViewKeyboardDismissModeInteractive;
    [scrollbarView.panGestureRecognizer addTarget:self action:@selector(scrollPanned:)];
    [self addSubview:scrollbarView];

    // Cmd-click opens links. The web view doesn't reliably pass Cmd through to
    // the page, so the modifier is read here.
    UITapGestureRecognizer *linkClick = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(linkClicked:)];
    linkClick.cancelsTouchesInView = NO;
    linkClick.delegate = self;
    [self addGestureRecognizer:linkClick];

    // A long press selects the word under it, and a drag on from there as far
    // as the finger goes (the page's selection: term.js); letting go shows its
    // menu (Copy). The page draws the text, so there is no text of the web
    // view's to select.
    UILongPressGestureRecognizer *select = [[UILongPressGestureRecognizer alloc] initWithTarget:self action:@selector(selectByPress:)];
    select.minimumPressDuration = 0.4;
    select.delegate = self;
    [self addGestureRecognizer:select];
    self.selectPress = select;
    self.pressFeedback = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];

    // Two fingers pinched or spread: the font smaller or bigger
    UIPinchGestureRecognizer *pinch = [[UIPinchGestureRecognizer alloc] initWithTarget:self action:@selector(pinched:)];
    pinch.delegate = self;
    [self addGestureRecognizer:pinch];

    // Two fingers tapped twice: Ctrl-L (clearTapped:). One finger's taps are
    // the page's clicks (a word or a line selected, a program's mouse), two
    // fingers' are no one's.
    UITapGestureRecognizer *clear = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(clearTapped:)];
    clear.numberOfTouchesRequired = 2;
    clear.numberOfTapsRequired = 2;
    clear.delegate = self;
    [self addGestureRecognizer:clear];

    // A tap on the margin view is its (marginTapped), not the page's click
    // there; a drag from it still scrolls, a long press still selects
    UITapGestureRecognizer *marginTap = [[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tappedMargin:)];
    marginTap.delegate = self;
    [self addGestureRecognizer:marginTap];
    self.marginTap = marginTap;

    UserPreferences *prefs = UserPreferences.shared;
    [prefs observe:@[@"capsLockMapping", @"optionMapping", @"backtickMapEscape", @"overrideControlSpace"]
           options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            self->_keyCommands = nil;
        });
    }];
    [prefs observe:@[@"colorScheme", @"fontFamily", @"fontSize", @"theme", @"cursorStyle", @"blinkCursor"]
           options:0 owner:self usingBlock:^(typeof(self) self) {
        dispatch_async(dispatch_get_main_queue(), ^{
            [self _updateStyle];
        });
    }];
    [NSNotificationCenter.defaultCenter addObserver:self
                                           selector:@selector(darkerColorsChanged:)
                                               name:UIAccessibilityDarkerSystemColorsStatusDidChangeNotification
                                             object:nil];

    // Scrolled up more than a screen: a button to go back down (bottomButton)
    __weak typeof(self) weakSelf = self;
    UIButtonConfiguration *config = [UIButtonConfiguration grayButtonConfiguration];
    config.image = [UIImage systemImageNamed:@"arrow.down"];
    config.cornerStyle = UIButtonConfigurationCornerStyleCapsule;
    UIButton *bottom = [UIButton buttonWithConfiguration:config primaryAction:[UIAction actionWithHandler:^(UIAction *_) {
        [weakSelf.terminal.webView evaluateJavaScript:@"exports.scrollToBottom()" completionHandler:nil];
    }]];
    bottom.accessibilityLabel = @"Scroll to Bottom";
    bottom.translatesAutoresizingMaskIntoConstraints = NO;
    bottom.alpha = 0;
    bottom.userInteractionEnabled = NO;
    [self addSubview:bottom];
    [NSLayoutConstraint activateConstraints:@[
        [bottom.widthAnchor constraintEqualToConstant:40],
        [bottom.heightAnchor constraintEqualToConstant:40],
        [bottom.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-12],
        [bottom.bottomAnchor constraintEqualToAnchor:self.bottomAnchor constant:-12],
    ]];
    self.bottomButton = bottom;

    self.markedRange = [UITextRange new];
    self.selectedRange = [UITextRange new];
}

- (void)dealloc {
    self.terminal = nil;
}

- (void)observeValueForKeyPath:(NSString *)keyPath ofObject:(id)object change:(NSDictionary<NSKeyValueChangeKey,id> *)change context:(void *)context {
    if (object == _terminal) {
        if (_terminal.loaded) {
            [self installTerminalView];
            [self _updateStyle];
        }
    }
}

static NSString *const HANDLERS[] = {@"syncFocus", @"focus", @"newScrollRange", @"newScrollTop", @"newWheelScrolling", @"openLink",
                                     @"findResults", @"cursorRect", @"screenEdge"};

- (void)setTerminal:(Terminal *)terminal {
    if (_terminal) {
        [_terminal removeObserver:self forKeyPath:@"loaded"];
        [self uninstallTerminalView];
    }

    _terminal = terminal;
    [_terminal addObserver:self forKeyPath:@"loaded" options:NSKeyValueObservingOptionInitial context:nil];
    if (_terminal.loaded)
        [self installTerminalView];
}

- (void)installTerminalView {
    NSAssert(_terminal.loaded, @"should probably not be installing a non-loaded terminal");
    UIView *superview = self.terminal.webView.superview;
    if (superview != nil) {
        NSAssert(superview == self.scrollbarView, @"installing terminal that is already installed elsewhere");
        return;
    }

    WKWebView *webView = _terminal.webView;
    _terminal.enableVoiceOverAnnounce = YES;
    webView.scrollView.scrollEnabled = NO;
    // A tap on the status bar is the terminal's scroll view's (to the top of
    // the scrollback), which takes this one's and the tab strip's not having it
    webView.scrollView.scrollsToTop = NO;
    webView.scrollView.delaysContentTouches = NO;
    webView.scrollView.canCancelContentTouches = NO;
    webView.scrollView.panGestureRecognizer.enabled = NO;
    id <WKScriptMessageHandler> handler = [[WeakScriptMessageHandler alloc] initWithHandler:self];
    for (int i = 0; i < sizeof(HANDLERS)/sizeof(HANDLERS[0]); i++) {
        [webView.configuration.userContentController addScriptMessageHandler:handler name:HANDLERS[i]];
    }
    webView.frame = self.bounds;
    self.opaque = webView.opaque = NO;
    webView.backgroundColor = UIColor.clearColor;
    webView.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;

    self.scrollbarView.contentView = webView;
    [self.scrollbarView addSubview:webView];

    // The scrollbar still holds the previous terminal's metrics. Reset them without
    // telling the new terminal, then ask it for its own state: anything that happened
    // while it was detached (output, scrolling, focus changes) was not delivered to us.
    self.syncingScroll = YES;
    self.scrollbarView.contentSize = CGSizeZero;
    self.scrollbarView.contentOffset = CGPointZero;
    self.syncingScroll = NO;
    self.wheelScrolling = NO;
    [webView evaluateJavaScript:@"exports.resyncScroll()" completionHandler:nil];
    self.terminalFocused = self.isFirstResponder;
}

- (void)uninstallTerminalView {
    // remove old terminal
    UIView *superview = _terminal.webView.superview;
    if (superview != self.scrollbarView) {
        NSAssert(superview == nil, @"uninstalling terminal that is installed elsewhere");
        return;
    }

    [self stopWheelMomentum];
    // what an input method had put together stays with this view, not the page
    if (_markedText != nil) {
        _markedText = nil;
        [_terminal.webView evaluateJavaScript:@"exports.setComposition('')" completionHandler:nil];
    }
    [_terminal.webView removeFromSuperview];
    self.scrollbarView.contentView = nil;
    for (int i = 0; i < sizeof(HANDLERS)/sizeof(HANDLERS[0]); i++) {
        [_terminal.webView.configuration.userContentController removeScriptMessageHandlerForName:HANDLERS[i]];
    }
    _terminal.enableVoiceOverAnnounce = NO;
}

#pragma mark Styling

- (void)_updateStyle {
    NSAssert(NSThread.isMainThread, @"This method needs to be called on the main thread");
    if (!self.terminal.loaded)
        return;
    BOOL showSize = self.showsSizeAtNextStyle;
    self.showsSizeAtNextStyle = NO;
    UserPreferences *prefs = [UserPreferences shared];
    Palette *palette = prefs.palette;
    if (self.overrideAppearance != OverrideAppearanceNone) {
        palette = self.overrideAppearance == OverrideAppearanceLight ? prefs.theme.lightPalette : prefs.theme.darkPalette;
    }
    NSMutableDictionary<NSString *, id> *themeInfo = [@{
        @"fontFamily": prefs.fontFamily,
        @"fontSize": prefs.fontSize,
        @"foregroundColor": palette.foregroundColor,
        @"backgroundColor": palette.backgroundColor,
        @"blinkCursor": @(prefs.blinkCursor),
        @"minimumContrastRatio": @(UIAccessibilityDarkerSystemColorsEnabled() ? 4.5 : 1),
        @"cursorShape": prefs.cursorShapeName,
    } mutableCopy];
    if (prefs.palette.colorPaletteOverrides) {
        themeInfo[@"colorPaletteOverrides"] = palette.colorPaletteOverrides;
    }
    NSString *json = [[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:themeInfo options:0 error:nil] encoding:NSUTF8StringEncoding];
    int fontSize = prefs.fontSize.intValue;
    // updateStyle returns the columns and rows the style makes
    [self.terminal.webView evaluateJavaScript:[NSString stringWithFormat:@"exports.updateStyle(%@)", json] completionHandler:^(NSArray<NSNumber *> *size, NSError *error){
        if (error != nil) {
            NSLog(@"updateStyle: %@", error);
            return;
        }
        [self updateFloatingCursorSensitivity];
        if (showSize)
            [self showFontSize:fontSize columns:size[0].intValue rows:size[1].intValue];
    }];
}

// Increase Contrast, turned on or off in Settings: the terminal's colors
// lightened or darkened to read, or not (_updateStyle)
- (void)darkerColorsChanged:(NSNotification *)notification {
    [self _updateStyle];
}

- (void)setOverrideAppearance:(enum OverrideAppearance)overrideAppearance {
    _overrideAppearance = overrideAppearance;
    [self _updateStyle];
}

// The sizes the font can be: as small as can still be read, as big as
// Settings' stepper goes
static const int kMinFontSize = 6;
static const int kMaxFontSize = 72;

- (void)changeFontSize:(int)size {
    size = MAX(kMinFontSize, MIN(kMaxFontSize, size));
    UserPreferences *prefs = UserPreferences.shared;
    if (size == prefs.fontSize.intValue)
        return;
    // shown once the terminal has it: _updateStyle, which the preference's
    // observer calls on the main queue after this
    self.showsSizeAtNextStyle = YES;
    prefs.fontSize = @(size);
}

// A pinch zooms the font once it has gone a tenth either way (a two-finger
// swipe to another tab does not), then as far as the fingers go
- (void)pinched:(UIPinchGestureRecognizer *)pinch {
    switch (pinch.state) {
        case UIGestureRecognizerStateBegan:
            self.pinchStartSize = UserPreferences.shared.fontSize.intValue;
            self.pinchZooms = NO;
            break;
        case UIGestureRecognizerStateChanged:
            if (!self.pinchZooms) {
                if (fabs(log(pinch.scale)) < log(1.1))
                    return;
                // the fingers moving are the font's, not a scroll
                self.pinchZooms = YES;
                self.scrollbarView.panGestureRecognizer.enabled = NO;
            }
            [self changeFontSize:(int) round(self.pinchStartSize * pinch.scale)];
            break;
        default:
            if (self.pinchZooms)
                self.scrollbarView.panGestureRecognizer.enabled = YES;
            self.pinchZooms = NO;
            break;
    }
}

// Ctrl-L, as typed: a shell's clear, the prompt drawn again at the top; a
// full-screen program's redraw. Felt as it is sent.
- (void)clearTapped:(UITapGestureRecognizer *)tap {
    [self.pressFeedback impactOccurred];
    [self insertControlChar:'l'];
}

// The font's size and the terminal's columns and rows, over the terminal for
// a moment, as the font changes
- (void)showFontSize:(int)fontSize columns:(int)columns rows:(int)rows {
    if (self.sizeView == nil) {
        UIVisualEffectView *view = [[UIVisualEffectView alloc] initWithEffect:[UIBlurEffect effectWithStyle:UIBlurEffectStyleSystemMaterial]];
        view.userInteractionEnabled = NO;
        view.layer.cornerRadius = 12;
        view.clipsToBounds = YES;
        view.translatesAutoresizingMaskIntoConstraints = NO;
        UILabel *label = [UILabel new];
        label.font = [UIFont monospacedDigitSystemFontOfSize:17 weight:UIFontWeightSemibold];
        label.translatesAutoresizingMaskIntoConstraints = NO;
        [view.contentView addSubview:label];
        [self addSubview:view];
        [NSLayoutConstraint activateConstraints:@[
            [label.leadingAnchor constraintEqualToAnchor:view.contentView.leadingAnchor constant:16],
            [label.trailingAnchor constraintEqualToAnchor:view.contentView.trailingAnchor constant:-16],
            [label.topAnchor constraintEqualToAnchor:view.contentView.topAnchor constant:10],
            [label.bottomAnchor constraintEqualToAnchor:view.contentView.bottomAnchor constant:-10],
            [view.centerXAnchor constraintEqualToAnchor:self.centerXAnchor],
            [view.centerYAnchor constraintEqualToAnchor:self.centerYAnchor],
        ]];
        self.sizeView = view;
        self.sizeLabel = label;
    }
    UIView *view = self.sizeView;
    view.overrideUserInterfaceStyle = UserPreferences.shared.requestingDarkAppearance ? UIUserInterfaceStyleDark : UIUserInterfaceStyleLight;
    self.sizeLabel.text = [NSString stringWithFormat:@"%d pt · %d × %d", fontSize, columns, rows];
    [self bringSubviewToFront:view];
    [view.layer removeAllAnimations];
    view.alpha = 1;
    [UIView animateWithDuration:0.3 delay:1 options:0 animations:^{
        view.alpha = 0;
    } completion:nil];
}

#pragma mark Focus and scrolling

- (void)setTerminalFocused:(BOOL)terminalFocused {
    _terminalFocused = terminalFocused;
    NSString *script = terminalFocused ? @"exports.setFocused(true)" : @"exports.setFocused(false)";
    [self.terminal.webView evaluateJavaScript:script completionHandler:nil];
}

- (BOOL)becomeFirstResponder {
    self.terminalFocused = YES;
    // No reloadInputViews here. It does nothing before the view is first
    // responder, and once it is, becomeFirstResponder comes again when a tab
    // is tapped or UIKit brings the keyboard back with the scene: reloading
    // then took the extra keys off the keyboard and put them back, and the
    // terminal kept the bottom it had been given without them, the keys
    // covering its last line. A change of the extra keys reloads them itself
    // (TerminalViewController's _updateStyleFromPreferences:).
    return [super becomeFirstResponder];
}
- (BOOL)resignFirstResponder {
    self.terminalFocused = NO;
    return [super resignFirstResponder];
}
- (void)windowDidBecomeKey:(NSNotification *)notif {
    self.terminalFocused = YES;
}
- (void)windowDidResignKey:(NSNotification *)notif {
    self.terminalFocused = NO;
}

- (IBAction)loseFocus:(id)sender {
    [self resignFirstResponder];
}

- (void)willMoveToWindow:(UIWindow *)newWindow {
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    if (self.window != nil) {
        [center removeObserver:self
                          name:UIWindowDidBecomeKeyNotification
                        object:self.window];
        [center removeObserver:self
                          name:UIWindowDidResignKeyNotification
                        object:self.window];
    }
    if (newWindow != nil) {
        [center addObserver:self
                   selector:@selector(windowDidBecomeKey:)
                       name:UIWindowDidBecomeKeyNotification
                     object:newWindow];
        [center addObserver:self
                   selector:@selector(windowDidResignKey:)
                       name:UIWindowDidResignKeyNotification
                     object:newWindow];
    } else {
        [self stopWheelMomentum];
    }
}

- (void)userContentController:(WKUserContentController *)userContentController didReceiveScriptMessage:(WKScriptMessage *)message {
    if ([message.name isEqualToString:@"syncFocus"]) {
        self.terminalFocused = self.terminalFocused;
    } else if ([message.name isEqualToString:@"focus"]) {
        // {line, input, alternate} (term.js): the line tapped, the input's
        // first, in the buffer's lines
        NSDictionary *tap = message.body;
        BOOL inputOnly = self.agentInput || ![tap[@"alternate"] boolValue];
        if (inputOnly && [tap[@"line"] integerValue] < [tap[@"input"] integerValue] - 1)
            return;
        if (!self.isFirstResponder) {
            [self becomeFirstResponder];
        }
    } else if ([message.name isEqualToString:@"newScrollRange"]) {
        self.scrollRange = [message.body doubleValue];
        [self applyScrollRange];
        // output below what shows takes it further from the bottom
        [self updateBottomButton];
    } else if ([message.name isEqualToString:@"newScrollTop"]) {
        CGFloat newOffset = [message.body doubleValue];
        if (self.scrollbarView.contentOffset.y == newOffset)
            return;
        self.syncingScroll = YES;
        [self.scrollbarView setContentOffset:CGPointMake(0, newOffset) animated:NO];
        self.syncingScroll = NO;
    } else if ([message.name isEqualToString:@"newWheelScrolling"]) {
        self.wheelScrolling = [message.body boolValue];
        if (!self.wheelScrolling)
            [self stopWheelMomentum];
        [self updateBottomButton];
    } else if ([message.name isEqualToString:@"openLink"]) {
        [UIApplication openURL:message.body];
    } else if ([message.name isEqualToString:@"findResults"]) {
        NSArray<NSNumber *> *results = message.body;
        if (self.findResults != nil)
            self.findResults(results[0].integerValue, results[1].integerValue);
    } else if ([message.name isEqualToString:@"cursorRect"]) {
        NSArray<NSNumber *> *rect = message.body;
        self.cursorRect = CGRectMake(rect[0].doubleValue, rect[1].doubleValue, rect[2].doubleValue, rect[3].doubleValue);
    } else if ([message.name isEqualToString:@"screenEdge"]) {
        // The page's points are this view's: the web view is at its left
        // edge, as wide as it is (installTerminalView)
        self.screenEdge = [message.body doubleValue];
        self.marginLeading.constant = self.screenEdge;
        self.marginView.hidden = NO;
    }
}

- (void)setMarginView:(UIView *)marginView {
    if (marginView == _marginView)
        return;
    [_marginView removeFromSuperview];
    _marginView = marginView;
    self.marginLeading = nil;
    if (marginView == nil)
        return;
    marginView.translatesAutoresizingMaskIntoConstraints = NO;
    marginView.hidden = self.screenEdge == 0;
    // over the terminal, under the button down and the font's size
    [self insertSubview:marginView aboveSubview:self.scrollbarView];
    self.marginLeading = [marginView.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:self.screenEdge];
    [NSLayoutConstraint activateConstraints:@[
        self.marginLeading,
        [marginView.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        [marginView.topAnchor constraintEqualToAnchor:self.topAnchor],
        [marginView.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
    ]];
}

#pragma mark Selection

- (void)selectByPress:(UILongPressGestureRecognizer *)press {
    WKWebView *webView = self.terminal.webView;
    if (webView.superview == nil)
        return;
    NSString *script;
    switch (press.state) {
        case UIGestureRecognizerStateBegan:
            // the drag is the selection's, not a scroll
            self.scrollbarView.panGestureRecognizer.enabled = NO;
            // a tap felt as the word is taken, as in a text field
            [self.pressFeedback impactOccurred];
            script = @"exports.selectWordAt(x, y)";
            break;
        case UIGestureRecognizerStateChanged:
            script = @"exports.extendSelectionTo(x, y)";
            break;
        case UIGestureRecognizerStateEnded:
            self.scrollbarView.panGestureRecognizer.enabled = YES;
            [self showSelectionMenu];
            return;
        default:
            self.scrollbarView.panGestureRecognizer.enabled = YES;
            return;
    }
    CGPoint point = [press locationInView:webView];
    [webView callAsyncJavaScript:script
                       arguments:@{@"x": @(point.x), @"y": @(point.y)}
                         inFrame:nil
                  inContentWorld:WKContentWorld.pageWorld
               completionHandler:nil];
}

// The selection's menu, pointing at its rows
- (void)showSelectionMenu {
    WKWebView *webView = self.terminal.webView;
    [webView callAsyncJavaScript:@"return exports.getSelection()"
                       arguments:@{}
                         inFrame:nil
                  inContentWorld:WKContentWorld.pageWorld
               completionHandler:^(id selection, NSError *error) {
        if (![selection isKindOfClass:NSDictionary.class])
            return;
        // the URL a link long pressed opens, else null
        id link = selection[@"link"];
        self.selectionLink = link == NSNull.null ? nil : link;
        NSArray<NSNumber *> *rect = selection[@"rect"];
        self.selectionRect = [self convertRect:CGRectMake(rect[0].doubleValue, rect[1].doubleValue,
                                                          rect[2].doubleValue, rect[3].doubleValue)
                                      fromView:webView];
        CGPoint top = CGPointMake(CGRectGetMidX(self.selectionRect), CGRectGetMinY(self.selectionRect));
        if (self.editMenu == nil) {
            self.editMenu = [[UIEditMenuInteraction alloc] initWithDelegate:self];
            [self addInteraction:self.editMenu];
        }
        [self.editMenu presentEditMenuWithConfiguration:[UIEditMenuConfiguration configurationWithIdentifier:nil sourcePoint:top]];
    }];
}

- (UIMenu *)editMenuInteraction:(UIEditMenuInteraction *)interaction menuForConfiguration:(UIEditMenuConfiguration *)configuration suggestedActions:(NSArray<UIMenuElement *> *)suggestedActions {
    // Its own actions, not the first responder's: the keyboard need not be up
    __weak typeof(self) weakSelf = self;
    NSMutableArray<UIMenuElement *> *actions = [NSMutableArray new];
    if (self.selectionLink != nil) {
        [actions addObject:[UIAction actionWithTitle:@"Open Link" image:[UIImage systemImageNamed:@"safari"] identifier:nil handler:^(UIAction *_) {
            [weakSelf openSelectedLink:nil];
        }]];
    }
    [actions addObject:[UIAction actionWithTitle:@"Copy" image:[UIImage systemImageNamed:@"doc.on.doc"] identifier:nil handler:^(UIAction *_) {
        [weakSelf copy:nil];
    }]];
    return [UIMenu menuWithChildren:actions];
}

// Open Link, in the menu of a link long pressed
- (void)openSelectedLink:(id)sender {
    [UIApplication openURL:self.selectionLink];
    [self.terminal.webView evaluateJavaScript:@"exports.clearSelection()" completionHandler:nil];
}


- (CGRect)editMenuInteraction:(UIEditMenuInteraction *)interaction targetRectForConfiguration:(UIEditMenuConfiguration *)configuration {
    return self.selectionRect;
}

- (void)linkClicked:(UITapGestureRecognizer *)recognizer {
    if (!(recognizer.modifierFlags & UIKeyModifierCommand))
        return;
    WKWebView *webView = self.terminal.webView;
    if (webView.superview == nil)
        return;
    CGPoint point = [recognizer locationInView:webView];
    [webView evaluateJavaScript:[NSString stringWithFormat:@"exports.openLinkAt(%f, %f)", point.x, point.y] completionHandler:nil];
}

- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer shouldRecognizeSimultaneouslyWithGestureRecognizer:(UIGestureRecognizer *)otherGestureRecognizer {
    return YES;
}

// The margin view's tap has the touches in it, while it shows
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer shouldReceiveTouch:(UITouch *)touch {
    if (gestureRecognizer != self.marginTap)
        return YES;
    UIView *margin = self.marginView;
    return margin != nil && !margin.hidden && CGRectContainsPoint(margin.frame, [touch locationInView:self]);
}

// The page's click there (the keyboard brought up, a program's mouse) waits
// for the margin view's tap to fail: a drag, a long press.
// And the web view's taps, which click, wait for the long press that selects
// to fail: one let go after the press had taken its word was a click as well,
// which xterm.js took for a new selection, none, and term.js for a tap to
// bring the keyboard up. The menu came up with the keyboard, its selection
// gone before Copy was tapped. A tap let go of before the press is due ends
// the press there and clicks at once.
- (BOOL)gestureRecognizer:(UIGestureRecognizer *)gestureRecognizer shouldBeRequiredToFailByGestureRecognizer:(UIGestureRecognizer *)otherGestureRecognizer {
    if (![otherGestureRecognizer.view isDescendantOfView:self.terminal.webView])
        return NO;
    if (gestureRecognizer == self.marginTap)
        return YES;
    return gestureRecognizer == self.selectPress && [otherGestureRecognizer isKindOfClass:UITapGestureRecognizer.class];
}

- (void)tappedMargin:(UITapGestureRecognizer *)tap {
    self.marginTapped();
}

// The button down to the bottom: there while what shows is more than a screen
// above it, which a full-screen program's screen never is
- (void)updateBottomButton {
    UIScrollView *scroll = self.scrollbarView;
    BOOL shown = !self.wheelScrolling && self.scrollRange - scroll.contentOffset.y > scroll.bounds.size.height;
    if (shown == self.bottomButton.userInteractionEnabled)
        return;
    self.bottomButton.userInteractionEnabled = shown;
    [UIView animateWithDuration:0.2 animations:^{
        self.bottomButton.alpha = shown ? 1 : 0;
    }];
}

- (void)applyScrollRange {
    CGFloat height = self.scrollRange + self.scrollbarView.bounds.size.height;
    if (self.scrollbarView.contentSize.height == height)
        return;
    // Changing the size can clamp the offset, which must not be echoed back as a scroll.
    self.syncingScroll = YES;
    self.scrollbarView.contentSize = CGSizeMake(0, height);
    self.syncingScroll = NO;
}

// The scroll view as high as the terminal shows (the keyboard up or down, a
// rotation, a foldable unfolded): its content with it
- (void)layoutSubviews {
    [super layoutSubviews];
    [self applyScrollRange];
}

- (void)scrollViewDidScroll:(UIScrollView *)scrollView {
    [self updateBottomButton];
    if (self.syncingScroll)
        return;
    [self.terminal.webView evaluateJavaScript:[NSString stringWithFormat:@"exports.newScrollTop(%f)", scrollView.contentOffset.y] completionHandler:nil];
}

// A drag in a full-screen program's screen turns the program's wheel, by the
// finger's way down (term.js's wheel); let go while moving, the wheel goes on.
// The scroll view's own scroll there is its bounce, which does not show.
- (void)scrollPanned:(UIPanGestureRecognizer *)pan {
    if (pan.state == UIGestureRecognizerStateBegan) {
        [self stopWheelMomentum];
        self.wheelSent = 0;
    }
    WKWebView *webView = self.terminal.webView;
    if (!self.wheelScrolling || webView.superview == nil)
        return;
    CGPoint point = [pan locationInView:webView];
    switch (pan.state) {
        case UIGestureRecognizerStateBegan:
        case UIGestureRecognizerStateChanged: {
            CGFloat way = [pan translationInView:self].y;
            [self turnWheelBy:way - self.wheelSent at:point];
            self.wheelSent = way;
            break;
        }
        case UIGestureRecognizerStateEnded:
            [self startWheelMomentum:[pan velocityInView:self].y at:point];
            break;
        default:
            break;
    }
}

- (void)turnWheelBy:(CGFloat)way at:(CGPoint)point {
    if (way == 0)
        return;
    [self.terminal.webView callAsyncJavaScript:@"exports.wheel(dy, x, y)"
                                     arguments:@{@"dy": @(way), @"x": @(point.x), @"y": @(point.y)}
                                       inFrame:nil
                                inContentWorld:WKContentWorld.pageWorld
                             completionHandler:nil];
}

- (void)startWheelMomentum:(CGFloat)velocity at:(CGPoint)point {
    [self stopWheelMomentum];
    self.wheelVelocity = velocity;
    self.wheelPoint = point;
    self.wheelMomentum = [CADisplayLink displayLinkWithTarget:self selector:@selector(wheelMomentumStep:)];
    [self.wheelMomentum addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
}

- (void)wheelMomentumStep:(CADisplayLink *)link {
    CFTimeInterval frame = link.targetTimestamp - link.timestamp;
    // A scroll view's deceleration rate is per millisecond
    self.wheelVelocity *= pow(UIScrollViewDecelerationRateNormal, frame * 1000);
    if (!self.wheelScrolling || fabs(self.wheelVelocity) < 20) {
        [self stopWheelMomentum];
        return;
    }
    [self turnWheelBy:self.wheelVelocity * frame at:self.wheelPoint];
}

// The display link holds this view: stopped when the view goes or its terminal does
- (void)stopWheelMomentum {
    [self.wheelMomentum invalidate];
    self.wheelMomentum = nil;
}

- (void)setKeyboardAppearance:(UIKeyboardAppearance)keyboardAppearance {
    BOOL needsFirstResponderDance = self.isFirstResponder && _keyboardAppearance != keyboardAppearance;
    if (needsFirstResponderDance) {
        [self resignFirstResponder];
    }
    _keyboardAppearance = keyboardAppearance;
    if (needsFirstResponderDance) {
        [self becomeFirstResponder];
    }
    if (keyboardAppearance == UIKeyboardAppearanceLight) {
        self.scrollbarView.indicatorStyle = UIScrollViewIndicatorStyleBlack;
    } else {
        self.scrollbarView.indicatorStyle = UIScrollViewIndicatorStyleWhite;
    }
}

#pragma mark Keyboard Input

// implementing these makes a keyboard pop up when this view is first responder

// Whether a bar modifier key applies to the next key. One held down applies
// while it is held; one toggled on applies once and is cleared.
static BOOL takeBarKey(UIButton *key) {
    if (key.highlighted)
        key.selected = YES;
    if (!key.selected)
        return NO;
    if (!key.highlighted)
        key.selected = NO;
    return YES;
}

- (UIKeyModifierFlags)takeBarModifiers {
    UIKeyModifierFlags modifiers = 0;
    if (takeBarKey(self.controlKey))
        modifiers |= UIKeyModifierControl;
    return modifiers;
}

- (void)insertText:(NSString *)text {
    self.markedText = nil;

    // The bar's modifiers apply to a single key. Pasted text and the escape
    // sequences of arrows and other special keys use them up unchanged.
    UIKeyModifierFlags modifiers = [self takeBarModifiers];
    if (text.length == 1 && (modifiers & UIKeyModifierControl))
        return [self insertControlChar:[text characterAtIndex:0]];

    text = [text stringByReplacingOccurrencesOfString:@"\n" withString:@"\r"];
    NSData *data = [text dataUsingEncoding:NSUTF8StringEncoding];
    [self.terminal sendInput:data];
}

- (void)insertSequence:(NSString *)sequence {
    self.markedText = nil;
    [self.terminal sendInput:[sequence dataUsingEncoding:NSUTF8StringEncoding]];
}

// What Ctrl and a key send, as xterm sends it: a letter's control character;
// NUL for space, @ and 2; ESC, FS, GS and RS for [ \ ] and ^ (and 6); US for
// _, - and /. A key with none (tab, esc, a digit, return) is sent as it is.
static int controlCharacter(unichar key) {
    if (key >= 'a' && key <= 'z')
        return key - 'a' + 1;
    if (key >= 'A' && key <= 'Z')
        return key - 'A' + 1;
    switch (key) {
        case ' ': case '@': case '2': return 0x00;
        case '[': return 0x1b;
        case '\\': return 0x1c;
        case ']': return 0x1d;
        case '^': case '6': return 0x1e;
        case '_': case '-': case '/': return 0x1f;
        default: return -1;
    }
}

- (void)insertControlChar:(unichar)key {
    int control = controlCharacter(key);
    if (control < 0) {
        NSString *text = key == '\n' ? @"\r" : [NSString stringWithCharacters:&key length:1];
        [self.terminal sendInput:[text dataUsingEncoding:NSUTF8StringEncoding]];
        return;
    }
    char byte = (char) control;
    [self.terminal sendInput:[NSData dataWithBytes:&byte length:1]];
}

- (void)deleteBackward {
    [self insertText:@"\x7f"];
}

- (BOOL)hasText {
    return YES; // it's always ok to send a "delete"
}

#pragma mark IME Input and Selection

- (void)setMarkedText:(nullable NSString *)markedText selectedRange:(NSRange)selectedRange {
    self.markedText = markedText;
}

- (void)setMarkedText:(NSString *)markedText {
    if (markedText == _markedText || [markedText isEqualToString:_markedText])
        return;
    _markedText = markedText;
    [self.terminal.webView callAsyncJavaScript:@"exports.setComposition(text)"
                                     arguments:@{@"text": markedText ?: @""}
                                       inFrame:nil
                                inContentWorld:WKContentWorld.pageWorld
                             completionHandler:nil];
}

// The cursor's cell, where an input method shows its candidates by what it
// puts together (an iPad's, with a keyboard)
- (CGRect)cursorRectInView {
    WKWebView *webView = self.terminal.webView;
    if (webView.superview == nil)
        return CGRectZero;
    return [self convertRect:self.cursorRect fromView:webView];
}

- (void)unmarkText {
    [self insertText:self.markedText];
}

- (UITextRange *)markedTextRange {
    if (self.markedText != nil)
        return self.markedRange;
    return nil;
}

// The only reason to have this selected range is to prevent the "speak selection" context action from failing to get the current selection and falling back on calling copy:. It doesn't even have to work, it seems...

- (UITextRange *)selectedTextRange {
    return self.selectedRange;
}

- (NSString *)textInRange:(UITextRange *)range {
    if (range == self.markedRange)
        return self.markedText;
    if (range == self.selectedRange)
        return @"";
    return nil;
}

- (id)insertDictationResultPlaceholder {
    return @"";
}
- (void)removeDictationResultPlaceholder:(id)placeholder willInsertResult:(BOOL)willInsertResult {
}

#pragma mark Keyboard Actions

- (void)paste:(id)sender {
    if (self.pasteInterceptor != nil && self.pasteInterceptor())
        return;
    NSString *string = UIPasteboard.generalPasteboard.string;
    if (string) {
        // bracketed if the program asked for that, so a pasted line is not
        // run as it comes
        [self.terminal pasteText:string];
    }
}

// The page's selection (a long press's, a pointer's drag), to the clipboard
- (void)copy:(id)sender {
    WKWebView *webView = self.terminal.webView;
    [webView callAsyncJavaScript:@"return exports.getSelection()"
                       arguments:@{}
                         inFrame:nil
                  inContentWorld:WKContentWorld.pageWorld
               completionHandler:^(id selection, NSError *error) {
        if (![selection isKindOfClass:NSDictionary.class])
            return;
        UIPasteboard.generalPasteboard.string = selection[@"text"];
        [webView evaluateJavaScript:@"exports.clearSelection()" completionHandler:nil];
    }];
}

- (void)clearScrollback {
    [self.terminal.webView evaluateJavaScript:@"exports.clearScrollback()" completionHandler:nil];
}

- (void)clearScreen {
    [self.terminal.webView evaluateJavaScript:@"exports.clearScreen()" completionHandler:nil];
}

- (void)resetTerminal {
    // A reset also drops the palette overrides the theme installed.
    [self.terminal.webView evaluateJavaScript:@"exports.reset()" completionHandler:^(id result, NSError *error) {
        [self _updateStyle];
    }];
}

- (void)find:(NSString *)text backwards:(BOOL)backwards incremental:(BOOL)incremental {
    [self.terminal.webView callAsyncJavaScript:@"exports.find(text, backwards, incremental)"
                                     arguments:@{@"text": text, @"backwards": @(backwards), @"incremental": @(incremental)}
                                       inFrame:nil
                                inContentWorld:WKContentWorld.pageWorld
                             completionHandler:nil];
}

- (void)clearFind {
    [self.terminal.webView evaluateJavaScript:@"exports.clearFind()" completionHandler:nil];
}

- (void)fetchTextWithCompletion:(void (^)(NSString *))completion {
    [self.terminal.webView evaluateJavaScript:@"exports.getText()" completionHandler:^(id result, NSError *error) {
        completion([result isKindOfClass:NSString.class] ? result : @"");
    }];
}

#pragma mark Floating cursor

- (void)updateFloatingCursorSensitivity {
    [self.terminal.webView evaluateJavaScript:@"exports.getCharacterSize()" completionHandler:^(NSArray *charSizeRaw, NSError *error) {
        if (error != nil) {
            NSLog(@"error getting character size: %@", error);
            return;
        }
        CGSize charSize = CGSizeMake([charSizeRaw[0] doubleValue], [charSizeRaw[1] doubleValue]);
        double sensitivity = 0.5;
        self.floatingCursorSensitivity = CGSizeMake(charSize.width / sensitivity, charSize.height / sensitivity);
    }];
}

- (struct rowcol)rowcolFromPoint:(CGPoint)point {
    CGSize sensitivity = self.actualFloatingCursorSensitivity;
    return (struct rowcol) {
        .row = (int) (-point.y / sensitivity.height),
        .col = (int) (point.x / sensitivity.width),
    };
}

- (void)beginFloatingCursorAtPoint:(CGPoint)point {
    self.actualFloatingCursorSensitivity = self.floatingCursorSensitivity;
    self.floatingCursor = [self rowcolFromPoint:point];
}

- (void)updateFloatingCursorAtPoint:(CGPoint)point {
    struct rowcol newPos = [self rowcolFromPoint:point];
    int rowDiff = newPos.row - self.floatingCursor.row;
    int colDiff = newPos.col - self.floatingCursor.col;
    NSMutableString *arrows = [NSMutableString string];
    for (int i = 0; i < abs(rowDiff); i++) {
        [arrows appendString:[self.terminal arrow:rowDiff > 0 ? 'A': 'B']];
    }
    for (int i = 0; i < abs(colDiff); i++) {
        [arrows appendString:[self.terminal arrow:colDiff > 0 ? 'C': 'D']];
    }
    [self insertText:arrows];
    self.floatingCursor = newPos;
}

- (void)endFloatingCursor {
    self.floatingCursor = (struct rowcol) {};
}

#pragma mark Keyboard Traits

- (UITextSmartDashesType)smartDashesType {
    return UITextSmartDashesTypeNo;
}
- (UITextSmartQuotesType)smartQuotesType {
    return UITextSmartQuotesTypeNo;
}
- (UITextSmartInsertDeleteType)smartInsertDeleteType {
    return UITextSmartInsertDeleteTypeNo;
}
- (UITextAutocapitalizationType)autocapitalizationType {
    return UITextAutocapitalizationTypeNone;
}
- (UITextAutocorrectionType)autocorrectionType {
    return UITextAutocorrectionTypeNo;
}
// Apparently required on iOS 15+: https://stackoverflow.com/a/72359764
- (UITextSpellCheckingType)spellCheckingType {
    return UITextSpellCheckingTypeNo;
}

#pragma mark Hardware Keyboard

// Escape sequence for a navigation or function key, following xterm conventions:
// unmodified keys use the terminal's cursor-key mode (CSI vs SS3); with modifiers
// the xterm modifier parameter is appended (2=Shift, 3=Alt, 5=Ctrl, 6=Ctrl+Shift, ...).
// Returns nil if the key is not a navigation/function key.
- (NSString *)specialKeySequence:(NSString *)key modifiers:(UIKeyModifierFlags)mods {
    char csiFinal = 0; // keys of the form CSI [1;m] X
    int tildeCode = 0; // keys of the form CSI n [;m] ~
    if ([key isEqualToString:UIKeyInputUpArrow])         csiFinal = 'A';
    else if ([key isEqualToString:UIKeyInputDownArrow])  csiFinal = 'B';
    else if ([key isEqualToString:UIKeyInputRightArrow]) csiFinal = 'C';
    else if ([key isEqualToString:UIKeyInputLeftArrow])  csiFinal = 'D';
    else if ([key isEqualToString:UIKeyInputHome])       csiFinal = 'H';
    else if ([key isEqualToString:UIKeyInputEnd])        csiFinal = 'F';
    else if ([key isEqualToString:UIKeyInputF1])         csiFinal = 'P';
    else if ([key isEqualToString:UIKeyInputF2])         csiFinal = 'Q';
    else if ([key isEqualToString:UIKeyInputF3])         csiFinal = 'R';
    else if ([key isEqualToString:UIKeyInputF4])         csiFinal = 'S';
    else if ([key isEqualToString:UIKeyInputPageUp])     tildeCode = 5;
    else if ([key isEqualToString:UIKeyInputPageDown])   tildeCode = 6;
    else if ([key isEqualToString:UIKeyInputF5])         tildeCode = 15;
    else if ([key isEqualToString:UIKeyInputF6])         tildeCode = 17;
    else if ([key isEqualToString:UIKeyInputF7])         tildeCode = 18;
    else if ([key isEqualToString:UIKeyInputF8])         tildeCode = 19;
    else if ([key isEqualToString:UIKeyInputF9])         tildeCode = 20;
    else if ([key isEqualToString:UIKeyInputF10])        tildeCode = 21;
    else if ([key isEqualToString:UIKeyInputF11])        tildeCode = 23;
    else if ([key isEqualToString:UIKeyInputF12])        tildeCode = 24;
    else return nil;

    int m = 1;
    if (mods & UIKeyModifierShift)     m += 1;
    if (mods & UIKeyModifierAlternate) m += 2;
    if (mods & UIKeyModifierControl)   m += 4;

    if (tildeCode != 0) {
        if (m == 1)
            return [NSString stringWithFormat:@"\x1b[%d~", tildeCode];
        return [NSString stringWithFormat:@"\x1b[%d;%d~", tildeCode, m];
    }
    if (m == 1) {
        if (csiFinal >= 'P' && csiFinal <= 'S') // F1-F4 are always SS3 when unmodified
            return [NSString stringWithFormat:@"\x1bO%c", csiFinal];
        return [self.terminal arrow:csiFinal]; // honours DECCKM (application cursor mode)
    }
    return [NSString stringWithFormat:@"\x1b[1;%d%c", m, csiFinal];
}

- (void)handleKeyCommand:(UIKeyCommand *)command {
    NSString *key = command.input;
    // Shift-Tab: back tab, as xterm sends it (claude's next mode, a
    // completion menu's previous entry)
    if ([key isEqualToString:@"\t"] && command.modifierFlags == UIKeyModifierShift) {
        [self insertSequence:@"\x1b[Z"];
        return;
    }
    // Shift with Page Up, Page Down, Home or End scrolls what the terminal
    // holds, as in xterm, where there is anything above to scroll: a
    // full-screen program's screen takes them as keys
    if (command.modifierFlags == UIKeyModifierShift && !self.wheelScrolling) {
        NSDictionary<NSString *, NSString *> *scrolls = @{
            UIKeyInputPageUp: @"exports.scrollPages(-1)",
            UIKeyInputPageDown: @"exports.scrollPages(1)",
            UIKeyInputHome: @"exports.scrollToTop()",
            UIKeyInputEnd: @"exports.scrollToBottom()",
        };
        if (scrolls[key] != nil) {
            [self.terminal.webView evaluateJavaScript:scrolls[key] completionHandler:nil];
            return;
        }
    }
    NSString *special = [self specialKeySequence:key modifiers:command.modifierFlags];
    if (special != nil) {
        [self insertText:special];
        return;
    }
    if (command.modifierFlags == 0) {
        if ([key isEqualToString:@"`"] && UserPreferences.shared.backtickMapEscape)
            key = UIKeyInputEscape;
        if ([key isEqualToString:UIKeyInputEscape])
            key = @"\x1b";
        [self insertText:key];
    } else if (command.modifierFlags & UIKeyModifierShift) {
        [self insertText:[key uppercaseString]];
    } else if (command.modifierFlags & UIKeyModifierAlternate) {
        [self insertText:[@"\x1b" stringByAppendingString:key]];
    } else if (command.modifierFlags & UIKeyModifierAlphaShift) {
        [self handleCapsLockWithCommand:command];
    } else if (command.modifierFlags & UIKeyModifierControl || command.modifierFlags & UIKeyModifierAlphaShift) {
        if (key.length == 0)
            return;
        [self insertControlChar:[key characterAtIndex:0]];
    }
}

// The keys that have a control character (controlCharacter)
static const char *controlKeys = "abcdefghijklmnopqrstuvwxyz@^26-/[]\\ ";
static const char *metaKeys = "abcdefghijklmnopqrstuvwxyz0123456789-=[]\\;',./";

- (NSArray<UIKeyCommand *> *)keyCommands {
    if (_keyCommands != nil)
        return _keyCommands;
    _keyCommands = [NSMutableArray new];
    [self addKeys:controlKeys withModifiers:UIKeyModifierControl];
    for (NSString *specialKey in @[UIKeyInputEscape, @"\t"]) {
        [self addKey:specialKey withModifiers:0];
    }
    [self addKey:@"\t" withModifiers:UIKeyModifierShift];
    // Navigation and function keys, with every Shift/Alt/Ctrl combination, so that
    // (UIKeyInputDelete is deliberately absent: it matches Backspace, which must keep going through deleteBackward.)
    // e.g. Ctrl+Arrow reaches the terminal as \e[1;5C instead of being eaten by iPadOS.
    NSArray<NSString *> *navKeys = @[UIKeyInputUpArrow, UIKeyInputDownArrow, UIKeyInputLeftArrow, UIKeyInputRightArrow,
                                     UIKeyInputHome, UIKeyInputEnd, UIKeyInputPageUp, UIKeyInputPageDown,
                                     UIKeyInputF1, UIKeyInputF2, UIKeyInputF3, UIKeyInputF4, UIKeyInputF5, UIKeyInputF6,
                                     UIKeyInputF7, UIKeyInputF8, UIKeyInputF9, UIKeyInputF10, UIKeyInputF11, UIKeyInputF12];
    UIKeyModifierFlags modifierCombos[] = {
        0,
        UIKeyModifierShift,
        UIKeyModifierAlternate,
        UIKeyModifierControl,
        UIKeyModifierShift | UIKeyModifierAlternate,
        UIKeyModifierShift | UIKeyModifierControl,
        UIKeyModifierAlternate | UIKeyModifierControl,
        UIKeyModifierShift | UIKeyModifierAlternate | UIKeyModifierControl,
    };
    for (NSString *navKey in navKeys) {
        for (size_t i = 0; i < sizeof(modifierCombos) / sizeof(modifierCombos[0]); i++) {
            [self addKey:navKey withModifiers:modifierCombos[i]];
        }
    }
    if (UserPreferences.shared.optionMapping == OptionMapEsc) {
        [self addKeys:metaKeys withModifiers:UIKeyModifierAlternate];
        // ESC CR, as Terminal.app's Option as Meta sends it: Claude Code's
        // newline that does not send
        [self addKey:@"\r" withModifiers:UIKeyModifierAlternate];
    }
    if (UserPreferences.shared.backtickMapEscape) {
        [self addKey:@"`" withModifiers:0];
    }
    return _keyCommands;
}

- (void)addKeys:(const char *)keys withModifiers:(UIKeyModifierFlags)modifiers {
    for (size_t i = 0; keys[i] != '\0'; i++) {
        [self addKey:[NSString stringWithFormat:@"%c", keys[i]] withModifiers:modifiers];
    }
}

- (void)addKey:(NSString *)key withModifiers:(UIKeyModifierFlags)modifiers {
    UIKeyCommand *command = [UIKeyCommand keyCommandWithInput:key
                                                modifierFlags:modifiers
                                                       action:@selector(handleKeyCommand:)];
    command.wantsPriorityOverSystemBehavior = YES;
    [_keyCommands addObject:command];
}

- (void)keyCommandTriggered:(UIKeyCommand *)sender {
    dispatch_async(dispatch_get_main_queue(), ^{
        [self handleKeyCommand:sender];
    });
}

- (void)handleCapsLockWithCommand:(UIKeyCommand *)command {
    CapsLockMapping target = UserPreferences.shared.capsLockMapping;
    NSString *newInput = command.input ? command.input : @"";
    UIKeyModifierFlags flags = command.modifierFlags;
    flags ^= UIKeyModifierAlphaShift;
    if(target == CapsLockMapEscape) {
        newInput = UIKeyInputEscape;
    } else if(target == CapsLockMapControl) {
        if([newInput length] == 0) {
            return;
        }
        flags |= UIKeyModifierControl;
    } else {
        return;
    }

    UIKeyCommand *newCommand = [UIKeyCommand keyCommandWithInput:newInput
                                                   modifierFlags:flags
                                                          action:@selector(keyCommandTriggered:)];
    [self handleKeyCommand:newCommand];
}

- (void)pressesBegan:(NSSet<UIPress *> *)presses withEvent:(UIPressesEvent *)event {
    UIKey *key = presses.anyObject.key;
    if (UserPreferences.shared.overrideControlSpace &&
        key.keyCode == UIKeyboardHIDUsageKeyboardSpacebar &&
        key.modifierFlags & UIKeyModifierControl) {
        return [self insertControlChar:' '];
    }
    return [super pressesBegan:presses withEvent:event];
}

#pragma mark UITextInput stubs

#if 0
#define LogStub() NSLog(@"%s", __func__)
#else
#define LogStub()
#endif

- (NSWritingDirection)baseWritingDirectionForPosition:(nonnull UITextPosition *)position inDirection:(UITextStorageDirection)direction { LogStub(); return NSWritingDirectionLeftToRight; }
- (void)setBaseWritingDirection:(NSWritingDirection)writingDirection forRange:(nonnull UITextRange *)range { LogStub(); }
- (UITextPosition *)beginningOfDocument { LogStub(); return nil; }
- (CGRect)caretRectForPosition:(nonnull UITextPosition *)position { return [self cursorRectInView]; }
- (nullable UITextRange *)characterRangeAtPoint:(CGPoint)point { LogStub(); return nil; }
- (nullable UITextRange *)characterRangeByExtendingPosition:(nonnull UITextPosition *)position inDirection:(UITextLayoutDirection)direction { LogStub(); return nil; }
- (nullable UITextPosition *)closestPositionToPoint:(CGPoint)point { LogStub(); return nil; }
- (nullable UITextPosition *)closestPositionToPoint:(CGPoint)point withinRange:(nonnull UITextRange *)range { LogStub(); return nil; }
- (NSComparisonResult)comparePosition:(nonnull UITextPosition *)position toPosition:(nonnull UITextPosition *)other { LogStub(); return NSOrderedSame; }
- (UITextPosition *)endOfDocument { LogStub(); return nil; }
- (CGRect)firstRectForRange:(nonnull UITextRange *)range { return [self cursorRectInView]; }
- (NSDictionary<NSAttributedStringKey,id> *)markedTextStyle { LogStub(); return nil; }
- (void)setMarkedTextStyle:(NSDictionary<NSAttributedStringKey,id> *)markedTextStyle { LogStub(); }
- (NSInteger)offsetFromPosition:(nonnull UITextPosition *)from toPosition:(nonnull UITextPosition *)toPosition { LogStub(); return 0; }
- (nullable UITextPosition *)positionFromPosition:(nonnull UITextPosition *)position inDirection:(UITextLayoutDirection)direction offset:(NSInteger)offset { LogStub(); return nil; }
- (nullable UITextPosition *)positionFromPosition:(nonnull UITextPosition *)position offset:(NSInteger)offset { LogStub(); return nil; }
- (nullable UITextPosition *)positionWithinRange:(nonnull UITextRange *)range farthestInDirection:(UITextLayoutDirection)direction { LogStub(); return nil; }
- (void)replaceRange:(nonnull UITextRange *)range withText:(nonnull NSString *)text { LogStub(); }
- (void)setSelectedTextRange:(UITextRange *)selectedTextRange { LogStub(); }
- (nonnull NSArray<UITextSelectionRect *> *)selectionRectsForRange:(nonnull UITextRange *)range { LogStub(); return @[]; }
- (nullable UITextRange *)textRangeFromPosition:(nonnull UITextPosition *)fromPosition toPosition:(nonnull UITextPosition *)toPosition { LogStub(); return nil; }

// conforming to UITextInput makes this view default to being an accessibility element, which blocks selecting anything in it
- (BOOL)isAccessibilityElement { return NO; }

@end
