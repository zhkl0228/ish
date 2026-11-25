//
//  MonitorColumnView.h
//  iSH
//
//  The meters in the window's column right of the terminal, where a phone that
//  folds puts its status bar, folded or not (TerminalViewController's
//  updateMonitorColumn): right under the status bar, drawn as it draws its
//  battery, room for more than the terminal's margin has (MonitorStripView),
//  whose sampler it reads. The CPU and the memory, each a gauge (a ring open
//  at its bottom, in green, orange past 75%, red past 90%, the percent in its
//  middle, its name in the opening), the CPU's last minute as a line under
//  its gauge; the heat, by name, while it is more than nominal. A tap
//  (VoiceOver's activation) shows their details (MonitorDetailViewController).
//

#import <UIKit/UIKit.h>
#import "MonitorStripView.h"

NS_ASSUME_NONNULL_BEGIN

// A line of a minute of the CPUs' busy share (MonitorSampler's cpuHistory),
// the newest reading at its right edge, kMonitorHistoryLength readings across
// its width, the room under the line filled
@interface MonitorSparkView : UIView
@property (nonatomic) NSArray<NSNumber *> *values;
@property (nonatomic) UIColor *lineColor;
@property (nonatomic) UIColor *areaColor;
@end

// A meter as the status bar's battery is drawn there: a ring open at its
// bottom, 270 degrees from its bottom left round over its top to its bottom
// right, as far as fraction in color over its track (strokeEnd, which
// animates); the value in its middle, its name in the opening
@interface MonitorGaugeView : UIView
@property (nonatomic) double fraction;
@property (nonatomic) UIColor *color;
@property (nonatomic) UIColor *trackColor;
@property (readonly) UILabel *valueLabel;
@property (readonly) UILabel *nameLabel;
@end

@interface MonitorColumnView : MonitorView
@end

NS_ASSUME_NONNULL_END
