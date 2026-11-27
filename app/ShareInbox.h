//
//  ShareInbox.h
//  iSH
//
//  What other apps share with iSH (Share → iSH, app/Share): the share
//  extension, which cannot reach the app's Documents, leaves it in the app
//  group's container (SHARE_INBOX_STAGING) and says so (SHARE_INBOX_NOTIFIED);
//  the app moves it into its Documents' inbox, the guest's
//  /root/Documents/inbox.
//

#import <Foundation/Foundation.h>

// The app group container's folder the extension leaves what is shared in,
// each file written under a hidden name (.part-<uuid>) and then renamed: the
// app moves those not hidden
#define SHARE_INBOX_STAGING @"ShareInbox"
// The Darwin notification the extension posts once it left something there
#define SHARE_INBOX_NOTIFIED "app.ish.share-inbox"

// A name not taken in folder: name, else "base 2.ext", "base 3.ext", ...
static inline NSURL *ShareInboxFreeURL(NSURL *folder, NSString *name) {
    NSURL *url = [folder URLByAppendingPathComponent:name];
    NSString *base = name.stringByDeletingPathExtension;
    NSString *extension = name.pathExtension;
    for (int n = 2; [NSFileManager.defaultManager fileExistsAtPath:url.path]; n++) {
        NSString *numbered = [NSString stringWithFormat:@"%@ %d", base, n];
        if (extension.length > 0)
            numbered = [numbered stringByAppendingPathExtension:extension];
        url = [folder URLByAppendingPathComponent:numbered];
    }
    return url;
}

// The app's, as it starts: moves what is left there now, and then as the
// extension says it left more, and as the app comes to the front (for what
// was left while it was not running, when a Darwin notification is not
// delivered)
void ShareInboxStart(void);
