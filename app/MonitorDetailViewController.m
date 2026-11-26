//
//  MonitorDetailViewController.m
//  iSH
//

#import "MonitorDetailViewController.h"
#import "MonitorStripView.h"
#import "UserPreferences.h"

static const CGFloat kWidth = 264;
static const CGFloat kInset = 16;

static NSString *Percent(double fraction) {
    return [NSString stringWithFormat:@"%d%%", (int) round(fraction * 100)];
}

static NSString *Bytes(uint64_t bytes) {
    return [NSByteCountFormatter stringFromByteCount:(long long) bytes countStyle:NSByteCountFormatterCountStyleMemory];
}

// What the heat does, as ProcessInfo's thermal states are told of
static NSString *ThermalMeaning(NSProcessInfoThermalState state) {
    switch (state) {
        case NSProcessInfoThermalStateNominal: return @"Not warm.";
        case NSProcessInfoThermalStateFair: return @"Slightly warm.";
        case NSProcessInfoThermalStateSerious: return @"Hot: iOS slows the CPUs down, and the guest's programs with them.";
        case NSProcessInfoThermalStateCritical: return @"Too hot: iOS slows everything down until the device cools.";
    }
}

@interface MonitorDetailViewController () <UIPopoverPresentationControllerDelegate>

@property MonitorSampler *sampler;
@property UIStackView *stack;
@property UILabel *cpuValue;
@property UILabel *cpuSplit;
@property UIStackView *cores;
@property UILabel *load;
@property UILabel *memoryValue;
@property UILabel *memoryAmount;
@property MeterBar *memoryBar;
@property UILabel *memoryLeft;
@property UILabel *brake;
@property UILabel *heatValue;
@property UILabel *heatMeaning;

@end

@implementation MonitorDetailViewController

- (instancetype)initWithSampler:(MonitorSampler *)sampler sourceView:(UIView *)view sourceRect:(CGRect)rect {
    if (self = [super initWithNibName:nil bundle:nil]) {
        _sampler = sampler;
        self.modalPresentationStyle = UIModalPresentationPopover;
        // In the app's appearance, as Settings are (AboutNavigationController),
        // the popover's background too, which is not this view's
        UIUserInterfaceStyle style = UserPreferences.shared.userInterfaceStyle;
        self.overrideUserInterfaceStyle = style;
        UITraitCollection *traits = [UITraitCollection traitCollectionWithTraitsFromCollections:@[
            [UITraitCollection traitCollectionWithUserInterfaceStyle:style],
            [UITraitCollection traitCollectionWithUserInterfaceLevel:UIUserInterfaceLevelElevated],
        ]];
        UIPopoverPresentationController *popover = self.popoverPresentationController;
        popover.sourceView = view;
        popover.sourceRect = rect;
        popover.permittedArrowDirections = UIPopoverArrowDirectionRight;
        popover.backgroundColor = [UIColor.systemBackgroundColor resolvedColorWithTraitCollection:traits];
        popover.delegate = self;
    }
    return self;
}

// A popover on a phone too, not a sheet over the terminal
- (UIModalPresentationStyle)adaptivePresentationStyleForPresentationController:(UIPresentationController *)controller
                                                               traitCollection:(UITraitCollection *)traitCollection {
    return UIModalPresentationNone;
}

- (void)viewDidLoad {
    [super viewDidLoad];
    UIStackView *stack = [UIStackView new];
    stack.axis = UILayoutConstraintAxisVertical;
    stack.spacing = 4;
    stack.translatesAutoresizingMaskIntoConstraints = NO;
    [self.view addSubview:stack];
    self.stack = stack;
    // the safe area leaves out the arrow
    UILayoutGuide *safe = self.view.safeAreaLayoutGuide;
    [NSLayoutConstraint activateConstraints:@[
        [stack.topAnchor constraintEqualToAnchor:safe.topAnchor constant:kInset],
        [stack.leadingAnchor constraintEqualToAnchor:safe.leadingAnchor constant:kInset],
        [stack.trailingAnchor constraintEqualToAnchor:safe.trailingAnchor constant:-kInset],
        [stack.bottomAnchor constraintLessThanOrEqualToAnchor:safe.bottomAnchor constant:-kInset],
    ]];

    // The CPUs: all of them, user and system, each one, the device's load
    self.cpuValue = [self addHeading:@"CPU"];
    self.cpuSplit = [self addDetail];
    UIStackView *cores = [UIStackView new];
    cores.spacing = 4;
    cores.translatesAutoresizingMaskIntoConstraints = NO;
    UIView *coresRow = [UIView new];
    [coresRow addSubview:cores];
    [NSLayoutConstraint activateConstraints:@[
        [cores.leadingAnchor constraintEqualToAnchor:coresRow.leadingAnchor],
        [cores.trailingAnchor constraintLessThanOrEqualToAnchor:coresRow.trailingAnchor],
        [cores.topAnchor constraintEqualToAnchor:coresRow.topAnchor constant:4],
        [cores.bottomAnchor constraintEqualToAnchor:coresRow.bottomAnchor constant:-4],
        [cores.heightAnchor constraintEqualToConstant:28],
    ]];
    [stack addArrangedSubview:coresRow];
    self.cores = cores;
    self.load = [self addDetail];
    [stack setCustomSpacing:16 afterView:self.load];

    // The memory: of the app's limit, a mark on its bar where the brake is
    self.memoryValue = [self addHeading:@"Memory"];
    self.memoryAmount = [self addDetail];
    MeterBar *bar = [MeterBar new];
    bar.horizontal = YES;
    bar.backgroundColor = UIColor.tertiarySystemFillColor;
    bar.translatesAutoresizingMaskIntoConstraints = NO;
    UIView *mark = [UIView new];
    mark.backgroundColor = UIColor.secondaryLabelColor;
    mark.translatesAutoresizingMaskIntoConstraints = NO;
    UIView *barRow = [UIView new];
    [barRow addSubview:bar];
    [barRow addSubview:mark];
    [NSLayoutConstraint activateConstraints:@[
        [bar.leadingAnchor constraintEqualToAnchor:barRow.leadingAnchor],
        [bar.trailingAnchor constraintEqualToAnchor:barRow.trailingAnchor],
        [bar.topAnchor constraintEqualToAnchor:barRow.topAnchor constant:6],
        [bar.bottomAnchor constraintEqualToAnchor:barRow.bottomAnchor constant:-6],
        [bar.heightAnchor constraintEqualToConstant:6],
        // the bar's leading is the row's: its trailing times 0.9 is 90% along
        [NSLayoutConstraint constraintWithItem:mark attribute:NSLayoutAttributeCenterX relatedBy:NSLayoutRelationEqual
                                        toItem:bar attribute:NSLayoutAttributeTrailing multiplier:kMonitorMemoryBrake constant:0],
        [mark.centerYAnchor constraintEqualToAnchor:bar.centerYAnchor],
        [mark.widthAnchor constraintEqualToConstant:2],
        [mark.heightAnchor constraintEqualToConstant:14],
    ]];
    [stack addArrangedSubview:barRow];
    self.memoryBar = bar;
    self.memoryLeft = [self addDetail];
    self.brake = [self addDetail];
    [stack setCustomSpacing:16 afterView:self.brake];

    // No button to turn the meters off: a tap that missed one turned them off
    // with no sign of how (Settings and the View menu do it)
    self.heatValue = [self addHeading:@"Heat"];
    self.heatMeaning = [self addDetail];

    [NSNotificationCenter.defaultCenter addObserver:self selector:@selector(samplerDidRead:)
                                               name:MonitorSamplerDidReadNotification object:self.sampler];
    [self showReading:self.sampler.reading];
}

// A heading and its value, at the right
- (UILabel *)addHeading:(NSString *)title {
    UILabel *name = [UILabel new];
    name.text = title;
    name.font = [UIFont systemFontOfSize:15 weight:UIFontWeightSemibold];
    UILabel *value = [UILabel new];
    value.font = [UIFont monospacedDigitSystemFontOfSize:15 weight:UIFontWeightSemibold];
    value.textAlignment = NSTextAlignmentRight;
    [value setContentHuggingPriority:UILayoutPriorityDefaultLow forAxis:UILayoutConstraintAxisHorizontal];
    UIStackView *row = [[UIStackView alloc] initWithArrangedSubviews:@[name, value]];
    row.spacing = 8;
    [self.stack addArrangedSubview:row];
    return value;
}

- (UILabel *)addDetail {
    UILabel *label = [UILabel new];
    label.font = [UIFont monospacedDigitSystemFontOfSize:13 weight:UIFontWeightRegular];
    label.textColor = UIColor.secondaryLabelColor;
    label.numberOfLines = 0;
    [self.stack addArrangedSubview:label];
    return label;
}

- (MeterBar *)addCoreBar {
    MeterBar *bar = [MeterBar new];
    bar.backgroundColor = UIColor.tertiarySystemFillColor;
    bar.fill.backgroundColor = [UIColor.labelColor colorWithAlphaComponent:0.55];
    [bar.widthAnchor constraintEqualToConstant:8].active = YES;
    [self.cores addArrangedSubview:bar];
    return bar;
}

- (void)samplerDidRead:(NSNotification *)notification {
    [UIView animateWithDuration:0.3 animations:^{
        [self showReading:self.sampler.reading];
        [self.view layoutIfNeeded];
    }];
}

- (void)showReading:(MonitorReading *)reading {
    // The CPUs as they were until counted again (the first reading after the
    // scene comes back)
    if (reading.counted) {
        self.cpuValue.text = Percent(reading.cpuBusy);
        self.cpuSplit.text = [NSString stringWithFormat:@"User %@ · System %@", Percent(reading.cpuUser), Percent(reading.cpuSystem)];
    }
    NSArray<NSNumber *> *coreBusy = reading.coreBusy;
    if (coreBusy.count != 0 && self.cores.arrangedSubviews.count != coreBusy.count) {
        for (UIView *bar in self.cores.arrangedSubviews)
            [bar removeFromSuperview];
        for (NSUInteger i = 0; i < coreBusy.count; i++)
            [self addCoreBar];
    }
    [coreBusy enumerateObjectsUsingBlock:^(NSNumber *busy, NSUInteger i, BOOL *_) {
        ((MeterBar *) self.cores.arrangedSubviews[i]).fraction = busy.doubleValue;
    }];
    self.load.text = [NSString stringWithFormat:@"Device load %.2f · %.2f · %.2f", reading.load1, reading.load5, reading.load15];

    self.memoryValue.text = Percent(reading.memoryFraction);
    self.memoryAmount.text = [NSString stringWithFormat:@"%@ of %@ %@", Bytes(reading.memoryUsed),
                              reading.appLimit ? @"the app's" : @"the device's", Bytes(reading.memoryTotal)];
    self.memoryBar.fraction = reading.memoryFraction;
    self.memoryBar.fill.backgroundColor = MonitorMemoryColor(reading.memoryFraction, [UIColor.labelColor colorWithAlphaComponent:0.55]);
    self.memoryLeft.hidden = !reading.appLimit;
    self.memoryLeft.text = [NSString stringWithFormat:@"%@ more and iOS ends the app", Bytes(reading.memoryTotal - reading.memoryUsed)];
    if (reading.memoryBraked) {
        self.brake.text = @"Braked: the guest's new memory is refused (ENOMEM)";
        self.brake.textColor = UIColor.systemRedColor;
    } else {
        self.brake.text = [NSString stringWithFormat:@"Past %@, the guest's new memory is refused", Percent(kMonitorMemoryBrake)];
        self.brake.textColor = UIColor.secondaryLabelColor;
    }

    self.heatValue.text = MonitorThermalName(reading.thermalState);
    self.heatValue.textColor = MonitorThermalColor(reading.thermalState, UIColor.labelColor);
    self.heatMeaning.text = ThermalMeaning(reading.thermalState);

    [self updateSize];
}

// As high as what it shows: the brake's line, or the heat's, may take two
- (void)updateSize {
    CGSize fitted = [self.stack systemLayoutSizeFittingSize:CGSizeMake(kWidth - 2 * kInset, UILayoutFittingCompressedSize.height)
                              withHorizontalFittingPriority:UILayoutPriorityRequired
                                    verticalFittingPriority:UILayoutPriorityFittingSizeLevel];
    CGSize size = CGSizeMake(kWidth, ceil(fitted.height) + 2 * kInset);
    if (!CGSizeEqualToSize(size, self.preferredContentSize))
        self.preferredContentSize = size;
}

@end
