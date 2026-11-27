//
//  DeviceStatus.h
//  iSH
//

// Starts keeping what /proc/ish/network and /proc/ish/power say, as iOS says
// it changes: on the main thread, as the app starts
void DeviceStatusStart(void);
// Their get_network_report and get_power_report (fs/proc/ish.h)
char *device_network_report(void);
char *device_power_report(void);
