//
//  UIActions.h
//  iSH
//

#include <stddef.h>

// /proc/ish/ui's do_ui_action (fs/proc/ish.h): an action in the window of the
// terminal in front, the bytes a program wrote
int do_app_ui_action(const char *bytes, size_t size);
