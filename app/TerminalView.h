//
//  TerminalView.h
//  iSH
//
//  Created by Theodore Dubois on 11/3/17.
//

#import <UIKit/UIKit.h>
#import "Terminal.h"

enum OverrideAppearance {
    OverrideAppearanceNone,
    OverrideAppearanceLight,
    OverrideAppearanceDark,
};

@interface TerminalView : UIView <UITextInput, WKScriptMessageHandler, UIScrollViewDelegate>

@property IBInspectable (nonatomic) BOOL canBecomeFirstResponder;

// The font size, the one in Settings (UserPreferences' fontSize): a pinch on
// the terminal, ⌘+ and ⌘- change it, within the sizes it can be, and the
// terminal shows the size and the columns and rows it makes
- (void)changeFontSize:(int)size;
// Puts the keyboard away (the bar's Hide Keyboard key, the View menu's)
- (IBAction)loseFocus:(nullable id)sender;
@property (nonatomic) enum OverrideAppearance overrideAppearance;

@property (nonatomic) UIKeyboardAppearance keyboardAppearance;

// A view in what the terminal leaves right of its last column (its meters:
// TerminalViewController's MonitorStripView), from there to this view's right
// edge and all its height: over the terminal, which its touches are to go
// through to. The page leaves the room the fit keeps for xterm.js's scroll bar
// and what is less than a column, 18 to 18 + a column's width (term.js's
// screenEdge). Hidden until the page says where its cells end.
@property (nonatomic, nullable) UIView *marginView;
// A tap on the margin view, which the page does not have (a drag from there
// still scrolls, a long press still selects). Set with marginView.
@property (copy, nullable) void (^marginTapped)(void);

@property (weak) IBOutlet UIInputView *inputAccessoryView;
@property (weak) IBOutlet UIButton *controlKey;

// The bar's ctrl key as a modifier for the next key, as insertText: applies
// it: held down it counts, toggled on it counts once and is cleared.
- (UIKeyModifierFlags)takeBarModifiers;
// Sends an already encoded key sequence (an arrow with its modifiers) without
// applying the bar's ctrl key again.
- (void)insertSequence:(nonnull NSString *)sequence;

@property (nonatomic, nullable) Terminal *terminal;

// Consulted by paste: before it pastes the system clipboard; returns YES if it
// pasted something itself (the clipboard manager's Paste Stack).
@property (copy, nullable) BOOL (^pasteInterceptor)(void);

// Find in what the terminal holds: the matches highlighted, the next one (or
// the previous one) selected and scrolled to; as the text is typed
// (incremental), the one found stays while it still matches. clearFind takes
// the highlights and the selection away.
- (void)find:(nonnull NSString *)text backwards:(BOOL)backwards incremental:(BOOL)incremental;
- (void)clearFind;
// Which match is found (-1 for none) of how many, after a find and again as
// output changes them
@property (copy, nullable) void (^findResults)(NSInteger index, NSInteger count);

// Screen housekeeping, for the menu commands.
- (void)clearScreen; // the screen but for the line being typed (term.js)
- (void)clearScrollback;
- (void)resetTerminal; // full VT reset, then the theme is reapplied
// Everything in the scrollback and on screen, as text.
- (void)fetchTextWithCompletion:(nonnull void (^)(NSString *_Nonnull text))completion;

@end
