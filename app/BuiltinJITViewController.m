//
//  BuiltinJITViewController.m
//  iSH
//

#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import "BuiltinJIT.h"
#import "BuiltinJITViewController.h"

enum {
    SectionStatus,
    SectionPairing,
    SectionDiskImage,
    SectionLog,
    SectionCount,
};

@interface BuiltinJITViewController () <UIDocumentPickerDelegate>
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
    case SectionStatus: return 3;
    case SectionPairing: return 1;
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
                "(LocalDevVPN): iOS turns off another VPN while it runs.";
    case SectionPairing:
        return @"The device's pairing file, made once on a computer with idevice_pair (or another pairing tool), "
                "imported here or copied to iSH's StikJIT folder with Finder.";
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
        UITableViewCell *cell = [self cellWithText:@"Import Pairing File…"
                                            detail:jit.hasPairingFile ? @"Imported" : @"Not imported"];
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
