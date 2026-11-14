#ifndef NOVA_APPS_H
#define NOVA_APPS_H
#include "ui.h"
void apps_init(void);
const char *apps_title(AppId app);
void apps_draw(AppId app, UiRect area, bool focused, const UiTheme *theme);
void apps_key(AppId app, int key);
void apps_key_up(AppId app, int key);
void apps_click(AppId app, int x, int y, int width, int height);
void apps_pointer(AppId app, int x, int y, int width, int height, bool down);
/* Animation support: the desktop advances one frame for apps that need it. */
void apps_tick(AppId app);
bool apps_animate(AppId app);
#endif
