//
//  UIApplication+OpenURL.h
//  iSH
//
//  Created by Theodore Dubois on 9/23/18.
//

#import <UIKit/UIKit.h>

NS_ASSUME_NONNULL_BEGIN

@interface UIApplication (OpenURL)

+ (void)openURL:(NSString *)url;

@end

// A program's request to open a URL (/proc/ish/open_url, which xdg-open
// writes): asked of the person in the terminal window in front, and opened if
// they allow it. The kernel's open_url (fs/proc/ish.h).
int ask_to_open_url(const char *url, size_t size);

NS_ASSUME_NONNULL_END
