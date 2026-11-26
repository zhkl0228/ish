//
//  MonitorDetailViewController.h
//  iSH
//
//  The meters' details, in a popover pointing at them (a popover on a phone
//  too, the terminal in sight): the CPUs, all and each, user and system, and
//  the device's load; the app's memory of its limit, and whether the kernel
//  refuses the guest's new memory; the JIT's code region, while it
//  translates; the heat and what it does. As the
//  sampler reads. Nothing in it turns the meters off: Settings and the View
//  menu do (UserPreferences' showMeters).
//

#import <UIKit/UIKit.h>
#import "MonitorSampler.h"

NS_ASSUME_NONNULL_BEGIN

@interface MonitorDetailViewController : UIViewController

- (instancetype)initWithSampler:(MonitorSampler *)sampler sourceView:(UIView *)view sourceRect:(CGRect)rect;

// The on-screen keyboard up (the terminal's view controller says so as it
// moves): the CPUs and the memory only, the JIT and the heat left out, as the
// column has them then, where the popover has not the room for all
@property (nonatomic) BOOL keyboardUp;

@end

NS_ASSUME_NONNULL_END
