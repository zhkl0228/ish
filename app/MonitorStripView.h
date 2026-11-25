//
//  MonitorStripView.h
//  iSH
//
//  Meters in what the terminal leaves right of its last column (TerminalView's
//  marginView): the CPU and the memory as the guest's top and free count them
//  (the device's CPUs; the app's memory against what iOS lets it have before
//  ending it), and how hot the device runs (MonitorSampler). Read while its
//  window's scene is in the foreground; touches go through it to the terminal,
//  whose tap on it shows their details (MonitorDetailViewController).
//

#import <UIKit/UIKit.h>
#import "MonitorSampler.h"

NS_ASSUME_NONNULL_BEGIN

// The memory's fill: the given color, orange past 75%, red past 90%, where
// the kernel's memory governor refuses the guest's new memory, and iOS ends
// the app not much later
UIColor *MonitorMemoryColor(double fraction, UIColor *normal);
// The heat's: the given color while nominal, then yellow, orange, red
UIColor *MonitorThermalColor(NSProcessInfoThermalState state, UIColor *normal);
NSString *MonitorThermalName(NSProcessInfoThermalState state);
extern const double kMonitorMemoryBrake;

// A meter's bar: how full, from the bottom up (or from the left, horizontal),
// in fill's color over its own (the track's)
@interface MeterBar : UIView
@property (nonatomic) double fraction;
@property (nonatomic) BOOL horizontal;
@property (readonly) UIView *fill;
@end

// A view of a sampler's readings: the meters in the terminal's margin
// (MonitorStripView), and in the window's column right of the terminal
// (MonitorColumnView), which read one sampler. It has the sampler read while
// it is not hidden, in a window whose scene is in the foreground (not behind
// another app, or the app in the background, where it may run on: Keep
// Running in Background), and shows each reading (showReading:).
@interface MonitorView : UIView

- (instancetype)initWithSampler:(MonitorSampler *)sampler;

// The theme's foreground, which the meters are drawn in, as the tab strip is
@property (nonatomic) UIColor *foregroundColor;
@property (readonly) MonitorSampler *sampler;
// Where the meters are, in its own points: what their details point at
@property (readonly) CGRect metersFrame;
// Its details asked for: VoiceOver's activation of it, and a tap
@property (copy, nullable) void (^activated)(void);

// For its subclasses: a reading to show, each as the sampler reads and the
// last one as it starts reading; and its colors, as the reading and
// foregroundColor have them
- (void)showReading:(MonitorReading *)reading;
- (void)applyColors;

@end

// Touches go through it to the terminal, whose tap on it is its activation
// (TerminalView's marginTapped)
@interface MonitorStripView : MonitorView
@end

NS_ASSUME_NONNULL_END
