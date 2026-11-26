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

@end

extern struct tty_driver ios_tty_driver;
