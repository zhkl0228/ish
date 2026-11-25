//
//  MonitorStripView.m
//  iSH
//

#import "MonitorStripView.h"

const double kMonitorMemoryBrake = 0.9;
static const double kMemoryWarning = 0.75;

UIColor *MonitorMemoryColor(double fraction, UIColor *normal) {
    if (fraction >= kMonitorMemoryBrake)
        return UIColor.systemRedColor;
    if (fraction >= kMemoryWarning)
        return UIColor.systemOrangeColor;
    return normal;
}

UIColor *MonitorThermalColor(NSProcessInfoThermalState state, UIColor *normal) {
    switch (state) {
        case NSProcessInfoThermalStateNominal: return normal;
        case NSProcessInfoThermalStateFair: return UIColor.systemYellowColor;
        case NSProcessInfoThermalStateSerious: return UIColor.systemOrangeColor;
        case NSProcessInfoThermalStateCritical: return UIColor.systemRedColor;
    }
}

NSString *MonitorThermalName(NSProcessInfoThermalState state) {
    switch (state) {
        case NSProcessInfoThermalStateNominal: return @"Nominal";
        case NSProcessInfoThermalStateFair: return @"Fair";
        case NSProcessInfoThermalStateSerious: return @"Serious";
        case NSProcessInfoThermalStateCritical: return @"Critical";
    }
}

@implementation MeterBar

- (instancetype)initWithFrame:(CGRect)frame {
    if (self = [super initWithFrame:frame]) {
        _fill = [UIView new];
        [self addSubview:_fill];
        self.clipsToBounds = YES;
    }
    return self;
}

- (void)setFraction:(double)fraction {
    _fraction = fraction;
    [self setNeedsLayout];
}

- (void)setHorizontal:(BOOL)horizontal {
    _horizontal = horizontal;
    [self setNeedsLayout];
}

- (void)layoutSubviews {
    [super layoutSubviews];
    CGSize size = self.bounds.size;
    self.layer.cornerRadius = MIN(size.width, size.height) / 2;
    if (self.horizontal) {
        self.fill.frame = CGRectMake(0, 0, round(size.width * self.fraction), size.height);
    } else {
        CGFloat height = round(size.height * self.fraction);
        self.fill.frame = CGRectMake(0, size.height - height, size.width, height);
    }
}

@end

@interface MonitorStripView ()

@property UIStackView *stack;
@property UIImageView *cpuIcon;
@property UILabel *cpuLabel;
@property MeterBar *cpuBar;
@property UIImageView *memoryIcon;
@property UILabel *memoryLabel;
@property MeterBar *memoryBar;
@property UIImageView *heatIcon;

@end

@implementation MonitorStripView

- (instancetype)initWithFrame:(CGRect)frame {
    if (self = [super initWithFrame:frame]) {
        self.userInteractionEnabled = NO;
        self.clipsToBounds = YES;
        self.isAccessibilityElement = YES;
        self.accessibilityTraits = UIAccessibilityTraitButton | UIAccessibilityTraitUpdatesFrequently;
        self.accessibilityHint = @"Shows the CPUs, the memory and the heat in detail.";
        _foregroundColor = UIColor.labelColor;
        _sampler = [MonitorSampler new];
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(samplerDidRead:)
                                                   name:MonitorSamplerDidReadNotification object:_sampler];

        UIStackView *stack = [UIStackView new];
        stack.axis = UILayoutConstraintAxisVertical;
        stack.alignment = UIStackViewAlignmentCenter;
        stack.spacing = 3;
        stack.translatesAutoresizingMaskIntoConstraints = NO;
        [self addSubview:stack];
        self.stack = stack;

        self.cpuIcon = [self addIcon:@"cpu" to:stack];
        self.cpuLabel = [self addLabelTo:stack];
        self.cpuBar = [self addBarTo:stack];
        self.memoryIcon = [self addIcon:@"memorychip" to:stack];
        self.memoryLabel = [self addLabelTo:stack];
        self.memoryBar = [self addBarTo:stack];
        self.heatIcon = [self addIcon:@"thermometer.medium" to:stack];
        [stack setCustomSpacing:10 afterView:self.cpuBar];
        [stack setCustomSpacing:10 afterView:self.memoryBar];

        // 3 points from the last column, 2 from the screen's edge: 13 points
        // across where the terminal leaves the least (TerminalView's
        // marginView: 18), the bars clear of the scroll bar at the edge. Too
        // short for all of it (the keyboard up, the phone on its side), the
        // bars give way, then the bottom is cut off.
        NSLayoutConstraint *bottom = [stack.bottomAnchor constraintLessThanOrEqualToAnchor:self.bottomAnchor constant:-6];
        bottom.priority = UILayoutPriorityRequired - 1;
        [NSLayoutConstraint activateConstraints:@[
            [stack.topAnchor constraintEqualToAnchor:self.topAnchor constant:6],
            [stack.leadingAnchor constraintEqualToAnchor:self.leadingAnchor constant:3],
            [stack.trailingAnchor constraintEqualToAnchor:self.trailingAnchor constant:-2],
            bottom,
            [self.cpuLabel.widthAnchor constraintLessThanOrEqualToAnchor:stack.widthAnchor],
            [self.memoryLabel.widthAnchor constraintLessThanOrEqualToAnchor:stack.widthAnchor],
        ]];
        [self applyColors];
    }
    return self;
}

- (UIImageView *)addIcon:(NSString *)symbol to:(UIStackView *)stack {
    UIImageSymbolConfiguration *config = [UIImageSymbolConfiguration configurationWithPointSize:10 weight:UIImageSymbolWeightSemibold];
    UIImageView *icon = [[UIImageView alloc] initWithImage:[UIImage systemImageNamed:symbol withConfiguration:config]];
    icon.contentMode = UIViewContentModeScaleAspectFit;
    [stack addArrangedSubview:icon];
    return icon;
}

// A percent: 9-point digits, smaller for 100 where the strip is narrow
- (UILabel *)addLabelTo:(UIStackView *)stack {
    UILabel *label = [UILabel new];
    label.font = [UIFont monospacedDigitSystemFontOfSize:9 weight:UIFontWeightSemibold];
    label.textAlignment = NSTextAlignmentCenter;
    label.adjustsFontSizeToFitWidth = YES;
    label.minimumScaleFactor = 0.6;
    label.text = @" ";
    [stack addArrangedSubview:label];
    return label;
}

- (MeterBar *)addBarTo:(UIStackView *)stack {
    MeterBar *bar = [MeterBar new];
    bar.translatesAutoresizingMaskIntoConstraints = NO;
    [stack addArrangedSubview:bar];
    NSLayoutConstraint *height = [bar.heightAnchor constraintEqualToConstant:48];
    height.priority = UILayoutPriorityDefaultLow;
    [NSLayoutConstraint activateConstraints:@[
        [bar.widthAnchor constraintEqualToConstant:4],
        [bar.heightAnchor constraintGreaterThanOrEqualToConstant:8],
        height,
    ]];
    return bar;
}

- (CGRect)metersFrame {
    return self.stack.frame;
}

- (BOOL)accessibilityActivate {
    self.activated();
    return YES;
}

- (void)setForegroundColor:(UIColor *)foregroundColor {
    _foregroundColor = foregroundColor;
    [self applyColors];
}

// As the last reading has them (none before the first: nominal, empty)
- (void)applyColors {
    MonitorReading *reading = self.sampler.reading;
    UIColor *foreground = self.foregroundColor;
    UIColor *fill = [foreground colorWithAlphaComponent:0.55];
    self.cpuIcon.tintColor = self.memoryIcon.tintColor = [foreground colorWithAlphaComponent:0.5];
    self.cpuLabel.textColor = self.memoryLabel.textColor = [foreground colorWithAlphaComponent:0.75];
    self.cpuBar.backgroundColor = self.memoryBar.backgroundColor = [foreground colorWithAlphaComponent:0.12];
    self.cpuBar.fill.backgroundColor = fill;
    self.memoryBar.fill.backgroundColor = MonitorMemoryColor(reading.memoryFraction, fill);
    self.heatIcon.tintColor = MonitorThermalColor(reading.thermalState, [foreground colorWithAlphaComponent:0.35]);
}

#pragma mark Reading

// Read while the window's scene is in the foreground, not behind another app
// or with the app in the background (where it may run on, Keep Running in
// Background)
- (void)didMoveToWindow {
    [super didMoveToWindow];
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center removeObserver:self name:UISceneWillEnterForegroundNotification object:nil];
    [center removeObserver:self name:UISceneDidEnterBackgroundNotification object:nil];
    UIWindowScene *scene = self.window.windowScene;
    if (scene == nil) {
        [self.sampler stop];
        return;
    }
    [center addObserver:self selector:@selector(sceneWillEnterForeground:) name:UISceneWillEnterForegroundNotification object:scene];
    [center addObserver:self selector:@selector(sceneDidEnterBackground:) name:UISceneDidEnterBackgroundNotification object:scene];
    if (scene.activationState == UISceneActivationStateForegroundActive ||
        scene.activationState == UISceneActivationStateForegroundInactive)
        [self.sampler start];
}

- (void)sceneWillEnterForeground:(NSNotification *)notification {
    [self.sampler start];
}

- (void)sceneDidEnterBackground:(NSNotification *)notification {
    [self.sampler stop];
}

- (void)samplerDidRead:(NSNotification *)notification {
    MonitorReading *reading = self.sampler.reading;
    int cpu = (int) round(reading.cpuBusy * 100);
    int memory = (int) round(reading.memoryFraction * 100);
    // the CPUs as they were until counted again
    if (reading.counted) {
        self.cpuLabel.text = [NSString stringWithFormat:@"%d", cpu];
        self.cpuBar.fraction = reading.cpuBusy;
    }
    self.memoryLabel.text = [NSString stringWithFormat:@"%d", memory];
    self.memoryBar.fraction = reading.memoryFraction;
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
