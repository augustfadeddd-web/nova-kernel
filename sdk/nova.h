#ifndef NOVA_SDK_H
#define NOVA_SDK_H
#include <stdint.h>
#include <stddef.h>
#define NOVA_ABI 1
#define NOVA_CAP_CONSOLE 1u
#define NOVA_CAP_STORAGE 2u
#define NOVA_CAP_GUI 4u
#define NOVA_CAP_NETWORK 8u
#define NOVA_CAP_FILES 16u
#define NOVA_CAP_SETTINGS 32u
#define NOVA_CAP_SHELL 64u
enum { NOVA_WRITE=1,NOVA_EXIT,NOVA_YIELD,NOVA_SLEEP,NOVA_TIME,NOVA_PID,
       NOVA_READFILE,NOVA_WRITEFILE,NOVA_ALLOC, NOVA_GUI_OPEN, NOVA_GUI_DRAW, NOVA_GUI_PRESENT,
       NOVA_GUI_EVENT, NOVA_GUI_BLIT, NOVA_INFO, NOVA_FS, NOVA_DESKTOP,
       NOVA_NET, NOVA_TERM, NOVA_COMMAND, NOVA_LOG };
/* ABI v1: int 0x80, rax=number; rdi/rsi/rdx=arguments. Negative returns are errors. */
static inline long nova_call(long n,long a,long b,long c) {
    __asm__ volatile("int $0x80" : "+a"(n) : "D"(a),"S"(b),"d"(c) : "memory","cc");
    return n;
}
static inline long nova_write(const char *s,size_t n) { return nova_call(NOVA_WRITE,(long)s,n,0); }
static inline long nova_puts(const char *s) { size_t n=0;while(s[n])++n;return nova_write(s,n); }
static inline __attribute__((noreturn)) void nova_exit(int code) { nova_call(NOVA_EXIT,code,0,0);for(;;){} }
static inline void nova_yield(void) { nova_call(NOVA_YIELD,0,0,0); }
static inline void nova_sleep(unsigned ms) { nova_call(NOVA_SLEEP,ms,0,0); }
static inline long nova_time(void) { return nova_call(NOVA_TIME,0,0,0); }
static inline long nova_pid(void) { return nova_call(NOVA_PID,0,0,0); }
static inline long nova_readfile(const char *name,void *buf,size_t n) {return nova_call(NOVA_READFILE,(long)name,(long)buf,n);}
static inline long nova_writefile(const char *name,const void *buf,size_t n) {return nova_call(NOVA_WRITEFILE,(long)name,(long)buf,n);}
/* Page-granular zeroed allocation, reclaimed on exit; no free in ABI v1. */
static inline void *nova_alloc(size_t n) {long r=nova_call(NOVA_ALLOC,n,0,0);return r<0?NULL:(void *)r;}

/* GUI events are window-local; the kernel owns the frame, focus and stacking. */
enum { NOVA_EV_REDRAW=1,NOVA_EV_KEY,NOVA_EV_KEY_UP,NOVA_EV_CLICK,NOVA_EV_POINTER,
       NOVA_EV_CLOSE,NOVA_EV_DOCUMENT,NOVA_EV_FOCUS };
struct nova_event {int32_t type,a,b,c,d;char text[128];};
enum {NOVA_DRAW_RECT=1,NOVA_DRAW_LINE,NOVA_DRAW_CIRCLE,NOVA_DRAW_TRIANGLE,
      NOVA_DRAW_ROUND,NOVA_DRAW_TEXT,NOVA_DRAW_CONSOLE};
struct nova_draw {int32_t op,x,y,w,h,a,b;uint32_t fg,bg;char text[96];};
struct nova_window {int32_t width,height;char title[48];};
struct nova_blit {int32_t x,y,width,height;uint64_t pixels;};
struct nova_info {
 uint32_t width,height,memory,uptime,ms,rx,tx,netflags,generation,busy,status,length;
 int32_t app,width_client,height_client,focused,visible;
 uint32_t theme[8],wallpaper,preferences;
 char cpu[64],time[8],date[32],ip[24],gateway[24],dns[24],netstatus[64],error[160],redirect[512];
};
enum {NOVA_FS_OPEN=1,NOVA_FS_CLOSE,NOVA_FS_READ,NOVA_FS_WRITE,NOVA_FS_SEEK,NOVA_FS_TELL,
      NOVA_FS_LENGTH,NOVA_FS_EXISTS,NOVA_FS_MKDIR,NOVA_FS_REMOVE,NOVA_FS_RENAME,NOVA_FS_LIST,NOVA_FS_USAGE};
struct nova_fs {int32_t op,handle,flags,whence;int64_t offset;uint64_t buffer,length;char path[128],other[128];};
enum {NOVA_DESK_OPEN=1,NOVA_DESK_DARK,NOVA_DESK_ACCENT,NOVA_DESK_SNOW,NOVA_DESK_WALL,
      NOVA_DESK_CLOCK,NOVA_DESK_BATTERY,NOVA_DESK_ICONS,NOVA_DESK_24H,NOVA_DESK_PASSWORD,
      NOVA_DESK_DOCUMENT,NOVA_DESK_INSTALL,NOVA_DESK_EXEC};
static inline long nova_window_open(struct nova_window *w){return nova_call(NOVA_GUI_OPEN,(long)w,0,0);}
static inline long nova_draw_batch(const struct nova_draw *c,size_t n){return nova_call(NOVA_GUI_DRAW,(long)c,n,0);}
static inline long nova_present(void){return nova_call(NOVA_GUI_PRESENT,0,0,0);}
static inline long nova_event_poll(struct nova_event *e){return nova_call(NOVA_GUI_EVENT,(long)e,0,0);}
static inline long nova_blit_pixels(struct nova_blit *b){return nova_call(NOVA_GUI_BLIT,(long)b,0,0);}
#endif
