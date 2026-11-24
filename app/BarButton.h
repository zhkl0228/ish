//
//  AccessoryButton.h
//  iSH
//
//  Created by Theodore Dubois on 9/22/18.
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface BarButton : UIButton

// Its colors, of the terminal's theme (TerminalViewController): at rest,
// pressed or ctrl toggled on, and of its name or glyph
- (void)setKeyColor:(UIColor *)key pressedColor:(UIColor *)pressed textColor:(UIColor *)text;
@property (readonly) UIColor *keyColor;
@property IBInspectable BOOL toggleable;

@end

// The symbols a key of the bar holds, shown over it while it is held: the
// finger picks one by moving onto it and letting go, as with the keyboard's
// held letters. Rows of symbols, the first nearest the key.
@interface KeyPopupView : UIView

- (instancetype)initWithRows:(NSArray<NSArray<NSString *> *> *)rows;
// Over key, in its window, within the width of area (the row of keys)
- (void)showOverKey:(BarButton *)key within:(UIView *)area;
// The finger, in the window. Nothing is chosen until it moves from where it
// was when the popup showed; then the symbol under it (the nearest row's
// while it is still on the key), none once it has gone away.
- (void)track:(CGPoint)point;
@property (readonly) BOOL moved;
@property (readonly, nullable) NSString *chosen;

@end

NS_ASSUME_NONNULL_END
