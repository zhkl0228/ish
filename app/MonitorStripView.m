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
        case NSProcessInfoThermalStateNominal: return NSLocalizedString(@"Nominal", @"Heat: ProcessInfo's thermal state, not warm");
        case NSProcessInfoThermalStateFair: return NSLocalizedString(@"Fair", @"Heat: ProcessInfo's thermal state, slightly warm");
        case NSProcessInfoThermalStateSerious: return NSLocalizedString(@"Serious", @"Heat: ProcessInfo's thermal state, hot");
        case NSProcessInfoThermalStateCritical: return NSLocalizedString(@"Critical", @"Heat: ProcessInfo's thermal state, too hot");
    }
}

NSString *MonitorSpokenReading(MonitorReading *reading) {
    int cpu = (int) round(reading.cpuBusy * 100);
    int memory = (int) round(reading.memoryFraction * 100);
    NSString *heat = MonitorThermalName(reading.thermalState).lowercaseString;
    if (!reading.counted)
        return [NSString stringWithFormat:NSLocalizedString(@"Memory %1$d%%, heat %2$@",
                                                            @"The meters read aloud, the CPU not counted yet"),
                memory, heat];
    return [NSString stringWithFormat:NSLocalizedString(@"CPU %1$d%%, memory %2$d%%, heat %3$@", @"The meters read aloud"),
            cpu, memory, heat];
}

NSString *MonitorDetailsHint(void) {
    return NSLocalizedString(@"Shows the CPUs, the memory and the heat in detail.", @"The meters' accessibility hint");
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

@implementation MonitorView {
    // The sampler started for it (updateSampling)
    BOOL _sampling;
    // Its window's scene in the foreground, as the scene's notifications say
    BOOL _sceneForeground;
}

- (instancetype)initWithSampler:(MonitorSampler *)sampler {
    if (self = [super initWithFrame:CGRectZero]) {
        _sampler = sampler;
        _foregroundColor = UIColor.labelColor;
        [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(samplerDidRead:)
                                                   name:MonitorSamplerDidReadNotification object:sampler];
    }
    return self;
}

- (void)dealloc {
    if (_sampling)
        [_sampler stop];
}

- (void)setForegroundColor:(UIColor *)foregroundColor {
    _foregroundColor = foregroundColor;
    [self applyColors];
}

- (void)showReading:(MonitorReading *)reading {
}

- (void)applyColors {
}

- (CGRect)metersFrame {
    return self.bounds;
}

- (BOOL)accessibilityActivate {
    self.activated();
    return YES;
}

#pragma mark Reading

- (void)didMoveToWindow {
    [super didMoveToWindow];
    NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
    [center removeObserver:self name:UISceneWillEnterForegroundNotification object:nil];
    [center removeObserver:self name:UISceneDidEnterBackgroundNotification object:nil];
    // (a scene's activationState of nil would be 0: ForegroundActive)
    UIWindowScene *scene = self.window.windowScene;
    _sceneForeground = scene != nil && (scene.activationState == UISceneActivationStateForegroundActive ||
                                        scene.activationState == UISceneActivationStateForegroundInactive);
    if (scene != nil) {
        [center addObserver:self selector:@selector(sceneWillEnterForeground:)
                       name:UISceneWillEnterForegroundNotification object:scene];
        [center addObserver:self selector:@selector(sceneDidEnterBackground:)
                       name:UISceneDidEnterBackgroundNotification object:scene];
    }
    [self updateSampling];
}

- (void)setHidden:(BOOL)hidden {
    [super setHidden:hidden];
    [self updateSampling];
}

- (void)sceneWillEnterForeground:(NSNotification *)notification {
    _sceneForeground = YES;
    [self updateSampling];
}

- (void)sceneDidEnterBackground:(NSNotification *)notification {
    _sceneForeground = NO;
    [self updateSampling];
}

// Started and stopped in pairs (MonitorSampler's start): one start while it
// shows, in the foreground
- (void)updateSampling {
    BOOL sampling = self.window != nil && _sceneForeground && !self.hidden;
    if (sampling == _sampling)
        return;
    _sampling = sampling;
    if (!sampling) {
        [self.sampler stop];
        return;
    }
    [self.sampler start];
    // the last reading at once, where another view had the sampler reading
    MonitorReading *reading = self.sampler.reading;
    if (reading != nil)
        [self showReading:reading];
}

- (void)samplerDidRead:(NSNotification *)notification {
    [self showReading:self.sampler.reading];
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

- (instancetype)initWithSampler:(MonitorSampler *)sampler {
    if (self = [super initWithSampler:sampler]) {
        self.userInteractionEnabled = NO;
        self.clipsToBounds = YES;
        self.isAccessibilityElement = YES;
        self.accessibilityTraits = UIAccessibilityTraitButton | UIAccessibilityTraitUpdatesFrequently;
        self.accessibilityHint = MonitorDetailsHint();

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

- (void)showReading:(MonitorReading *)reading {
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

    self.accessibilityLabel = MonitorSpokenReading(reading);
}

@end
