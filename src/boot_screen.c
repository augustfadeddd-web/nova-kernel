#include "boot_screen.h"
#include "graphics.h"

void boot_screen(unsigned int progress, const char *status)
{
    (void)status; /* Keep status in the boot sequence, artwork stays minimal. */
    int width = (int)gfx_width(), height = (int)gfx_height();
    gfx_rect(0, 0, width, height, 0x000000);
    int art_height = height * 72 / 100;
    int art_width = art_height * 4 / 3;
    if (art_width > width) { art_width = width; art_height = width * 3 / 4; }
    gfx_mask_scaled((width - art_width) / 2, (height - art_height) / 2 - height / 25,
                    art_width, art_height, nova_logo_aa, 768, 576, 0xFFFFFF, 0x000000);
    int bar_width = width / 5, bar_height = 4, bar_y = height - height / 14;
    int bar_x = (width - bar_width) / 2;
    gfx_round_rect(bar_x, bar_y, bar_width, bar_height, 2, 0x242424);
    if (progress > 100) progress = 100;
    int filled = (int)(bar_width * progress / 100);
    if (filled) gfx_round_rect(bar_x, bar_y, filled, bar_height, 2, 0xEEEEEE);
    gfx_present();
}
