#ifndef NOVA_DOOM_NOVA_H
#define NOVA_DOOM_NOVA_H

#include <stdbool.h>
#include "ui.h"

/* Synthetic Nova key code for the mouse's left button held over the DOOM
 * view; the bridge forwards it as DOOM's fire key. */
#define DOOM_NOVA_FIRE 0x154

/* Installs the exit hook that turns DOOM's exit()/I_Error into a longjmp. */
void doom_nova_init(void);
/* Advances one desktop frame of the game.  False once the game has quit or
 * aborted; the window then keeps showing the last rendered frame. */
bool doom_nova_frame(void);
/* Copies the 960x600 game frame into the window's content area. */
void doom_nova_draw(UiRect area);
/* Nova key event (press or release) on its way into DOOM. */
void doom_nova_key(int key, bool pressed);
/* True between a successful Create and the game's own exit(). */
bool doom_nova_ready(void);

#endif
