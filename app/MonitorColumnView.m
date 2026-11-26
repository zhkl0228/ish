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
        UIBezierPath *line = [UIBezierPath bezierPath];
        for (NSUInteger i = 0; i < count; i++) {
            // half the line's width clear of the top and the bottom
            CGPoint point = CGPointMake(width - (count - 1 - i) * step,
                                        0.5 + (height - 1) * (1 - self.values[i].doubleValue));
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

@implementation MonitorTileView

- (instancetype)initWithName:(NSString *)name lines:(NSInteger)lines fontSize:(CGFloat)fontSize {
    if (self = [super initWithFrame:CGRectZero]) {
        _valueLabel = [UILabel new];
        _valueLabel.font = [UIFont monospacedDigitSystemFontOfSize:fontSize weight:UIFontWeightBold];
        _valueLabel.textAlignment = NSTextAlignmentCenter;
        _valueLabel.numberOfLines = lines;
        _valueLabel.adjustsFontSizeToFitWidth = YES;
        _valueLabel.minimumScaleFactor = 0.6;
        // a line of a dash for each line of the value, till the first reading
        NSMutableArray<NSString *> *dashes = [NSMutableArray new];
        for (NSInteger i = 0; i < lines; i++)
            [dashes addObject:@"–"];
        _valueLabel.text = [dashes componentsJoinedByString:@"\n"];
        _nameLabel = [UILabel new];
        _nameLabel.font = [UIFont systemFontOfSize:8 weight:UIFontWeightSemibold];
        _nameLabel.textAlignment = NSTextAlignmentCenter;
        _nameLabel.text = name;
        UIStackView *stack = [[UIStackView alloc] initWithArrangedSubviews:@[_valueLabel, _nameLabel]];
        stack.axis = UILayoutConstraintAxisVertical;
        stack.spacing = 1;
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        [self addSubview:stack];
        [NSLayoutConstraint activateConstraints:@[
            [stack.topAnchor constraintEqualToAnchor:self.topAnchor],
            [stack.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [stack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [stack.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        ]];
    }
    return self;
}

@end

// Bytes a second in four characters, so that the down's line and the up's are
// as wide as each other: zeros before a whole number (005B, 512B, 095K, 830K,
// 075M), a tenth under ten of a unit (9.5K, 1.2M). 1024 a step, as top's; one
// that would round to 1000 is the next unit's (1.0M, not 1000K).
static NSString *Rate(double bytes) {
    NSArray<NSString *> *units = @[@"B", @"K", @"M", @"G"];
    double value = bytes;
    NSUInteger unit = 0;
    while (round(value) >= 1000 && unit + 1 < units.count) {
        value /= 1024;
        unit++;
    }
    if (unit > 0 && value < 9.95)
        return [NSString stringWithFormat:@"%.1f%@", value, units[unit]];
    return [NSString stringWithFormat:@"%03.0f%@", value, units[unit]];
}

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
@property MonitorTileView *networkTile;
@property MonitorTileView *processTile;
@property UIStackView *heatRow;
@property UIImageView *heatIcon;
@property UILabel *heatLabel;
// The last reading's heat more than nominal: the heat's row shown, the
// keyboard down
@property BOOL hot;

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

        // The CPU's last minute over its gauge, first under the status bar. The
        // line gives way first where the column has not the room (the keyboard
        // up under it), to nothing; the clip keeps the rest off the keys.
        MonitorSparkView *line = [MonitorSparkView new];
        line.translatesAutoresizingMaskIntoConstraints = NO;
        NSLayoutConstraint *lineHeight = [line.heightAnchor constraintEqualToConstant:14];
        lineHeight.priority = UILayoutPriorityDefaultLow;
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
        [stack setCustomSpacing:10 afterView:self.nativeGauge];
        // down over up, each in a gauge's width: one line of both did not fit
        self.networkTile = [self addTile:NSLocalizedString(@"NET", @"The network's name, under its rates: a few letters")
                                   lines:2 fontSize:11];
        [stack setCustomSpacing:8 afterView:self.networkTile];
        self.processTile = [self addTile:NSLocalizedString(@"PROC", @"The processes' name, under their count: a few letters")
                                   lines:1 fontSize:13];
        [stack setCustomSpacing:8 afterView:self.processTile];

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
        // middle as the status bar is; as tall as the meters are, which the
        // column's constraints keep under the status bar and above the keys
        // (TerminalViewController)
        [NSLayoutConstraint activateConstraints:@[
            [stack.topAnchor constraintEqualToAnchor:self.topAnchor],
            [stack.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [stack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [stack.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-2 * kStatusInset],
        ]];
        [self applyColors];
    }
    return self;
}

- (void)setKeyboardUp:(BOOL)keyboardUp {
    _keyboardUp = keyboardUp;
    [self showRows];
}

// What is under the memory, hidden while the keyboard is up; the heat's row
// shown only while it is more than nominal
- (void)showRows {
    for (UIView *row in @[self.nativeGauge, self.networkTile, self.processTile]) {
        if (row.hidden != self.keyboardUp)
            row.hidden = self.keyboardUp;
    }
    BOOL heat = self.hot && !self.keyboardUp;
    if (self.heatRow.hidden == heat)
        self.heatRow.hidden = !heat;
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

- (MonitorTileView *)addTile:(NSString *)name lines:(NSInteger)lines fontSize:(CGFloat)fontSize {
    MonitorTileView *tile = [[MonitorTileView alloc] initWithName:name lines:lines fontSize:fontSize];
    tile.translatesAutoresizingMaskIntoConstraints = NO;
    [tile.widthAnchor constraintEqualToConstant:kGaugeSize].active = YES;
    [self.stack addArrangedSubview:tile];
    return tile;
}

- (CGRect)metersFrame {
    return self.stack.frame;
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
    // green however full: more of it native is better, not a load
    self.nativeGauge.color = UIColor.systemGreenColor;
    for (MonitorGaugeView *gauge in @[self.cpuGauge, self.memoryGauge, self.nativeGauge]) {
        gauge.trackColor = track;
        gauge.valueLabel.textColor = foreground;
        gauge.nameLabel.textColor = [foreground colorWithAlphaComponent:0.6];
    }
    for (MonitorTileView *tile in @[self.networkTile, self.processTile]) {
        tile.valueLabel.textColor = foreground;
        tile.nameLabel.textColor = [foreground colorWithAlphaComponent:0.6];
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
    // the native share, the network and the processes as they were till
    // known, counted again
    if (reading.nativeKnown) {
        self.nativeGauge.valueLabel.text = [NSString stringWithFormat:@"%d", (int) round(reading.nativeShare * 100)];
        self.nativeGauge.fraction = reading.nativeShare;
    }
    if (reading.networkCounted)
        self.networkTile.valueLabel.text = [NSString stringWithFormat:@"↓%@\n↑%@", Rate(reading.networkIn),
                                            Rate(reading.networkOut)];
    self.processTile.valueLabel.text = [NSString stringWithFormat:@"%lld", (long long) reading.processes];
    self.hot = reading.thermalState != NSProcessInfoThermalStateNominal;
    self.heatLabel.text = MonitorThermalName(reading.thermalState);
    [self showRows];
    [UIView animateWithDuration:0.3 animations:^{
        [self applyColors];
    }];

    NSString *native = reading.nativeKnown
        ? [NSString stringWithFormat:@"%d%%", (int) round(reading.nativeShare * 100)]
        : NSLocalizedString(@"unknown", @"The JIT gauge read aloud before any code was compiled");
    NSString *down = [NSByteCountFormatter stringFromByteCount:(long long) reading.networkIn
                                                    countStyle:NSByteCountFormatterCountStyleMemory];
    NSString *up = [NSByteCountFormatter stringFromByteCount:(long long) reading.networkOut
                                                  countStyle:NSByteCountFormatterCountStyleMemory];
    self.accessibilityLabel = [NSString stringWithFormat:NSLocalizedString(
        @"%1$@, JIT %2$@, network down %3$@ a second, up %4$@ a second, %5$lld processes",
        @"The column's meters read aloud: the strip's, then the JIT's share, the network, the processes"),
        MonitorSpokenReading(reading), native, down, up, (long long) reading.processes];
}

@end
