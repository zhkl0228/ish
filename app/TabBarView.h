//
//  TabBarView.h
//  iSH
//
//  A strip of shell tabs shown above the terminal, in the style of Hyper's tab bar:
//  tabs share the width and the selected tab is the terminal color with no seam. A tab
//  has no close button, so that a tap cannot end what runs in it: it closes from its
//  menu (a long press), with Cmd-W, or when its shell exits.
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
