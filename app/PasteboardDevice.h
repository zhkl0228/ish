#include <stddef.h>

// Pasteboard is implementation of /dev/clipboard device
extern struct dev_ops clipboard_dev;
// /proc/ish/clipboard_image's get_clipboard_image (fs/proc/ish.h): the image
// on the clipboard, a PNG
int clipboard_image_png(void **png, size_t *size);
