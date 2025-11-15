#ifndef SYSMON_H_
#define SYSMON_H_

/* Prints a waybar json string
 * Type:
 * 0 - CPU,
 * 1 - Memory/RAM
 * 2 - Network
 * 3 - GPU (TBA) */
void system_monitor(int type);

#endif // !SYSMON_H_
