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
//  middle, its name in the opening), the CPU's last minute as a line over
//  them; the share of lately compiled code that runs natively (JIT: an AOT
//  image's or the JIT's), a gauge too, red once the code region is full; the
//  heat, by name, while it is more than nominal; each but the CPU while the
//  column has the room for it (rowsForRoom:). A tap (VoiceOver's activation)
//  shows their details (MonitorDetailViewController). Under them, at the
//  column's bottom, the terminal's keys (TerminalViewController's columnKeys)
//  have the room the network's rates and the processes had.
//

#import <UIKit/UIKit.h>
#import "MonitorStripView.h"

NS_ASSUME_NONNULL_BEGIN

// A line of a minute of the CPUs' busy share (MonitorSampler's cpuHistory),
// the newest reading at its right edge, kMonitorHistoryLength readings across
// its width, the room under the line filled; its top the highest reading, or
// leastTop where that is lower (1, all the CPUs busy, unless set)
@interface MonitorSparkView : UIView
@property (nonatomic) NSArray<NSNumber *> *values;
@property (nonatomic) double leastTop;
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
// The keyboard up (TerminalViewController's keyboardMoved:): the CPU, its
// line and the memory only, what is under them hidden
@property (nonatomic) BOOL keyboardUp;
// The meters' middle, which is kStatusInset left of the column's: what is put
// under them in the column lines up with it (TerminalViewController's keys)
@property (readonly) NSLayoutXAxisAnchor *metersCenterXAnchor;
// The height of the CPU and the memory, the meters the column keeps: what is
// put under them gives way for them (TerminalViewController's keys)
@property (readonly) CGFloat leastHeight;
@end

NS_ASSUME_NONNULL_END
