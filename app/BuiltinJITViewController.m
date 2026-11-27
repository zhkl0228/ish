//
//  BuiltinJITViewController.m
//  iSH
//

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import "BuiltinJIT.h"
#import "BuiltinJITViewController.h"
#import "OnDevicePairing.h"

enum {
    SectionStatus,
    SectionPairing,
    SectionDiskImage,
    SectionLog,
    SectionCount,
};

@interface BuiltinJITViewController () <UIDocumentPickerDelegate>
// The pairing going on, and the alert that tells of it
@property (nullable) OnDevicePairing *pairing;
@property (nullable) UIAlertController *pairingAlert;
@end

@implementation BuiltinJITViewController

- (instancetype)init {
    return [super initWithStyle:UITableViewStyleInsetGrouped];
}

- (void)viewDidLoad {
    [super viewDidLoad];
    self.title = @"JIT";
    [NSNotificationCenter.defaultCenter addObserver:self.tableView
                                           selector:@selector(reloadData)
                                               name:BuiltinJITChangedNotification
                                             object:nil];
}

- (void)dealloc {
    [NSNotificationCenter.defaultCenter removeObserver:self.tableView];
}

- (NSInteger)numberOfSectionsInTableView:(UITableView *)tableView {
    return SectionCount;
}

- (NSInteger)tableView:(UITableView *)tableView numberOfRowsInSection:(NSInteger)section {
    switch (section) {
    case SectionStatus: return 4;
    case SectionPairing: return OnDevicePairing.available ? 2 : 1;
    case SectionDiskImage: return 2;
    case SectionLog: return MAX(BuiltinJIT.shared.log.count, 1);
    }
    return 0;
}

- (NSString *)tableView:(UITableView *)tableView titleForHeaderInSection:(NSInteger)section {
    switch (section) {
    case SectionPairing: return @"Pairing File";
    case SectionDiskImage: return @"Developer Disk Image";
    case SectionLog: return @"Last Attempt";
    }
    return nil;
}

- (NSString *)tableView:(UITableView *)tableView titleForFooterInSection:(NSInteger)section {
    switch (section) {
    case SectionStatus:
        return @"iSH starts at once without the JIT, and takes it when it comes: programs compiled from then on "
                "run as native code. It needs Developer Mode, iSH signed with get-task-allow (a development profile) "
                "and the Network Extensions capability, and Wi-Fi (or, without, airplane mode after the tunnel is "
                "up). iSH's own loopback tunnel, a VPN, runs only while the JIT is enabled, unless one is up already "
                "(LocalDevVPN): iOS turns off another VPN while it runs.\n\n"
                "Code Region: the room for translated code. When it is full every program's code is translated "
                "again, so a larger one means fewer of those, and takes that much more memory as the JIT is "
                "enabled (the debugger writes in each of its pages). It is made once: a new size applies at iSH's "
                "next start if the JIT translates now.";
    case SectionPairing:
        return OnDevicePairing.available
            ? @"The device's pairing file: made here, by pairing the device with iSH in Settings, or on a computer "
               "with idevice_pair (or another pairing tool), imported here or copied to iSH's StikJIT folder with "
               "Finder."
            : @"The device's pairing file, made once on a computer with idevice_pair (or another pairing tool), "
               "imported here or copied to iSH's StikJIT folder with Finder. From iOS 27 it can be made here.";
    case SectionDiskImage:
        return @"Downloaded the first time it is needed, from StikJIT's sources, and mounted once a boot.";
    }
    return nil;
}

- (UITableViewCell *)cellWithText:(NSString *)text detail:(nullable NSString *)detail {
    UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleSubtitle reuseIdentifier:nil];
    cell.textLabel.text = text;
    cell.textLabel.numberOfLines = 0;
    cell.detailTextLabel.text = detail;
    cell.detailTextLabel.numberOfLines = 0;
    return cell;
}

- (UITableViewCell *)tableView:(UITableView *)tableView cellForRowAtIndexPath:(NSIndexPath *)indexPath {
    BuiltinJIT *jit = BuiltinJIT.shared;
    switch (indexPath.section) {
    case SectionStatus:
        if (indexPath.row == 0) {
            UITableViewCell *cell = [self cellWithText:jit.translating ? @"Translating" : @"Not translating"
                                                detail:jit.status];
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            return cell;
        } else if (indexPath.row == 1) {
            UITableViewCell *cell = [self cellWithText:@"Enable JIT Now" detail:nil];
            cell.textLabel.textColor = jit.busy || jit.translating ? UIColor.secondaryLabelColor : self.view.tintColor;
            return cell;
        } else if (indexPath.row == 3) {
            NSUInteger mb = BuiltinJIT.codeRegionMB, now = jit.codeRegionMBNow;
            NSString *detail = now == 0 || now == mb
                ? [NSString stringWithFormat:@"%lu MB", (unsigned long) mb]
                : [NSString stringWithFormat:@"%lu MB from iSH's next start (%lu MB now)", (unsigned long) mb,
                                             (unsigned long) now];
            UITableViewCell *cell = [[UITableViewCell alloc] initWithStyle:UITableViewCellStyleValue1
                                                           reuseIdentifier:nil];
            cell.textLabel.text = @"Code Region";
            cell.detailTextLabel.text = detail;
            cell.detailTextLabel.numberOfLines = 0;
            cell.accessoryType = UITableViewCellAccessoryDisclosureIndicator;
            return cell;
        } else {
            UITableViewCell *cell = [self cellWithText:@"Enable When iSH Starts" detail:nil];
            UISwitch *toggle = [UISwitch new];
            toggle.on = jit.enabledAtStart;
            [toggle addTarget:self action:@selector(enabledAtStartChanged:) forControlEvents:UIControlEventValueChanged];
            cell.accessoryView = toggle;
            cell.selectionStyle = UITableViewCellSelectionStyleNone;
            return cell;
        }
    case SectionPairing: {
        BOOL pair = OnDevicePairing.available && indexPath.row == 0;
        UITableViewCell *cell = [self cellWithText:pair ? @"Pair This Device…" : @"Import Pairing File…"
                                            detail:pair ? nil : jit.hasPairingFile ? @"There is one" : @"None"];
        cell.textLabel.textColor = self.view.tintColor;
        return cell;
    }
    case SectionDiskImage: {
        UITableViewCell *cell = [self cellWithText:indexPath.row == 0 ? @"Prepare Device" : @"Reset Developer Disk Image"
                                            detail:nil];
        cell.textLabel.textColor = jit.busy ? UIColor.secondaryLabelColor
                                 : indexPath.row == 0 ? self.view.tintColor : UIColor.systemRedColor;
        return cell;
    }
    case SectionLog: {
        NSArray<NSString *> *log = jit.log;
        UITableViewCell *cell = [self cellWithText:log.count ? log[indexPath.row] : @"Nothing yet" detail:nil];
        cell.textLabel.font = [UIFont monospacedSystemFontOfSize:12 weight:UIFontWeightRegular];
        cell.selectionStyle = UITableViewCellSelectionStyleNone;
        return cell;
    }
    }
    return [self cellWithText:@"" detail:nil];
}

- (void)tableView:(UITableView *)tableView didSelectRowAtIndexPath:(NSIndexPath *)indexPath {
    [tableView deselectRowAtIndexPath:indexPath animated:YES];
    BuiltinJIT *jit = BuiltinJIT.shared;
    if (indexPath.section == SectionStatus && indexPath.row == 1 && !jit.busy && !jit.translating) {
        [jit enable];
    } else if (indexPath.section == SectionStatus && indexPath.row == 3) {
        [self chooseCodeRegion:[tableView cellForRowAtIndexPath:indexPath]];
    } else if (indexPath.section == SectionPairing && OnDevicePairing.available && indexPath.row == 0) {
        [self pair];
    } else if (indexPath.section == SectionPairing) {
        UIDocumentPickerViewController *picker =
            [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[UTTypeItem] asCopy:YES];
        picker.delegate = self;
        [self presentViewController:picker animated:YES completion:nil];
    } else if (indexPath.section == SectionDiskImage && !jit.busy) {
        if (indexPath.row == 0)
            [jit prepareDevice];
        else
            [jit resetDeveloperDiskImage];
    }
}

#pragma mark Pairing

// An alert of the pairing's, in place of the one before: the instructions
// until the device connects, then the code, then how it ended
- (void)showPairingAlert:(UIAlertController *)alert {
    void (^present)(void) = ^{
        self.pairingAlert = alert;
        [self presentViewController:alert animated:YES completion:nil];
    };
    if (self.pairingAlert != nil && self.pairingAlert.presentingViewController != nil)
        [self.pairingAlert dismissViewControllerAnimated:NO completion:present];
    else
        present();
}

- (UIAlertController *)cancellableAlert:(NSString *)title message:(NSString *)message {
    UIAlertController *alert = [UIAlertController alertControllerWithTitle:title message:message
                                                            preferredStyle:UIAlertControllerStyleAlert];
    [alert addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:^(UIAlertAction *_) {
        [self.pairing cancel];
    }]];
    return alert;
}

- (void)pair {
    if (self.pairing != nil)
        return;
    self.pairing = [OnDevicePairing new];
    // The words the device shows: "Pair with iSH" here, "Pair with iSH" there
    UIAlertController *instructions = [self cancellableAlert:@"Pair with iSH"
                                                 message:@"On this device, open Settings › Privacy & Security › "
                                                          "Developer Mode, scroll down, tap \"Pair with iSH\", then come "
                                                          "back here for the code."];
    [instructions addAction:[UIAlertAction actionWithTitle:@"Open Settings" style:UIAlertActionStyleDefault
                                                   handler:^(UIAlertAction *_) {
        [UIApplication.sharedApplication openURL:[NSURL URLWithString:UIApplicationOpenSettingsURLString]
                                         options:@{} completionHandler:nil];
    }]];
    [self showPairingAlert:instructions];
    [self.pairing startWithPIN:^(NSString *pin) {
        [self showPairingAlert:[self cancellableAlert:@"Pairing Code"
                                          message:[NSString stringWithFormat:@"Enter this code on the device to finish "
                                                                              "pairing:\n\n%@", pin]]];
    } done:^(NSData *pairingFile, NSString *failure) {
        self.pairing = nil;
        NSString *message = nil;
        NSError *error = nil;
        if (pairingFile != nil)
            message = [BuiltinJIT.shared savePairingFile:pairingFile error:&error]
                ? @"The pairing file is saved: the JIT can be enabled."
                : [@"The pairing file was not saved: " stringByAppendingString:error.localizedDescription];
        else if (failure != nil)
            message = [@"Not paired: " stringByAppendingString:failure];
        if (message == nil) {
            [self.pairingAlert dismissViewControllerAnimated:YES completion:nil];
            self.pairingAlert = nil;
            return;
        }
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Pairing" message:message
                                                                 preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [self showPairingAlert:alert];
        [self.tableView reloadData];
    }];
}

- (void)chooseCodeRegion:(UITableViewCell *)cell {
    UIAlertController *sheet = [UIAlertController alertControllerWithTitle:@"Code Region"
                                                                   message:@"The room for translated code, and the "
                                                                            "memory it takes"
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    NSUInteger current = BuiltinJIT.codeRegionMB;
    for (NSNumber *choice in BuiltinJIT.codeRegionChoices) {
        NSUInteger mb = choice.unsignedIntegerValue;
        NSString *title = [NSString stringWithFormat:@"%lu MB%@", (unsigned long) mb, mb == current ? @" ✓" : @""];
        [sheet addAction:[UIAlertAction actionWithTitle:title style:UIAlertActionStyleDefault
                                                handler:^(UIAlertAction *_) {
            BuiltinJIT.codeRegionMB = mb;
            [self.tableView reloadData];
        }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Cancel" style:UIAlertActionStyleCancel handler:nil]];
    sheet.popoverPresentationController.sourceView = cell ?: self.tableView;
    sheet.popoverPresentationController.sourceRect = cell ? cell.bounds : self.tableView.bounds;
    [self presentViewController:sheet animated:YES completion:nil];
}

- (void)enabledAtStartChanged:(UISwitch *)toggle {
    BuiltinJIT.shared.enabledAtStart = toggle.on;
}

- (void)documentPicker:(UIDocumentPickerViewController *)controller didPickDocumentsAtURLs:(NSArray<NSURL *> *)urls {
    NSError *error = nil;
    if (![BuiltinJIT.shared importPairingFileFromURL:urls.firstObject error:&error]) {
        UIAlertController *alert = [UIAlertController alertControllerWithTitle:@"Import Failed"
                                                                       message:error.localizedDescription
                                                                preferredStyle:UIAlertControllerStyleAlert];
        [alert addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
        [self presentViewController:alert animated:YES completion:nil];
    }
    [self.tableView reloadData];
}

@end
