//
//  MonitorColumnView.h
//  iSH
//
//  The meters in the window's column right of the terminal, where a phone that
//  folds puts its status bar, folded or not (TerminalViewController's
//  updateMonitorColumn): under the status bar, room for more than the
//  terminal's margin has (MonitorStripView), whose sampler it reads. The CPU,
//  a percent over a bar and its last minute as a line; the memory, a percent
//  over a bar; the heat, by name. A tap (VoiceOver's activation) shows their
//  details (MonitorDetailViewController).
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

@interface MonitorColumnView : MonitorView
@end

NS_ASSUME_NONNULL_END
