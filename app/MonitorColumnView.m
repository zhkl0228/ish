//
//  MonitorColumnView.m
//  iSH
//

#import "MonitorColumnView.h"

@implementation MonitorSparkView {
    CAShapeLayer *_line;
    CAShapeLayer *_area;
}

- (instancetype)initWithFrame:(CGRect)frame {
    if (self = [super initWithFrame:frame]) {
        _values = @[];
        _leastTop = 1;
        _area = [CAShapeLayer layer];
        _line = [CAShapeLayer layer];
        _line.fillColor = NULL;
        _line.lineWidth = 1;
        _line.lineJoin = kCALineJoinRound;
        [self.layer addSublayer:_area];
        [self.layer addSublayer:_line];
    }
    return self;
}

- (void)setValues:(NSArray<NSNumber *> *)values {
    _values = values;
    [self setNeedsLayout];
}

- (void)setLineColor:(UIColor *)lineColor {
    _lineColor = lineColor;
    _line.strokeColor = lineColor.CGColor;
}

- (void)setAreaColor:(UIColor *)areaColor {
    _areaColor = areaColor;
    _area.fillColor = areaColor.CGColor;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    // At once: the paths have as many points as readings, which the layers'
    // own animation would morph between
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    CGRect bounds = self.bounds;
    _line.frame = _area.frame = bounds;
    NSUInteger count = self.values.count;
    if (count < 2) {
        _line.path = _area.path = NULL;
    } else {
        CGFloat width = bounds.size.width, height = bounds.size.height;
        CGFloat step = width / (kMonitorHistoryLength - 1);
        double top = self.leastTop;
        for (NSNumber *value in self.values)
            top = MAX(top, value.doubleValue);
        UIBezierPath *line = [UIBezierPath bezierPath];
        for (NSUInteger i = 0; i < count; i++) {
            // half the line's width clear of the top and the bottom
            CGPoint point = CGPointMake(width - (count - 1 - i) * step,
                                        0.5 + (height - 1) * (1 - self.values[i].doubleValue / top));
            if (i == 0)
                [line moveToPoint:point];
            else
                [line addLineToPoint:point];
        }
        UIBezierPath *area = [line copy];
        [area addLineToPoint:CGPointMake(width, height)];
        [area addLineToPoint:CGPointMake(width - (count - 1) * step, height)];
        [area closePath];
        _line.path = line.CGPath;
        _area.path = area.CGPath;
    }
    [CATransaction commit];
}

@end

static const CGFloat kGaugeSize = 44;
static const CGFloat kGaugeLine = 4;
// How far left of the column's middle the status bar's battery is: the gauges
// centered in the column were 5.7 points right of it folded and 5.8 unfolded
// (the phone's screenshots, gauges and battery side by side): the screen's
// rounded edge keeps the status bar off its right
static const CGFloat kStatusInset = 6;

@implementation MonitorGaugeView {
    CAShapeLayer *_track;
    CAShapeLayer *_arc;
}

- (instancetype)initWithFrame:(CGRect)frame {
    if (self = [super initWithFrame:frame]) {
        _track = [CAShapeLayer layer];
        _arc = [CAShapeLayer layer];
        for (CAShapeLayer *layer in @[_track, _arc]) {
            layer.fillColor = NULL;
            layer.lineWidth = kGaugeLine;
            layer.lineCap = kCALineCapRound;
            [self.layer addSublayer:layer];
        }
        _arc.strokeEnd = 0;

        _valueLabel = [UILabel new];
        _valueLabel.font = [UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightBold];
        _valueLabel.textAlignment = NSTextAlignmentCenter;
        _valueLabel.adjustsFontSizeToFitWidth = YES;
        _valueLabel.minimumScaleFactor = 0.6;
        _valueLabel.text = @"–";
        [self addSubview:_valueLabel];
        _nameLabel = [UILabel new];
        _nameLabel.font = [UIFont systemFontOfSize:8 weight:UIFontWeightSemibold];
        _nameLabel.textAlignment = NSTextAlignmentCenter;
        [self addSubview:_nameLabel];
    }
    return self;
}

- (void)setFraction:(double)fraction {
    _fraction = fraction;
    _arc.strokeEnd = MIN(MAX(fraction, 0), 1);
}

- (void)setColor:(UIColor *)color {
    _color = color;
    _arc.strokeColor = color.CGColor;
}

- (void)setTrackColor:(UIColor *)trackColor {
    _trackColor = trackColor;
    _track.strokeColor = trackColor.CGColor;
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGRect bounds = self.bounds;
    CGFloat side = MIN(bounds.size.width, bounds.size.height);
    CGPoint center = CGPointMake(CGRectGetMidX(bounds), CGRectGetMidY(bounds));
    // From the bottom left (135 degrees, y down) clockwise over the top to the
    // bottom right (45): the opening is the bottom's quarter
    UIBezierPath *ring = [UIBezierPath bezierPathWithArcCenter:center radius:(side - kGaugeLine) / 2
                                                    startAngle:M_PI * 3 / 4 endAngle:M_PI * 9 / 4 clockwise:YES];
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    _track.frame = _arc.frame = bounds;
    _track.path = _arc.path = ring.CGPath;
    [CATransaction commit];
    CGFloat inset = kGaugeLine + 3;
    self.valueLabel.frame = CGRectMake(inset, center.y - 9, bounds.size.width - 2 * inset, 18);
    self.nameLabel.frame = CGRectMake(0, CGRectGetMaxY(bounds) - 10, bounds.size.width, 10);
}

@end

// A gauge's color: green, then as the memory's (orange past 75%, red past 90%)
static UIColor *GaugeColor(double fraction) {
    return MonitorMemoryColor(fraction, UIColor.systemGreenColor);
}

@interface MonitorColumnView ()

@property UIStackView *stack;
@property MonitorGaugeView *cpuGauge;
@property MonitorSparkView *cpuLine;
@property MonitorGaugeView *memoryGauge;
@property MonitorGaugeView *nativeGauge;
@property UIStackView *heatRow;
@property UIImageView *heatIcon;
@property UILabel *heatLabel;
// The last reading's heat more than nominal: the heat's row shown, the
// keyboard down, where it fits (fitRows)
@property (nonatomic) BOOL hot;

@end

@implementation MonitorColumnView

- (instancetype)initWithSampler:(MonitorSampler *)sampler {
    if (self = [super initWithSampler:sampler]) {
        self.clipsToBounds = YES;
        self.isAccessibilityElement = YES;
        self.accessibilityTraits = UIAccessibilityTraitButton | UIAccessibilityTraitUpdatesFrequently;
        self.accessibilityHint = MonitorDetailsHint();
        [self addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tapped:)]];

        UIStackView *stack = [UIStackView new];
        stack.axis = UILayoutConstraintAxisVertical;
        stack.alignment = UIStackViewAlignmentCenter;
        stack.spacing = 4;
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        [self addSubview:stack];
        self.stack = stack;

        // The CPU's last minute over its gauge, first under the status bar, its
        // top a CPU's whole share at least (the guest's one thread busy) or
        // the minute's highest: of all the CPUs, a busy thread's share a sixth
        // of six, the line was a flat one at the bottom of its 14 points. 999:
        // hidden (fitRows), the stack's own height of 0 for it holds.
        MonitorSparkView *line = [MonitorSparkView new];
        line.leastTop = 1.0 / NSProcessInfo.processInfo.activeProcessorCount;
        line.translatesAutoresizingMaskIntoConstraints = NO;
        NSLayoutConstraint *lineHeight = [line.heightAnchor constraintEqualToConstant:14];
        lineHeight.priority = UILayoutPriorityRequired - 1;
        [NSLayoutConstraint activateConstraints:@[
            [line.widthAnchor constraintEqualToConstant:kGaugeSize],
            lineHeight,
        ]];
        [stack addArrangedSubview:line];
        [stack setCustomSpacing:6 afterView:line];
        self.cpuLine = line;
        self.cpuGauge = [self addGauge:@"CPU"];
        [stack setCustomSpacing:10 afterView:self.cpuGauge];
        self.memoryGauge = [self addGauge:NSLocalizedString(@"MEM", @"The memory gauge's name, under it: a few letters")];
        [stack setCustomSpacing:10 afterView:self.memoryGauge];
        self.nativeGauge = [self addGauge:@"JIT"];
        [stack setCustomSpacing:8 afterView:self.nativeGauge];

        // The heat while more than nominal: a thermometer and its name
        UIImageSymbolConfiguration *config = [UIImageSymbolConfiguration configurationWithPointSize:9 weight:UIImageSymbolWeightSemibold];
        UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:@"thermometer.medium" withConfiguration:config]];
        icon.contentMode = UIViewContentModeScaleAspectFit;
        UILabel *heat = [UILabel new];
        heat.font = [UIFont systemFontOfSize:10 weight:UIFontWeightSemibold];
        heat.adjustsFontSizeToFitWidth = YES;
        heat.minimumScaleFactor = 0.7;
        UIStackView *heatRow = [[UIStackView alloc] initWithArrangedSubviews:@[icon, heat]];
        heatRow.axis = UILayoutConstraintAxisHorizontal;
        heatRow.alignment = UIStackViewAlignmentCenter;
        heatRow.spacing = 2;
        heatRow.hidden = YES;
        [stack addArrangedSubview:heatRow];
        self.heatRow = heatRow;
        self.heatIcon = icon;
        self.heatLabel = heat;

        // Under the status bar's battery, kStatusInset left of the column's
        // middle as the status bar is; from the column's top, as tall as the
        // rows that fit in it (fitRows), the column's height being the room
        // the status bar and the keys leave (TerminalViewController)
        [NSLayoutConstraint activateConstraints:@[
            [stack.topAnchor constraintEqualToAnchor:self.topAnchor],
            [stack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [stack.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-2 * kStatusInset],
        ]];
        [self applyColors];
    }
    return self;
}

- (void)setKeyboardUp:(BOOL)keyboardUp {
    if (keyboardUp == _keyboardUp)
        return;
    _keyboardUp = keyboardUp;
    [self setNeedsLayout];
}

- (void)setHot:(BOOL)hot {
    if (hot == _hot)
        return;
    _hot = hot;
    [self setNeedsLayout];
}

// The rows chosen for the room the column has, before the stack is laid out
// in it: its height is the room, whatever the rows (TerminalViewController),
// so that choosing them lays out nothing again
- (void)layoutSubviews {
    [self fitRows];
    [super layoutSubviews];
}

// The rows the column shows: the CPU; the memory; under them the heat's
// while it is more than nominal and the JIT's, with the keyboard down; over
// them the CPU's last minute: each, in that order, while the column has the
// room for it and those before it. One without goes whole: squashed (the
// stack as tall as the column), the line was flat, the heat's row a dot,
// and the memory's ring, the keyboard up, half its size under its name.
- (void)fitRows {
    CGFloat room = self.bounds.size.height;
    NSMutableSet<UIView *> *shown = [NSMutableSet setWithObject:self.cpuGauge];
    NSMutableArray<UIView *> *wanted = [NSMutableArray arrayWithObject:self.memoryGauge];
    if (self.hot && !self.keyboardUp)
        [wanted addObject:self.heatRow];
    if (!self.keyboardUp)
        [wanted addObject:self.nativeGauge];
    [wanted addObject:self.cpuLine];
    for (UIView *row in wanted) {
        [shown addObject:row];
        if ([self heightOfRows:shown] > room)
            [shown removeObject:row];
    }
    for (UIView *row in @[self.cpuLine, self.memoryGauge, self.nativeGauge, self.heatRow]) {
        BOOL hidden = ![shown containsObject:row];
        if (row.hidden != hidden)
            row.hidden = hidden;
    }
}

- (CGFloat)leastHeight {
    return [self heightOfRows:[NSSet setWithObjects:self.cpuGauge, self.memoryGauge, nil]];
}

// The stack's height with these rows of it shown: theirs, and the spacing
// after each but the last
- (CGFloat)heightOfRows:(NSSet<UIView *> *)rows {
    CGFloat height = 0;
    UIView *previous = nil;
    for (UIView *row in self.stack.arrangedSubviews) {
        if (![rows containsObject:row])
            continue;
        if (previous != nil) {
            CGFloat spacing = [self.stack customSpacingAfterView:previous];
            height += spacing == UIStackViewSpacingUseDefault ? self.stack.spacing : spacing;
        }
        height += [row systemLayoutSizeFittingSize:UILayoutFittingCompressedSize].height;
        previous = row;
    }
    return height;
}

- (MonitorGaugeView *)addGauge:(NSString *)name {
    MonitorGaugeView *gauge = [MonitorGaugeView new];
    gauge.nameLabel.text = name;
    gauge.translatesAutoresizingMaskIntoConstraints = NO;
    [NSLayoutConstraint activateConstraints:@[
        [gauge.widthAnchor constraintEqualToConstant:kGaugeSize],
        [gauge.heightAnchor constraintEqualToConstant:kGaugeSize],
    ]];
    [self.stack addArrangedSubview:gauge];
    return gauge;
}

- (CGRect)metersFrame {
    return self.stack.frame;
}

- (NSLayoutXAxisAnchor *)metersCenterXAnchor {
    return self.stack.centerXAnchor;
}

- (void)tapped:(UITapGestureRecognizer *)tap {
    self.activated();
}

// As the last reading has them (none before the first: empty, nominal)
- (void)applyColors {
    MonitorReading *reading = self.sampler.reading;
    UIColor *foreground = self.foregroundColor;
    UIColor *track = [foreground colorWithAlphaComponent:0.15];
    UIColor *cpu = GaugeColor(reading.cpuBusy);
    self.cpuGauge.color = cpu;
    self.memoryGauge.color = GaugeColor(reading.memoryFraction);
    // Green however full: more of it native is better, not a load. Red while
    // the JIT's code region is full, till its flush (a moment): what is
    // compiled then runs as gadgets, unless an AOT image has it.
    self.nativeGauge.color = reading.codeRegionFull ? UIColor.systemRedColor : UIColor.systemGreenColor;
    for (MonitorGaugeView *gauge in @[self.cpuGauge, self.memoryGauge, self.nativeGauge]) {
        gauge.trackColor = track;
        gauge.valueLabel.textColor = foreground;
        gauge.nameLabel.textColor = [foreground colorWithAlphaComponent:0.6];
    }
    self.cpuLine.lineColor = cpu;
    self.cpuLine.areaColor = [cpu colorWithAlphaComponent:0.15];
    UIColor *heat = MonitorThermalColor(reading.thermalState, foreground);
    self.heatIcon.tintColor = heat;
    self.heatLabel.textColor = heat;
}

- (void)showReading:(MonitorReading *)reading {
    int cpu = (int) round(reading.cpuBusy * 100);
    int memory = (int) round(reading.memoryFraction * 100);
    // the CPUs as they were until counted again
    if (reading.counted) {
        self.cpuGauge.valueLabel.text = [NSString stringWithFormat:@"%d", cpu];
        self.cpuGauge.fraction = reading.cpuBusy;
    }
    self.cpuLine.values = self.sampler.cpuHistory;
    self.memoryGauge.valueLabel.text = [NSString stringWithFormat:@"%d", memory];
    self.memoryGauge.fraction = reading.memoryFraction;
    // the native share as it was till known
    if (reading.nativeKnown) {
        self.nativeGauge.valueLabel.text = [NSString stringWithFormat:@"%d", (int) round(reading.nativeShare * 100)];
        self.nativeGauge.fraction = reading.nativeShare;
    }
    self.hot = reading.thermalState != NSProcessInfoThermalStateNominal;
    self.heatLabel.text = MonitorThermalName(reading.thermalState);
    [UIView animateWithDuration:0.3 animations:^{
        [self applyColors];
    }];

    NSString *native = reading.nativeKnown
        ? [NSString stringWithFormat:@"%d%%", (int) round(reading.nativeShare * 100)]
        : NSLocalizedString(@"unknown", @"The JIT gauge read aloud before any code was compiled");
    if (reading.codeRegionFull)
        native = [NSString stringWithFormat:NSLocalizedString(@"%@, its code region full",
                                                              @"The JIT gauge read aloud once the code region is full"),
                  native];
    self.accessibilityLabel = [NSString stringWithFormat:NSLocalizedString(
        @"%1$@, JIT %2$@", @"The column's meters read aloud: the strip's, then the JIT's share"),
        MonitorSpokenReading(reading), native];
}

@end
