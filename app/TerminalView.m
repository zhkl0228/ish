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

@property (nullable) NSString *markedText;
@property (nullable) NSString *selectedText;
@property UITextRange *markedRange;
@property UITextRange *selectedRange;

// The selection's menu (Copy), and where it points: the selection's rows
@property UIEditMenuInteraction *editMenu API_AVAILABLE(ios(16.0));
@property CGRect selectionRect;

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

static NSString *const HANDLERS[] = {@"syncFocus", @"focus", @"newScrollRange", @"newScrollTop", @"newWheelScrolling", @"openLink"};

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
    UserPreferences *prefs = [UserPreferences shared];
    if (_overrideFontSize == prefs.fontSize.doubleValue)
        _overrideFontSize = 0;
    Palette *palette = prefs.palette;
    if (self.overrideAppearance != OverrideAppearanceNone) {
        palette = self.overrideAppearance == OverrideAppearanceLight ? prefs.theme.lightPalette : prefs.theme.darkPalette;
    }
    NSMutableDictionary<NSString *, id> *themeInfo = [@{
        @"fontFamily": prefs.fontFamily,
        @"fontSize": @(self.effectiveFontSize),
        @"foregroundColor": palette.foregroundColor,
        @"backgroundColor": palette.backgroundColor,
        @"blinkCursor": @(prefs.blinkCursor),
        @"cursorShape": prefs.cursorShapeName,
    } mutableCopy];
    if (prefs.palette.colorPaletteOverrides) {
        themeInfo[@"colorPaletteOverrides"] = palette.colorPaletteOverrides;
    }
    NSString *json = [[NSString alloc] initWithData:[NSJSONSerialization dataWithJSONObject:themeInfo options:0 error:nil] encoding:NSUTF8StringEncoding];
    [self.terminal.webView evaluateJavaScript:[NSString stringWithFormat:@"exports.updateStyle(%@)", json] completionHandler:^(id result, NSError *error){
        [self updateFloatingCursorSensitivity];
    }];
}

- (void)setOverrideFontSize:(CGFloat)overrideFontSize {
    _overrideFontSize = overrideFontSize;
    [self _updateStyle];
}

- (void)setOverrideAppearance:(enum OverrideAppearance)overrideAppearance {
    _overrideAppearance = overrideAppearance;
    [self _updateStyle];
}

- (CGFloat)effectiveFontSize {
    if (self.overrideFontSize != 0)
        return self.overrideFontSize;
    return UserPreferences.shared.fontSize.doubleValue;
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
        if (!self.isFirstResponder) {
            [self becomeFirstResponder];
        }
    } else if ([message.name isEqualToString:@"newScrollRange"]) {
        self.scrollRange = [message.body doubleValue];
        [self applyScrollRange];
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
    } else if ([message.name isEqualToString:@"openLink"]) {
        [UIApplication openURL:message.body];
    }
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
        NSArray<NSNumber *> *rect = selection[@"rect"];
        self.selectionRect = [self convertRect:CGRectMake(rect[0].doubleValue, rect[1].doubleValue,
                                                          rect[2].doubleValue, rect[3].doubleValue)
                                      fromView:webView];
        CGPoint top = CGPointMake(CGRectGetMidX(self.selectionRect), CGRectGetMinY(self.selectionRect));
        if (@available(iOS 16.0, *)) {
            if (self.editMenu == nil) {
                self.editMenu = [[UIEditMenuInteraction alloc] initWithDelegate:self];
                [self addInteraction:self.editMenu];
            }
            [self.editMenu presentEditMenuWithConfiguration:[UIEditMenuConfiguration configurationWithIdentifier:nil sourcePoint:top]];
        } else {
            // UIMenuController's actions are the first responder's
            [self becomeFirstResponder];
            [UIMenuController.sharedMenuController showMenuFromView:self rect:self.selectionRect];
        }
    }];
}

- (UIMenu *)editMenuInteraction:(UIEditMenuInteraction *)interaction menuForConfiguration:(UIEditMenuConfiguration *)configuration suggestedActions:(NSArray<UIMenuElement *> *)suggestedActions API_AVAILABLE(ios(16.0)) {
    // Its own actions, not the first responder's: the keyboard need not be up
    __weak typeof(self) weakSelf = self;
    return [UIMenu menuWithChildren:@[
        [UIAction actionWithTitle:@"Copy" image:[UIImage systemImageNamed:@"doc.on.doc"] identifier:nil handler:^(UIAction *action) {
            [weakSelf copy:nil];
        }],
    ]];
}

- (CGRect)editMenuInteraction:(UIEditMenuInteraction *)interaction targetRectForConfiguration:(UIEditMenuConfiguration *)configuration API_AVAILABLE(ios(16.0)) {
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

- (void)insertControlChar:(char)ch {
    if (strchr(controlKeys, ch) != NULL) {
        if (ch == ' ') ch = '\0';
        if (ch == '2') ch = '@';
        if (ch == '6') ch = '^';
        if (ch != '\0')
            ch = toupper(ch) ^ 0x40;
        [self.terminal sendInput:[NSData dataWithBytes:&ch length:1]];
    }
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

- (UITextSmartDashesType)smartDashesType API_AVAILABLE(ios(11)) {
    return UITextSmartDashesTypeNo;
}
- (UITextSmartQuotesType)smartQuotesType API_AVAILABLE(ios(11)) {
    return UITextSmartQuotesTypeNo;
}
- (UITextSmartInsertDeleteType)smartInsertDeleteType API_AVAILABLE(ios(11)) {
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
        if ([key isEqualToString:@"2"])
            key = @"@";
        else if ([key isEqualToString:@"6"])
            key = @"^";
        else if ([key isEqualToString:@"-"])
            key = @"_";
        [self insertControlChar:[key characterAtIndex:0]];
    }
}

static const char *alphabet = "abcdefghijklmnopqrstuvwxyz";
static const char *controlKeys = "abcdefghijklmnopqrstuvwxyz@^26-=[]\\ ";
static const char *metaKeys = "abcdefghijklmnopqrstuvwxyz0123456789-=[]\\;',./";

- (NSArray<UIKeyCommand *> *)keyCommands {
    if (_keyCommands != nil)
        return _keyCommands;
    _keyCommands = [NSMutableArray new];
    [self addKeys:controlKeys withModifiers:UIKeyModifierControl];
    for (NSString *specialKey in @[UIKeyInputEscape, @"\t"]) {
        [self addKey:specialKey withModifiers:0];
    }
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
    if (UserPreferences.shared.capsLockMapping != CapsLockMapNone) {
        if (@available(iOS 13, *)); else {
            [self addKeys:controlKeys withModifiers:UIKeyModifierAlphaShift];
            [self addKeys:alphabet withModifiers:0];
            [self addKeys:alphabet withModifiers:UIKeyModifierShift];
            [self addKey:@"" withModifiers:UIKeyModifierAlphaShift]; // otherwise tap of caps lock can switch layouts
        }
    }
    if (UserPreferences.shared.optionMapping == OptionMapEsc) {
        [self addKeys:metaKeys withModifiers:UIKeyModifierAlternate];
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
    if (@available(iOS 15, *)) {
        command.wantsPriorityOverSystemBehavior = YES;
    }
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
    if (@available(iOS 13.4, *)) {
        UIKey *key = presses.anyObject.key;
        if (UserPreferences.shared.overrideControlSpace &&
            key.keyCode == UIKeyboardHIDUsageKeyboardSpacebar &&
            key.modifierFlags & UIKeyModifierControl) {
            return [self insertControlChar:' '];
        }
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
- (CGRect)caretRectForPosition:(nonnull UITextPosition *)position { LogStub(); return CGRectZero; }
- (nullable UITextRange *)characterRangeAtPoint:(CGPoint)point { LogStub(); return nil; }
- (nullable UITextRange *)characterRangeByExtendingPosition:(nonnull UITextPosition *)position inDirection:(UITextLayoutDirection)direction { LogStub(); return nil; }
- (nullable UITextPosition *)closestPositionToPoint:(CGPoint)point { LogStub(); return nil; }
- (nullable UITextPosition *)closestPositionToPoint:(CGPoint)point withinRange:(nonnull UITextRange *)range { LogStub(); return nil; }
- (NSComparisonResult)comparePosition:(nonnull UITextPosition *)position toPosition:(nonnull UITextPosition *)other { LogStub(); return NSOrderedSame; }
- (UITextPosition *)endOfDocument { LogStub(); return nil; }
- (CGRect)firstRectForRange:(nonnull UITextRange *)range { LogStub(); return CGRectZero; }
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
