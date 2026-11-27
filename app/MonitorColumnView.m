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

// kStatusInset left of the column's middle, its width less twice that: where
// the rows are centered, and what goes under them in the column lines up with
@property UILayoutGuide *meters;
// The line, the CPU, the memory, the JIT, the heat: top to bottom
@property NSArray<UIView *> *rows;
@property MonitorGaugeView *cpuGauge;
@property MonitorSparkView *cpuLine;
@property MonitorGaugeView *memoryGauge;
@property MonitorGaugeView *nativeGauge;
@property UIStackView *heatRow;
@property UIImageView *heatIcon;
@property UILabel *heatLabel;
// The last reading's heat more than nominal: the heat's row shown, the
// keyboard down, where it fits (rowsForRoom:)
@property (nonatomic) BOOL hot;
// The JIT's ring is shown only while its code region is full and for 30 s
// after (jitShownUntil, a CACurrentMediaTime): its share of native code is the
// details' to tell, and a full region flushed every few seconds under clang
// would have it come and go
@property (nonatomic) BOOL showsJIT;
@property CFTimeInterval jitShownUntil;
// From the column's top, as tall as the rows shown (layoutSubviews):
// rowsBottomAnchor
@property UILayoutGuide *shownRows;
@property NSLayoutConstraint *shownRowsHeight;

@end

@implementation MonitorColumnView

- (instancetype)initWithSampler:(MonitorSampler *)sampler {
    if (self = [super initWithSampler:sampler]) {
        self.clipsToBounds = YES;
        self.isAccessibilityElement = YES;
        self.accessibilityTraits = UIAccessibilityTraitButton | UIAccessibilityTraitUpdatesFrequently;
        self.accessibilityHint = MonitorDetailsHint();
        [self addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tapped:)]];

        // Under the status bar's battery, kStatusInset left of the column's
        // middle as the status bar is
        UILayoutGuide *meters = [UILayoutGuide new];
        [self addLayoutGuide:meters];
        [NSLayoutConstraint activateConstraints:@[
            [meters.topAnchor constraintEqualToAnchor:self.topAnchor],
            [meters.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [meters.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [meters.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-2 * kStatusInset],
        ]];
        self.meters = meters;
        UILayoutGuide *shownRows = [UILayoutGuide new];
        [self addLayoutGuide:shownRows];
        self.shownRowsHeight = [shownRows.heightAnchor constraintEqualToConstant:0];
        [NSLayoutConstraint activateConstraints:@[
            [shownRows.topAnchor constraintEqualToAnchor:self.topAnchor],
            [shownRows.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [shownRows.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
            self.shownRowsHeight,
        ]];
        self.shownRows = shownRows;

        // The CPU's last minute over its gauge, first under the status bar, its
        // top a CPU's whole share at least (the guest's one thread busy) or
        // the minute's highest: of all the CPUs, a busy thread's share a sixth
        // of six, the line was a flat one at the bottom of its 14 points
        MonitorSparkView *line = [MonitorSparkView new];
        line.leastTop = 1.0 / NSProcessInfo.processInfo.activeProcessorCount;
        self.cpuLine = line;
        self.cpuGauge = [MonitorGaugeView new];
        self.cpuGauge.nameLabel.text = @"CPU";
        self.memoryGauge = [MonitorGaugeView new];
        self.memoryGauge.nameLabel.text = NSLocalizedString(@"MEM", @"The memory gauge's name, under it: a few letters");
        self.nativeGauge = [MonitorGaugeView new];
        self.nativeGauge.nameLabel.text = @"JIT";

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
        self.heatRow = heatRow;
        self.heatIcon = icon;
        self.heatLabel = heat;

        self.rows = @[line, self.cpuGauge, self.memoryGauge, self.nativeGauge, heatRow];
        for (UIView *row in self.rows)
            [self addSubview:row];
        [self applyColors];
    }
    return self;
}

- (void)setKeyboardUp:(BOOL)keyboardUp {
    if (keyboardUp == _keyboardUp)
        return;
    _keyboardUp = keyboardUp;
    // the rows chosen now, as the keyboard starts to move: those it hides go
    // at once (the room it leaves is the next layout's)
    [self chooseRows];
    [self setNeedsLayout];
}

- (void)setHot:(BOOL)hot {
    if (hot == _hot)
        return;
    _hot = hot;
    [self setNeedsLayout];
}

- (void)setShowsJIT:(BOOL)showsJIT {
    if (showsJIT == _showsJIT)
        return;
    _showsJIT = showsJIT;
    [self setNeedsLayout];
}

// A row's size: a gauge's, the line's 14 points under the gauges' width, the
// heat's its own, no wider than the meters
- (CGSize)sizeOfRow:(UIView *)row {
    if (row == self.cpuLine)
        return CGSizeMake(kGaugeSize, 14);
    if (row == self.heatRow) {
        CGSize size = [row systemLayoutSizeFittingSize:UILayoutFittingCompressedSize];
        return CGSizeMake(MIN(size.width, self.meters.layoutFrame.size.width), size.height);
    }
    return CGSizeMake(kGaugeSize, kGaugeSize);
}

// The room between a row and the next one shown
- (CGFloat)gapAfterRow:(UIView *)row {
    if (row == self.cpuLine)
        return 6;
    if (row == self.nativeGauge)
        return 8;
    return 10;
}

// These rows' height, top to bottom: theirs, and the gap after each but the
// last. What layoutSubviews lays them out by, so that what is said to fit,
// fits: a stack view, which they were in, kept a hidden line's spacing after
// it over the CPU, 6 points, and the heat's row, said to fit, was cut through
// its middle by the column's bottom.
- (CGFloat)heightOfRows:(NSSet<UIView *> *)shown {
    CGFloat height = 0;
    UIView *previous = nil;
    for (UIView *row in self.rows) {
        if (![shown containsObject:row])
            continue;
        if (previous != nil)
            height += [self gapAfterRow:previous];
        height += [self sizeOfRow:row].height;
        previous = row;
    }
    return height;
}

// The rows the column shows in this much room: the CPU; the memory; under
// them the heat's while it is more than nominal and the JIT's while it is
// shown (showsJIT), with the keyboard down; over them the CPU's last minute:
// each, in that order, while
// there is room for it and those before it. One without is not shown at all:
// squashed (a stack as tall as the column), the line was flat, the heat's row
// a dot, and the memory's ring, the keyboard up, half its size under its name.
- (NSSet<UIView *> *)rowsForRoom:(CGFloat)room {
    NSMutableArray<UIView *> *wanted = [NSMutableArray arrayWithObject:self.memoryGauge];
    if (self.hot && !self.keyboardUp)
        [wanted addObject:self.heatRow];
    if (self.showsJIT && !self.keyboardUp)
        [wanted addObject:self.nativeGauge];
    [wanted addObject:self.cpuLine];
    NSMutableSet<UIView *> *shown = [NSMutableSet setWithObject:self.cpuGauge];
    for (UIView *row in wanted) {
        [shown addObject:row];
        if ([self heightOfRows:shown] > room)
            [shown removeObject:row];
    }
    return shown;
}

- (CGFloat)leastHeight {
    return [self heightOfRows:[NSSet setWithObjects:self.cpuGauge, self.memoryGauge, nil]];
}

// Where a row goes among these shown: under those before it, each its height
// and the gap after it, from the column's top; centered on the meters' middle
- (CGRect)frameOfRow:(UIView *)row shown:(NSSet<UIView *> *)shown {
    CGFloat y = 0;
    for (UIView *above in self.rows) {
        if (above == row)
            break;
        if ([shown containsObject:above])
            y += [self sizeOfRow:above].height + [self gapAfterRow:above];
    }
    CGSize size = [self sizeOfRow:row];
    return CGRectMake(CGRectGetMidX(self.meters.layoutFrame) - size.width / 2, y, size.width, size.height);
}

// The rows that fit in the column's height, the room the status bar and the
// keys leave it (TerminalViewController), shown, the others hidden. Not
// animated: laid out in the keyboard's animation (TerminalViewController's
// keyboardMoved:), a stack view the rows were in animated a row it hid, shown
// till the animation ended (the JIT's went about a second after the keyboard
// came up); a row shown is put in its place at once, not moved there from
// where it last was.
- (void)chooseRows {
    NSSet<UIView *> *shown = [self rowsForRoom:self.bounds.size.height];
    [UIView performWithoutAnimation:^{
        for (UIView *row in self.rows) {
            BOOL hidden = ![shown containsObject:row];
            if (row.hidden == hidden)
                continue;
            row.hidden = hidden;
            if (!hidden)
                row.frame = [self frameOfRow:row shown:shown];
        }
    }];
}

// The rows chosen, and those shown put in their places, one under another:
// those that were shown move with the keyboard, in its animation. Their
// bottom (rowsBottomAnchor) set only as it changes: what follows it is laid
// out again, in the column's superview, which sets the column's height by
// other things than it.
- (void)layoutSubviews {
    [super layoutSubviews];
    [self chooseRows];
    NSMutableSet<UIView *> *shown = [NSMutableSet set];
    for (UIView *row in self.rows) {
        if (!row.hidden)
            [shown addObject:row];
    }
    for (UIView *row in shown)
        row.frame = [self frameOfRow:row shown:shown];
    CGFloat height = [self heightOfRows:shown];
    if (self.shownRowsHeight.constant != height)
        self.shownRowsHeight.constant = height;
}

- (CGRect)metersFrame {
    CGRect frame = CGRectNull;
    for (UIView *row in self.rows) {
        if (!row.hidden)
            frame = CGRectUnion(frame, row.frame);
    }
    return frame;
}

- (NSLayoutXAxisAnchor *)metersCenterXAnchor {
    return self.meters.centerXAnchor;
}

- (NSLayoutYAxisAnchor *)rowsBottomAnchor {
    return self.shownRows.bottomAnchor;
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
    CFTimeInterval now = CACurrentMediaTime();
    if (reading.codeRegionFull)
        self.jitShownUntil = now + 30;
    self.showsJIT = now < self.jitShownUntil;
    // its row as wide as its name
    NSString *heat = MonitorThermalName(reading.thermalState);
    if (![heat isEqualToString:self.heatLabel.text]) {
        self.heatLabel.text = heat;
        [self setNeedsLayout];
    }
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
