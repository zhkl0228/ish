//
//  AccessoryButton.m
//  iSH
//
//  Created by Theodore Dubois on 9/22/18.
//

#import "BarButton.h"

@interface BarButton ()
@end

extern UIAccessibilityTraits UIAccessibilityTraitToggle;

@implementation BarButton

- (void)awakeFromNib {
    [super awakeFromNib];
    // Flat keys, square and without a shadow, a point apart in the row: the
    // terminal's background shows between them.
    self.backgroundColor = self.defaultColor;
    self.keyAppearance = UIKeyboardAppearanceLight;
    self.accessibilityTraits |= UIAccessibilityTraitKeyboardKey;
    if (self.toggleable) {
        self.accessibilityTraits |= 0x20000000000000;
    }
}

// A key at rest: dark gray (#2E2E2E) with the dark keyboard, light gray with
// the light one. Pressed, or ctrl toggled on: lighter with the dark keyboard,
// darker with the light one.
- (UIColor *)primaryColor {
    if (self.keyAppearance == UIKeyboardAppearanceLight)
        return [UIColor colorWithWhite:209/255. alpha:1];
    else
        return [UIColor colorWithWhite:46/255. alpha:1];
}
- (UIColor *)secondaryColor {
    if (self.keyAppearance == UIKeyboardAppearanceLight)
        return [UIColor colorWithWhite:160/255. alpha:1];
    else
        return [UIColor colorWithWhite:100/255. alpha:1];
}
- (UIColor *)defaultColor {
    if (self.secondary)
        return self.secondaryColor;
    return self.primaryColor;
}
- (UIColor *)highlightedColor {
    if (!self.secondary)
        return self.secondaryColor;
    return self.primaryColor;
}

- (void)chooseBackground {
    if (self.selected || self.highlighted) {
        self.backgroundColor = self.highlightedColor;
    } else {
        [UIView animateWithDuration:0 delay:0.1 options:UIViewAnimationOptionAllowUserInteraction animations:^{
            self.backgroundColor = self.defaultColor;
        } completion:nil];
    }
    if (self.keyAppearance == UIKeyboardAppearanceLight) {
        self.tintColor = UIColor.blackColor;
    } else {
        self.tintColor = UIColor.whiteColor;
    }
    [self setTitleColor:self.tintColor forState:UIControlStateNormal];
}

- (void)setHighlighted:(BOOL)highlighted {
    [super setHighlighted:highlighted];
    [self chooseBackground];
}
- (void)setSelected:(BOOL)selected {
    [super setSelected:selected];
    [self chooseBackground];
}

- (void)setKeyAppearance:(UIKeyboardAppearance)keyAppearance {
    _keyAppearance = keyAppearance;
    [self chooseBackground];
}

- (NSString *)accessibilityValue {
    if (self.toggleable) {
        return self.selected ? @"1" : @"0";
    }
    return nil;
}

@end
