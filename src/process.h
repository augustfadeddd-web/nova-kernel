#ifndef NOVA_PROCESS_H
#define NOVA_PROCESS_H
#include <stdint.h>
#include <stdbool.h>
#include "ui.h"
#include "../sdk/nova.h"
void process_init(uint32_t multiboot);
void process_tick(void);
bool process_command(const char *command, const char *arguments);
void process_catalog_init(void);
int process_catalog_count(void);
const char *process_app_title(AppId app);
bool process_app_ensure(AppId app,int width,int height);
void process_app_draw(AppId app,UiRect area);
void process_app_event(AppId app,const struct nova_event *event);
void process_app_close(AppId app);
void process_app_resize(AppId app,int width,int height);
void process_app_focus(AppId app,bool focused);
void process_notify_all(void);
#endif
