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
@property UIStackView *heatRow;
@property UIImageView *heatIcon;
@property UILabel *heatLabel;

@end

@implementation MonitorColumnView

- (instancetype)initWithSampler:(MonitorSampler *)sampler {
    if (self = [super initWithSampler:sampler]) {
        self.clipsToBounds = YES;
        self.isAccessibilityElement = YES;
        self.accessibilityTraits = UIAccessibilityTraitButton | UIAccessibilityTraitUpdatesFrequently;
        self.accessibilityHint = @"Shows the CPUs, the memory and the heat in detail.";
        [self addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tapped:)]];

        UIStackView *stack = [UIStackView new];
        stack.axis = UILayoutConstraintAxisVertical;
        stack.alignment = UIStackViewAlignmentCenter;
        stack.spacing = 4;
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        [self addSubview:stack];
        self.stack = stack;

        self.cpuGauge = [self addGauge:@"CPU"];
        // The line gives way first where the column has not the room (the
        // keyboard up under it), to nothing; the clip keeps the rest off the keys
        MonitorSparkView *line = [MonitorSparkView new];
        line.translatesAutoresizingMaskIntoConstraints = NO;
        NSLayoutConstraint *lineHeight = [line.heightAnchor constraintEqualToConstant:14];
        lineHeight.priority = UILayoutPriorityDefaultLow;
        [NSLayoutConstraint activateConstraints:@[
            [line.widthAnchor constraintEqualToConstant:kGaugeSize],
            lineHeight,
        ]];
        [stack addArrangedSubview:line];
        [stack setCustomSpacing:12 afterView:line];
        self.cpuLine = line;
        self.memoryGauge = [self addGauge:@"MEM"];
        [stack setCustomSpacing:8 afterView:self.memoryGauge];

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

        // centered in the column; as tall as the meters are, which the column's
        // constraints keep under the status bar and above the keys
        // (TerminalViewController)
        [NSLayoutConstraint activateConstraints:@[
            [stack.topAnchor constraintEqualToAnchor:self.topAnchor],
            [stack.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [stack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor],
            [stack.trailingAnchor constraintEqualToAnchor:self.trailingAnchor],
        ]];
        [self applyColors];
    }
    return self;
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
    for (MonitorGaugeView *gauge in @[self.cpuGauge, self.memoryGauge]) {
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
    BOOL hot = reading.thermalState != NSProcessInfoThermalStateNominal;
    self.heatLabel.text = MonitorThermalName(reading.thermalState);
    if (self.heatRow.hidden == hot)
        self.heatRow.hidden = !hot;
    [UIView animateWithDuration:0.3 animations:^{
        [self applyColors];
    }];

    NSString *cpuText = reading.counted ? [NSString stringWithFormat:@"CPU %d%%, ", cpu] : @"";
    self.accessibilityLabel = [NSString stringWithFormat:@"%@memory %d%%, heat %@", cpuText, memory,
                               MonitorThermalName(reading.thermalState).lowercaseString];
}

@end
