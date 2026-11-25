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

@interface MonitorColumnView ()

@property UIStackView *stack;
@property NSArray<UIImageView *> *icons;
@property NSArray<UILabel *> *titles;
@property UILabel *cpuLabel;
@property MeterBar *cpuBar;
@property MonitorSparkView *cpuLine;
@property UILabel *memoryLabel;
@property MeterBar *memoryBar;
@property UILabel *heatLabel;

@end

@implementation MonitorColumnView

- (instancetype)initWithSampler:(MonitorSampler *)sampler {
    if (self = [super initWithSampler:sampler]) {
        self.isAccessibilityElement = YES;
        self.accessibilityTraits = UIAccessibilityTraitButton | UIAccessibilityTraitUpdatesFrequently;
        self.accessibilityHint = @"Shows the CPUs, the memory and the heat in detail.";
        [self addGestureRecognizer:[[UITapGestureRecognizer alloc] initWithTarget:self action:@selector(tapped:)]];

        UIStackView *stack = [UIStackView new];
        stack.axis = UILayoutConstraintAxisVertical;
        stack.alignment = UIStackViewAlignmentFill;
        stack.spacing = 3;
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        [self addSubview:stack];
        self.stack = stack;

        NSMutableArray<UIImageView *> *icons = [NSMutableArray new];
        NSMutableArray<UILabel *> *titles = [NSMutableArray new];
        [self addTitle:@"CPU" symbol:@"cpu" icons:icons titles:titles];
        self.cpuLabel = [self addValue];
        self.cpuBar = [self addBar];
        MonitorSparkView *line = [MonitorSparkView new];
        line.translatesAutoresizingMaskIntoConstraints = NO;
        [line.heightAnchor constraintEqualToConstant:24].active = YES;
        [stack addArrangedSubview:line];
        self.cpuLine = line;
        [stack setCustomSpacing:14 afterView:line];

        [self addTitle:@"Memory" symbol:@"memorychip" icons:icons titles:titles];
        self.memoryLabel = [self addValue];
        self.memoryBar = [self addBar];
        [stack setCustomSpacing:14 afterView:self.memoryBar];

        [self addTitle:@"Heat" symbol:@"thermometer.medium" icons:icons titles:titles];
        UILabel *heat = [UILabel new];
        heat.font = [UIFont systemFontOfSize:12 weight:UIFontWeightSemibold];
        heat.adjustsFontSizeToFitWidth = YES;
        heat.minimumScaleFactor = 0.7;
        heat.text = MonitorThermalName(NSProcessInfoThermalStateNominal);
        [stack addArrangedSubview:heat];
        self.heatLabel = heat;
        self.icons = icons;
        self.titles = titles;

        // 8 points from the column's left, 6 from the screen's edge; as tall
        // as the meters are, which the column's constraints keep clear of the
        // status bar and the home indicator (TerminalViewController)
        [NSLayoutConstraint activateConstraints:@[
            [stack.topAnchor constraintEqualToAnchor:self.topAnchor],
            [stack.bottomAnchor constraintEqualToAnchor:self.bottomAnchor],
            [stack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:8],
            [stack.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-6],
        ]];
        [self applyColors];
    }
    return self;
}

// A meter's title: its symbol and its name, in the foreground's fainter color
- (void)addTitle:(NSString *)title symbol:(NSString *)symbol
           icons:(NSMutableArray<UIImageView *> *)icons titles:(NSMutableArray<UILabel *> *)titles {
    UIImageSymbolConfiguration *config = [UIImageSymbolConfiguration configurationWithPointSize:10 weight:UIImageSymbolWeightSemibold];
    UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:symbol withConfiguration:config]];
    icon.contentMode = UIViewContentModeScaleAspectFit;
    [icon setContentHuggingPriority:UILayoutPriorityRequired forAxis:UILayoutConstraintAxisHorizontal];
    UILabel *label = [UILabel new];
    label.font = [UIFont systemFontOfSize:10 weight:UIFontWeightSemibold];
    label.adjustsFontSizeToFitWidth = YES;
    label.minimumScaleFactor = 0.7;
    label.text = title;
    UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[icon, label]];
    row.axis = UILayoutConstraintAxisHorizontal;
    row.alignment = UIStackViewAlignmentCenter;
    row.spacing = 3;
    [self.stack addArrangedSubview:row];
    [icons addObject:icon];
    [titles addObject:label];
}

// A percent, 17-point digits, smaller where the column is narrow
- (UILabel *)addValue {
    UILabel *label = [UILabel new];
    label.font = [UIFont monospacedDigitSystemFontOfSize:17 weight:UIFontWeightSemibold];
    label.adjustsFontSizeToFitWidth = YES;
    label.minimumScaleFactor = 0.6;
    label.text = @"–";
    [self.stack addArrangedSubview:label];
    return label;
}

- (MeterBar *)addBar {
    MeterBar *bar = [MeterBar new];
    bar.horizontal = YES;
    bar.translatesAutoresizingMaskIntoConstraints = NO;
    [bar.heightAnchor constraintEqualToConstant:4].active = YES;
    [self.stack addArrangedSubview:bar];
    return bar;
}

- (CGRect)metersFrame {
    return self.stack.frame;
}

- (void)tapped:(UITapGestureRecognizer *)tap {
    self.activated();
}

// As the last reading has them (none before the first: nominal, empty)
- (void)applyColors {
    MonitorReading *reading = self.sampler.reading;
    UIColor *foreground = self.foregroundColor;
    UIColor *faint = [foreground colorWithAlphaComponent:0.6];
    UIColor *fill = [foreground colorWithAlphaComponent:0.55];
    UIColor *track = [foreground colorWithAlphaComponent:0.12];
    for (UIImageView *icon in self.icons)
        icon.tintColor = faint;
    for (UILabel *title in self.titles)
        title.textColor = faint;
    self.cpuLabel.textColor = self.memoryLabel.textColor = foreground;
    self.cpuBar.backgroundColor = self.memoryBar.backgroundColor = track;
    self.cpuBar.fill.backgroundColor = fill;
    self.memoryBar.fill.backgroundColor = MonitorMemoryColor(reading.memoryFraction, fill);
    self.cpuLine.lineColor = fill;
    self.cpuLine.areaColor = track;
    self.heatLabel.textColor = MonitorThermalColor(reading.thermalState, [foreground colorWithAlphaComponent:0.75]);
}

- (void)showReading:(MonitorReading *)reading {
    int cpu = (int) round(reading.cpuBusy * 100);
    int memory = (int) round(reading.memoryFraction * 100);
    // the CPUs as they were until counted again
    if (reading.counted) {
        self.cpuLabel.text = [NSString stringWithFormat:@"%d%%", cpu];
        self.cpuBar.fraction = reading.cpuBusy;
    }
    self.cpuLine.values = self.sampler.cpuHistory;
    self.memoryLabel.text = [NSString stringWithFormat:@"%d%%", memory];
    self.memoryBar.fraction = reading.memoryFraction;
    self.heatLabel.text = MonitorThermalName(reading.thermalState);
    [UIView animateWithDuration:0.3 animations:^{
        [self applyColors];
        [self.cpuBar layoutIfNeeded];
        [self.memoryBar layoutIfNeeded];
    }];

    NSString *cpuText = reading.counted ? [NSString stringWithFormat:@"CPU %d%%, ", cpu] : @"";
    self.accessibilityLabel = [NSString stringWithFormat:@"%@memory %d%%, heat %@", cpuText, memory,
                               MonitorThermalName(reading.thermalState).lowercaseString];
}

@end
