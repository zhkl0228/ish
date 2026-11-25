//
//  MonitorDetailViewController.h
//  iSH
//
//  The meters' details, in a popover pointing at them (a popover on a phone
//  too, the terminal in sight): the CPUs, all and each, user and system, and
//  the device's load; the app's memory of its limit, what is left before iOS
//  ends the app, and whether the kernel refuses the guest's new memory; the
//  heat and what it does. As the sampler reads. Hide Meters turns them off
//  (UserPreferences' showMeters).
//

#import <UIKit/UIKit.h>
#import "MonitorSampler.h"

NS_ASSUME_NONNULL_BEGIN

@interface MonitorDetailViewController : UIViewController

- (instancetype)initWithSampler:(MonitorSampler *)sampler sourceView:(UIView *)view sourceRect:(CGRect)rect;

@end

NS_ASSUME_NONNULL_END
