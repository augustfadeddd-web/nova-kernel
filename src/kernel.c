#include <stdint.h>
#include "graphics.h"
#include "terminal.h"
#include "system.h"
#include "input.h"
#include "apps.h"
#include "desktop.h"
#include "boot_screen.h"
#include "network.h"

#include "fat32.h"
#include "login.h"
#include "process.h"

static void log_number(uint32_t number)
{
    char reversed[12], text[12];
    unsigned int count = 0, written = 0;
    do { reversed[count++] = '0' + number % 10; number /= 10; } while (number);
    while (count) text[written++] = reversed[--count];
    text[written] = 0;
    terminal_log(text);
}

static void boot_progress(unsigned int from, unsigned int to, const char *status)
{
    for (unsigned int progress = from; progress <= to; progress += 4) {
        boot_screen(progress, status);
        system_wait(65);
    }
}

void kernel_main(uint32_t boot_magic, uint32_t multiboot_info)
{
    /* The bootloader leaves the FPU in an unknown state; DOOM's fixed-point
     * math runs on x87, so start from a clean one. */
    __asm__ volatile ("fninit");
    terminal_serial_init();
    if (boot_magic != 0x36D76289) {
        terminal_log("BOOT_ERROR: A Multiboot2 bootloader is required.\n");
        return;
    }
    if (!gfx_init(multiboot_info)) {
        terminal_log("GRAPHICS_ERROR: A 24/32-bit RGB framebuffer is required.\n");
        return;
    }
    system_init(multiboot_info);
    process_init(multiboot_info);
    boot_screen(4, "Starting your universe");
    terminal_log("GRAPHICS_OK: "); log_number(gfx_width()); terminal_log("x");
    log_number(gfx_height()); terminal_log("\nSPLASH_READY: Nova OS\n");
    boot_progress(4, 28, "Preparing graphics");
    input_init((int)gfx_width(), (int)gfx_height());
    network_init();
    boot_progress(32, 56, "Connecting devices and network");
    /* Persistent storage first, so the login greeter and the desktop can
     * read/write the disk (settings, notes, DOOM saves).  A diskless boot
     * still works: the file layer falls back to RAM. */
    if (!fat32_mount())
        terminal_log("STORAGE_MISSING: running without persistent storage\n");
    boot_progress(56, 68, "Preparing storage");
    /* SDDM-style greeter before the session starts. */
    login_run();
    boot_screen(72, "Welcome back");
    apps_init();
    boot_progress(76, 88, "Starting applications");
    desktop_init();
    boot_progress(88, 100, "Welcome to Nova OS");
    system_wait(150);
    desktop_render();
    terminal_log("BOOT_OK: Nova OS desktop ready.\nDESKTOP_READY\n");

    unsigned int idle = 0;
    for (;;) {
        process_tick();
        network_poll();

        InputEvent event;
        for (unsigned int count = 0; count < 64 && input_poll(&event); ++count)
            desktop_event(&event);
        if (++idle == 512) {
            idle = 0;
            if (system_tick()) desktop_mark_dirty();
        }
        desktop_render();
        __asm__ volatile ("pause");
    }
}
