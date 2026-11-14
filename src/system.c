#include "system.h"

static char cpu[49];
static uint32_t memory, uptime;
static unsigned int last_second;
static unsigned int cached_hour, cached_minute, cached_second;

static void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d)
{
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d)
                      : "a"(leaf), "c"(0));
}

static uint8_t cmos(uint8_t address)
{
    outb(0x70, address | 0x80);
    return inb(0x71);
}

static uint8_t decimal(uint8_t value)
{
    return (value & 15) + (value >> 4) * 10;
}

static void copy(char *to, const char *from)
{
    do { *to++ = *from; } while (*from++);
}

static bool rtc(unsigned int *hours, unsigned int *minutes, unsigned int *seconds)
{
    uint8_t hour = 0, minute = 0, second = 0, format = 0;
    bool valid = false;
    for (unsigned int attempt = 0; attempt < 8; ++attempt) {
        if (cmos(0x0A) & 0x80) continue;
        second = cmos(0);
        minute = cmos(2);
        hour = cmos(4);
        format = cmos(0x0B);
        if (second == cmos(0) && !(cmos(0x0A) & 0x80)) { valid = true; break; }
    }
    if (!valid) return false;
    bool afternoon = (hour & 0x80) != 0;
    hour &= 0x7F;
    if (!(format & 4)) {
        second = decimal(second);
        minute = decimal(minute);
        hour = decimal(hour);
    }
    if (!(format & 2)) hour = hour % 12 + (afternoon ? 12 : 0);
    if (hour >= 24 || minute >= 60 || second >= 60) return false;
    *hours = cached_hour = hour;
    *minutes = cached_minute = minute;
    *seconds = cached_second = second;
    return true;
}

void system_init(uint32_t multiboot_info_address)
{
    if (multiboot_info_address) {
        const uint8_t *tag = (const uint8_t *)(uintptr_t)multiboot_info_address + 8;
        uint32_t total_size = ((const uint32_t *)(uintptr_t)multiboot_info_address)[0];
        const uint8_t *end = (const uint8_t *)(uintptr_t)multiboot_info_address + total_size;
        /* Memory information tag (type 4): mem_lower/mem_upper in KiB. */
        while (tag + 8 <= end) {
            uint32_t type = ((const uint32_t *)tag)[0];
            uint32_t size = ((const uint32_t *)tag)[1];
            if (size < 8 || type == 0)
                break;
            if (type == 4 && size >= 16) {
                uint32_t mem_upper = ((const uint32_t *)tag)[3];
                memory = (mem_upper + 1024) / 1024;
                break;
            }
            if (type == 6 && size >= 16) {
                /* Memory map tag: take the highest end of an available
                 * (type 1) region as the installed RAM estimate. */
                uint32_t entry_size = ((const uint32_t *)tag)[2];
                uint64_t highest = 0;
                if (entry_size >= 24) {
                    const uint8_t *entry = tag + 16;
                    const uint8_t *map_end = tag + size;
                    while (entry + entry_size <= map_end) {
                        uint64_t base = 0, length = 0;
                        uint32_t kind = 0;
                        __builtin_memcpy(&base, entry, 8);
                        __builtin_memcpy(&length, entry + 8, 8);
                        __builtin_memcpy(&kind, entry + 16, 4);
                        if (kind == 1 && base + length > highest)
                            highest = base + length;
                        entry += entry_size;
                    }
                    if (highest)
                        memory = (uint32_t)(highest / (1024ULL * 1024ULL));
                    break;
                }
            }
            tag += (size + 7U) & ~7U;
        }
    }
    uint32_t a, b, c, d;
    cpuid(0x80000000, &a, &b, &c, &d);
    if (a >= 0x80000004) {
        uint32_t brand[12];
        for (unsigned int i = 0; i < 3; ++i)
            cpuid(0x80000002 + i, &brand[i * 4], &brand[i * 4 + 1],
                  &brand[i * 4 + 2], &brand[i * 4 + 3]);
        const char *text = (const char *)brand;
        unsigned int start = 0, end = 48;
        while (start < end && text[start] == ' ') ++start;
        while (end > start && (text[end - 1] == 0 || text[end - 1] == ' ')) --end;
        for (unsigned int i = start; i < end; ++i) cpu[i - start] = text[i];
        cpu[end - start] = 0;
    } else {
        cpuid(0, &a, &b, &c, &d);
        uint32_t vendor[3] = { b, d, c };
        for (unsigned int i = 0; i < 12; ++i) cpu[i] = ((char *)vendor)[i];
        cpu[12] = 0;
    }
    unsigned int hours, minutes;
    if (!rtc(&hours, &minutes, &last_second)) last_second = 60;
    uptime = 0;
}

const char *system_cpu(void) { return cpu; }
uint32_t system_memory_mib(void) { return memory; }
uint32_t system_uptime(void) { return uptime; }

bool system_tick(void)
{
    unsigned int hours, minutes, second;
    if (!rtc(&hours, &minutes, &second)) return false;
    if (last_second >= 60) { last_second = second; return true; }
    if (second == last_second) return false;
    uptime += (second + 60 - last_second) % 60;
    last_second = second;
    return true;
}

void system_time(char buffer[6])
{
    unsigned int hours = cached_hour, minutes = cached_minute, seconds = cached_second;
    (void)rtc(&hours, &minutes, &seconds);
    buffer[0] = '0' + hours / 10;
    buffer[1] = '0' + hours % 10;
    buffer[2] = ':';
    buffer[3] = '0' + minutes / 10;
    buffer[4] = '0' + minutes % 10;
    buffer[5] = 0;
}

static const char *const weekday_names[7] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};
static const char *const month_names[12] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static bool leap_year(unsigned int year)
{
    return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

/* Whole days from 1970-01-01 (a Thursday) to the given calendar date. */
static unsigned int days_since_epoch(unsigned int year, unsigned int month,
                                     unsigned int day)
{
    static const unsigned int month_days[12] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    unsigned int days = (year - 1970) * 365;
    for (unsigned int counted = 1970; counted < year; ++counted)
        if (leap_year(counted)) ++days;
    for (unsigned int passed = 1; passed < month; ++passed) days += month_days[passed - 1];
    if (month > 2 && leap_year(year)) ++days;
    return days + day - 1;
}

void system_date(char buffer[24])
{
    unsigned int day = decimal(cmos(0x07));
    unsigned int month = decimal(cmos(0x08));
    unsigned int year = 2000 + decimal(cmos(0x09));
    if (month < 1 || month > 12 || day < 1 || day > 31 || year < 1970 || year > 2099) {
        copy(buffer, "Mon 1 Jan 1970");
        return;
    }
    unsigned int weekday = (days_since_epoch(year, month, day) + 4) % 7;
    copy(buffer, weekday_names[weekday]);
    unsigned int length = 3;
    buffer[length++] = ' ';
    if (day >= 10) buffer[length++] = (char)('0' + day / 10);
    buffer[length++] = (char)('0' + day % 10);
    buffer[length++] = ' ';
    copy(buffer + length, month_names[month - 1]);
    length += 3;
    buffer[length++] = ' ';
    buffer[length++] = (char)('0' + year / 1000);
    buffer[length++] = (char)('0' + year / 100 % 10);
    buffer[length++] = (char)('0' + year / 10 % 10);
    buffer[length++] = (char)('0' + year % 10);
    buffer[length] = 0;
}

uint32_t system_battery_percent(void)
{
    uint32_t drained = uptime / 20; /* one percent every twenty seconds */
    if (drained > 80) drained = 80;
    return 100 - drained;
}

const char *system_battery_state(void)
{
    return system_battery_percent() <= 30 ? "Low battery" : "On battery";
}

/* PIT channel 2 supplies short boot-animation waits without enabling IRQs. */
void system_wait(unsigned int milliseconds)
{
    while (milliseconds) {
        unsigned int interval = milliseconds > 40 ? 40 : milliseconds;
        uint16_t count = (uint16_t)(1193182U * interval / 1000U);
        uint8_t previous = inb(0x61);
        outb(0x61, previous & 0xFC);
        outb(0x43, 0xB0);
        outb(0x42, (uint8_t)count);
        outb(0x42, (uint8_t)(count >> 8));
        outb(0x61, (previous & 0xFC) | 1);
        for (unsigned int guard = 0; guard < 2000000; ++guard) {
            if (inb(0x61) & 0x20) break;
            __asm__ volatile ("pause");
        }
        outb(0x61, previous);
        milliseconds -= interval;
    }
}

/* ---------------------------------------------------------------- timing -- */

static uint32_t ticks_per_ms; /* lazy: measured on the first system_ms() */
static uint64_t last_tsc;
static uint32_t elapsed_ms;

static uint64_t read_tsc(void)
{
    uint32_t low, high;
    __asm__ volatile ("rdtsc" : "=a"(low), "=d"(high));
    return ((uint64_t)high << 32) | low;
}

uint32_t system_ms(void)
{
    uint64_t now = read_tsc();
    uint64_t ticks;
    uint32_t delta;

    if (!ticks_per_ms) {
        uint64_t start = now;
        system_wait(40);
        now = read_tsc();
        ticks_per_ms = (uint32_t)((now - start) / 40u);
        if (!ticks_per_ms) ticks_per_ms = 1;
        last_tsc = start;
        elapsed_ms = 0;
        return 0;
    }

    ticks = now - last_tsc;
    /* Anything past a second means the VM paused (screendump, debugger):
     * clamp instead of letting the clock leap forward. */
    if (ticks > (uint64_t)ticks_per_ms * 1000u)
        delta = 1001;
    else
        delta = (uint32_t)ticks / ticks_per_ms;
    if (delta > 1000) { last_tsc = now; return elapsed_ms; }
    last_tsc += (uint64_t)delta * ticks_per_ms;
    elapsed_ms += delta;
    return elapsed_ms;
}
