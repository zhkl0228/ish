//
//  UserPreferences.h
//  iSH
//
//  Created by Charlie Melbye on 11/12/18.
//

#import <UIKit/UIKit.h>
#import "Theme.h"

typedef NS_ENUM(NSInteger, CapsLockMapping) {
    __CapsLockMapFirst = 0,
    CapsLockMapNone = 0,
    CapsLockMapControl,
    CapsLockMapEscape,
    __CapsLockMapLast,
};

typedef enum : NSUInteger {
    __OptionMapFirst = 0,
    OptionMapNone = 0,
    OptionMapEsc,
    __OptionMapLast,
} OptionMapping;

typedef NS_ENUM(NSInteger, CursorStyle) {
    __CursorStyleFirst = 0,
    CursorStyleBlock = 0,
    CursorStyleBeam,
    CursorStyleUnderline,
    __CursorStyleLast,
};

typedef NS_ENUM(NSInteger, ColorScheme) {
    __ColorSchemeFirst = 0,
    ColorSchemeMatchSystem = 0,
    ColorSchemeAlwaysLight,
    ColorSchemeAlwaysDark,
    __ColorSchemeLast,
};

NS_ASSUME_NONNULL_BEGIN

extern NSString *const kThemeForegroundColor;
extern NSString *const kThemeBackgroundColor;
// The font size until one is chosen, and the one ⌘0 goes back to
extern const int kDefaultFontSize;

@interface UserPreferences : NSObject

@property CapsLockMapping capsLockMapping;
@property OptionMapping optionMapping;
@property BOOL backtickMapEscape;
@property BOOL hideExtraKeysWithExternalKeyboard;
@property BOOL overrideControlSpace;
@property BOOL hideStatusBar;
// The meters right of the terminal's last column (MonitorStripView)
@property BOOL showMeters;
// The column's two middle keys for the program in the foreground: vi's : and
// /, a pager's space and q (TerminalViewController's updateColumnProfile);
// off, a shell's tab and / always
@property BOOL columnKeysFollowProgram;
@property (nonatomic) Theme *theme;
@property (nonatomic, readonly) Palette *palette;
@property BOOL shouldDisableDimming;
@property (null_resettable) NSString *fontFamily;
@property (readonly) NSString *fontFamilyUserFacingName;
@property (readonly) UIFont *approximateFont;
@property NSNumber *fontSize;
@property ColorScheme colorScheme;
@property (readonly) BOOL requestingDarkAppearance;
@property (readonly) UIUserInterfaceStyle userInterfaceStyle;
@property (readonly) UIKeyboardAppearance keyboardAppearance;
@property CursorStyle cursorStyle;
@property (readonly) NSString *cursorShapeName;
@property BOOL blinkCursor;
@property (readonly) UIStatusBarStyle statusBarStyle;
@property NSArray<NSString *> *launchCommand;
@property NSArray<NSString *> *bootCommand;
@property NSString *hostnameOverride;
// Same as above but returns nil if the user has never set the hostname
@property (readonly) NSString *_hostnameOverride;
@property (nullable) NSString *claudeProxy;
// Claude's Proxy as chosen in Settings: @YES None, @NO HTTPS through
// claudeProxy; nil not chosen yet, which claude-wrapper and Settings take as
// HTTPS when a claudeProxy is set (as it was before there was a choice)
@property (nullable) NSNumber *claudeNoProxy;
// The imported CLAUDE.md, its name and its content in base64 in one value
// ({"name": ..., "content": ...}), so that the guest (/proc/ish/defaults/
// claude_memory) changes or removes both at once
@property (nullable) NSDictionary<NSString *, NSString *> *claudeMemory;

+ (instancetype)shared;

- (BOOL)hasChangedLaunchCommand;

@end

extern NSString *const kPreferenceLaunchCommandKey;
extern NSString *const kPreferenceBootCommandKey;
extern NSString *const kHostnameOverrideKey;
// Where the meters' column was last laid out, and by what (TerminalViewController's
// updateMonitorColumn): /proc/ish/defaults/meters_column, to place them by on a phone
extern NSString *const kMetersColumnLayoutKey;

NS_ASSUME_NONNULL_END
