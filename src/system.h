#ifndef NOVA_SYSTEM_H
#define NOVA_SYSTEM_H
#include <stdbool.h>
#include <stdint.h>
void system_init(uint32_t multiboot_info_address);
const char *system_cpu(void);
uint32_t system_memory_mib(void);
uint32_t system_uptime(void);
bool system_tick(void);
void system_time(char buffer[6]);
/* Weekday, day, month and year read from the CMOS real-time clock. */
void system_date(char buffer[24]);
/* The guest reports no battery, so the widget shows a simulated cell that
 * slowly discharges with uptime; 100 at boot down to a floor of 20 percent. */
uint32_t system_battery_percent(void);
const char *system_battery_state(void);
void system_wait(unsigned int milliseconds);
/* Milliseconds since the first call, from the TSC. The CPU frequency is
 * measured lazily on the first (40 ms) calibration wait, so this works even
 * before any timer is programmed; a forward jump over a second (VM stall)
 * re-bases the epoch instead of jumping the clock. */
uint32_t system_ms(void);
#endif
