//
//  TabBarView.h
//  iSH
//
//  A strip of shell tabs shown above the terminal: capsules as wide as what
//  they show, the selected one on a fill that slides to it. A tab shows what
//  runs in it and where (an icon for the program, its title), and what it did
//  while not looked at: a dot for output, a check for a job that ended. A tab
//  has no close button, so that a tap cannot end what runs in it: it closes
//  from its menu (a long press), with Cmd-W, or when its shell exits. One that
//  exited at once restarts with its arrow. Dragging a tab moves it along the
//  strip.
//

#import <UIKit/UIKit.h>
#import "TerminalSession.h"

NS_ASSUME_NONNULL_BEGIN

@class TabBarView;

@protocol TabBarViewDelegate <NSObject>
- (void)tabBar:(TabBarView *)tabBar didSelectTabAtIndex:(NSUInteger)index;
- (void)tabBar:(TabBarView *)tabBar didRequestCloseTabAtIndex:(NSUInteger)index;
- (void)tabBar:(TabBarView *)tabBar didRequestCloseOtherTabsAtIndex:(NSUInteger)index;
- (void)tabBar:(TabBarView *)tabBar didRequestRenameTabAtIndex:(NSUInteger)index;
- (void)tabBarDidRequestNewTab:(TabBarView *)tabBar;
// A new tab whose shell starts in this one's directory
- (void)tabBar:(TabBarView *)tabBar didRequestDuplicateTabAtIndex:(NSUInteger)index;
// A fresh shell in place of this one, which exited
- (void)tabBar:(TabBarView *)tabBar didRequestRestartTabAtIndex:(NSUInteger)index;
// Dragged: the tab at from is to be at to, the others keeping their order
- (void)tabBar:(TabBarView *)tabBar didMoveTabAtIndex:(NSUInteger)from toIndex:(NSUInteger)to;
// The menu behind the strip's ⋯ button; asked for each time it opens.
- (nullable UIMenu *)commandsMenuForTabBar:(TabBarView *)tabBar;
@end

@interface TabBarView : UIView

@property (weak, nullable) id <TabBarViewDelegate> delegate;

// Updates the strip in place. Cheap enough to call on every change; tab counts are small.
- (void)setSessions:(NSArray<TerminalSession *> *)sessions selectedIndex:(NSUInteger)selectedIndex;

// Colors follow the terminal theme: background is the terminal background.
- (void)setBackgroundColor:(UIColor *)background foregroundColor:(UIColor *)foreground;

// A red dot on the ⋯ button, for something in its menu that wants attention.
@property (nonatomic) BOOL showsCommandsBadge;

@end

NS_ASSUME_NONNULL_END
