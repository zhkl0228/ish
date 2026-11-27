//
//  ViewController.h
//  iSH
//
//  Created by Theodore Dubois on 10/17/17.
//

#import <UIKit/UIKit.h>
#import "Terminal.h"

@interface TerminalViewController : UIViewController

@property (nonatomic) Terminal *terminal;

// Opens a new shell in a new tab and selects it.
- (void)startNewSession;
// Adopts the sessions that still exist (e.g. after the window was reconnected), in
// order, selecting the given one; starts a new session if none of them exist.
- (void)reconnectSessionsFromTerminalUUIDs:(NSArray<NSUUID *> *)uuids selected:(NSUUID *)selected;
// UUIDs of this window's tabs, in order, and of the selected one (nil if none).
@property (readonly) NSArray<NSUUID *> *tabTerminalUUIDs;
// The tabs of every window, kept for the app's next start: their order, names
// and directories, and what each showed. Called as the app goes to the
// background, where it may end without warning.
+ (void)saveTabs;
// The tabs saved last, opened again here: each a new shell in the directory
// it was in, under what it showed. NO if none were saved.
- (BOOL)restoreSavedTabs;
@property (readonly) NSUUID *sessionTerminalUUID;
// Shows the tab of the session with this uuid if it is one of this window's
// (a notification of it tapped). Whether it is.
- (BOOL)showSessionWithUUID:(NSUUID *)uuid;
@property UISceneSession *sceneSession;

// What a program asks of the window (/proc/ish/ui, UIActions.m), each done at
// once, without animation: Settings over the terminal at page, pushed over
// its first page (that alone if nil), what was over the terminal gone first
- (void)showSettingsPage:(UIViewController *)page;
// Clipboard's settings over the terminal, what was over it gone first
- (void)showClipboardSettingsPage;
@property (readonly) NSUInteger tabCount;
- (void)selectTabAtIndex:(NSUInteger)index;
// Whether the terminal has the keyboard, which setting it gives or takes
@property (nonatomic) BOOL keyboardShown;
// The clipboard's shelf over the terminal, or away
- (void)showClipboard;
- (void)hideClipboard;

@end

extern struct tty_driver ios_tty_driver;
