#include "apps.h"
#include "process.h"
#include "desktop.h"
void apps_init(void){process_catalog_init();}
const char *apps_title(AppId app){return process_app_title(app);}
void apps_draw(AppId app,UiRect area,bool focused,const UiTheme *theme){(void)focused;(void)theme;process_app_draw(app,area);}
bool apps_animate(AppId app){(void)app;return false;}
void apps_tick(AppId app){(void)app;}
void apps_key(AppId app,int key){struct nova_event e={.type=NOVA_EV_KEY,.a=key};process_app_event(app,&e);}
void apps_key_up(AppId app,int key){struct nova_event e={.type=NOVA_EV_KEY_UP,.a=key};process_app_event(app,&e);}
void apps_click(AppId app,int x,int y,int width,int height){struct nova_event e={.type=NOVA_EV_CLICK,.a=x,.b=y,.c=width,.d=height};process_app_event(app,&e);}
void apps_pointer(AppId app,int x,int y,int width,int height,bool down){(void)width;(void)height;struct nova_event e={.type=NOVA_EV_POINTER,.a=x,.b=y,.c=down};process_app_event(app,&e);}
