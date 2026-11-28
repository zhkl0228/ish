//
//  AccessoryButton.m
//  iSH
//
//  Created by Theodore Dubois on 9/22/18.
//

#import "BarButton.h"

extern UIAccessibilityTraits UIAccessibilityTraitToggle;

@implementation BarButton {
    UIColor *_pressedColor;
    UIColor *_textColor;
}

// A key of the storyboard's, or one made in code (the symbol key)
- (void)awakeFromNib {
    [super awakeFromNib];
    [self setUpKey];
}

- (instancetype)initWithFrame:(CGRect)frame {
    if (self = [super initWithFrame:frame])
        [self setUpKey];
    return self;
}

- (void)setUpKey {
    // Flat keys without a shadow, shaped by their row (TerminalViewController:
    // the bar's rounded as the keyboard's, the column's round), the
    // terminal's background between them. Light gray until the theme's colors
    // are set.
    [self setKeyColor:[UIColor colorWithWhite:209/255. alpha:1]
         pressedColor:[UIColor colorWithWhite:160/255. alpha:1]
            textColor:UIColor.blackColor];
    self.accessibilityTraits |= UIAccessibilityTraitKeyboardKey;
    if (self.toggleable) {
        self.accessibilityTraits |= 0x20000000000000;
    }
}

- (void)setKeyColor:(UIColor *)key pressedColor:(UIColor *)pressed textColor:(UIColor *)text {
    _keyColor = key;
    _pressedColor = pressed;
    _textColor = text;
    [self chooseBackground];
}

- (void)chooseBackground {
    if (self.selected || self.highlighted) {
        self.backgroundColor = _pressedColor;
    } else {
        [UIView animateWithDuration:0 delay:0.1 options:UIViewAnimationOptionAllowUserInteraction animations:^{
            self.backgroundColor = self.keyColor;
        } completion:nil];
    }
    self.tintColor = _textColor;
    [self setTitleColor:_textColor forState:UIControlStateNormal];
}

- (void)setHighlighted:(BOOL)highlighted {
    [super setHighlighted:highlighted];
    [self chooseBackground];
}
- (void)setSelected:(BOOL)selected {
    [super setSelected:selected];
    [self chooseBackground];
}

- (NSString *)accessibilityValue {
    if (self.toggleable) {
        return self.selected ? @"1" : @"0";
    }
    return nil;
}

@end

#pragma mark - KeyPopupView

// A symbol's cell, at most; narrower on a narrow screen
static const CGFloat kPopupCellWidth = 40;
static const CGFloat kPopupCellHeight = 44;
// Around the cells, and between the popup and its key
static const CGFloat kPopupInset = 4;
static const CGFloat kPopupGap = 6;
// How far the finger goes before it chooses
static const CGFloat kPopupSlop = 8;

@implementation KeyPopupView {
    NSArray<NSArray<NSString *> *> *_rows;
    // The cells, row after row as in _rows
    NSArray<UILabel *> *_cells;
    CGFloat _cellWidth;
    // The key's frame, in the window
    CGRect _keyFrame;
    UIColor *_textColor;
    NSInteger _chosenIndex;
    // where the finger was when the popup showed
    BOOL _started;
    CGPoint _start;
}

- (instancetype)initWithRows:(NSArray<NSArray<NSString *> *> *)rows {
    if (self = [super initWithFrame:CGRectZero]) {
        _rows = rows;
        _chosenIndex = -1;
        self.userInteractionEnabled = NO;
        self.layer.cornerRadius = 8;
        self.layer.shadowColor = UIColor.blackColor.CGColor;
        self.layer.shadowOpacity = 0.3;
        self.layer.shadowRadius = 6;
        self.layer.shadowOffset = CGSizeMake(0, 2);
        NSMutableArray<UILabel *> *cells = [NSMutableArray new];
        for (NSArray<NSString *> *row in rows) {
            for (NSString *symbol in row) {
                UILabel *cell = [UILabel new];
                cell.text = symbol;
                cell.textAlignment = NSTextAlignmentCenter;
                cell.font = [UIFont monospacedSystemFontOfSize:20 weight:UIFontWeightRegular];
                cell.layer.cornerRadius = 6;
                cell.clipsToBounds = YES;
                [self addSubview:cell];
                [cells addObject:cell];
            }
        }
        _cells = cells;
    }
    return self;
}

- (void)showOverKey:(BarButton *)key within:(UIView *)area {
    UIWindow *window = key.window;
    _keyFrame = [key convertRect:key.bounds toView:window];
    CGRect room = [area convertRect:area.bounds toView:window];
    NSUInteger columns = 0;
    for (NSArray<NSString *> *row in _rows)
        columns = MAX(columns, row.count);
    _cellWidth = MIN(kPopupCellWidth, floor((room.size.width - 2 * kPopupInset) / columns));
    CGSize size = CGSizeMake(columns * _cellWidth + 2 * kPopupInset, _rows.count * kPopupCellHeight + 2 * kPopupInset);
    // its left edge at the key's, as far as the row leaves room
    CGFloat x = MAX(CGRectGetMinX(room), MIN(CGRectGetMinX(_keyFrame) - kPopupInset, CGRectGetMaxX(room) - size.width));
    self.frame = CGRectMake(x, CGRectGetMinY(_keyFrame) - kPopupGap - size.height, size.width, size.height);
    NSUInteger i = 0;
    for (NSUInteger r = 0; r < _rows.count; r++) {
        // the first row at the bottom, nearest the key
        CGFloat y = kPopupInset + (_rows.count - 1 - r) * kPopupCellHeight;
        for (NSUInteger c = 0; c < _rows[r].count; c++)
            _cells[i++].frame = CGRectMake(kPopupInset + c * _cellWidth, y, _cellWidth, kPopupCellHeight);
    }
    // the key's colors
    self.backgroundColor = key.keyColor;
    _textColor = key.tintColor;
    [self showChosen];
    [window addSubview:self];
}

- (void)track:(CGPoint)point {
    if (!_started) {
        _started = YES;
        _start = point;
        return;
    }
    if (!_moved) {
        if (hypot(point.x - _start.x, point.y - _start.y) < kPopupSlop)
            return;
        _moved = YES;
    }
    CGRect frame = self.frame;
    // Where a finger still picks: the popup and half a cell around it, and
    // below it down to the key's bottom
    CGRect reach = CGRectMake(CGRectGetMinX(frame) - _cellWidth / 2, CGRectGetMinY(frame) - kPopupCellHeight / 2,
                              CGRectGetWidth(frame) + _cellWidth,
                              CGRectGetMaxY(_keyFrame) - CGRectGetMinY(frame) + kPopupCellHeight / 2);
    NSInteger chosen = -1;
    if (CGRectContainsPoint(reach, point)) {
        NSInteger rows = (NSInteger) _rows.count;
        NSInteger fromTop = (NSInteger) floor((point.y - CGRectGetMinY(frame) - kPopupInset) / kPopupCellHeight);
        NSInteger r = rows - 1 - MAX(0, MIN(rows - 1, fromTop));
        NSInteger columns = (NSInteger) _rows[r].count;
        NSInteger c = (NSInteger) floor((point.x - CGRectGetMinX(frame) - kPopupInset) / _cellWidth);
        chosen = MAX(0, MIN(columns - 1, c));
        for (NSInteger before = 0; before < r; before++)
            chosen += _rows[before].count;
    }
    if (chosen == _chosenIndex)
        return;
    _chosenIndex = chosen;
    [self showChosen];
}

- (void)showChosen {
    [_cells enumerateObjectsUsingBlock:^(UILabel *cell, NSUInteger i, BOOL *_) {
        BOOL chosen = (NSInteger) i == self->_chosenIndex;
        cell.backgroundColor = chosen ? UIColor.systemBlueColor : UIColor.clearColor;
        cell.textColor = chosen ? UIColor.whiteColor : self->_textColor;
    }];
}

- (NSString *)chosen {
    if (_chosenIndex < 0)
        return nil;
    return _cells[_chosenIndex].text;
}

@end
