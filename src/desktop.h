#ifndef NOVA_DESKTOP_H
#define NOVA_DESKTOP_H
#include "ui.h"
#include "input.h"
void desktop_init(void);
void desktop_event(const InputEvent *event);
void desktop_render(void);
void desktop_mark_dirty(void);
void desktop_open(AppId app);
void desktop_set_dark(bool dark);
void desktop_set_accent(uint32_t accent);
void desktop_set_snow(bool snow);
bool desktop_snow(void);
const UiTheme *desktop_theme(void);
/* Procedural wallpapers: Aurora (default), Alpine, Skyline, Sunset, Ocean,
 * and Galaxy. Settings offers them as a picker and as the W key. */
int desktop_wallpaper(void);
int desktop_wallpaper_count(void);
const char *desktop_wallpaper_name(int wallpaper);
void desktop_set_wallpaper(int wallpaper);
/* macOS-style desktop widgets and the smaller display preferences. */
bool desktop_clock_widget(void);
void desktop_set_clock_widget(bool visible);
bool desktop_battery_widget(void);
void desktop_set_battery_widget(bool visible);
bool desktop_icons_visible(void);
void desktop_set_icons(bool visible);
bool desktop_clock_24h(void);
void desktop_set_clock_24h(bool use_24h);
bool desktop_process_focused(AppId app);
bool desktop_process_visible(AppId app);
void desktop_process_exited(AppId app);
void desktop_external_rectangle(AppId app,int width,int height);
#endif
