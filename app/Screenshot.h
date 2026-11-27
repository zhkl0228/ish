//
//  Screenshot.h
//  iSH
//

#include <stddef.h>

// /proc/ish/screenshot's take_screenshot (fs/proc/ish.h): the windows of iSH
// in front, as the screen shows them, a PNG
int take_app_screenshot(void **png, size_t *size);
