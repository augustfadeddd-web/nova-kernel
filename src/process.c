/* Nova application platform v1: static ELF64, ring 3, per-process paging,
 * PIT-preempted round robin, bounded syscall copying and local packages. */
#include "process.h"
#include "pages.h"
#include "fat32.h"
#include "terminal.h"
#include "system.h"
#include "desktop.h"
#include "graphics.h"
#include "apps.h"
#include "network.h"
#include "login.h"
#include "../sdk/nova.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#define BASE 0x40000000ULL
#define USER_SIZE (64u*1024u*1024u)
#define TABLE_COUNT (3+USER_SIZE/(2u*1024u*1024u))
#define IMAGE_SIZE (16u*1024u*1024u)
#define HEAP_START (BASE+IMAGE_SIZE)
#define HEAP_END (BASE+60u*1024u*1024u)
#define STACK_LOW (BASE+USER_SIZE-524288)
#define MAX_TASKS 24
#define MAX_ELF (8u*1024u*1024u)
#define NX (1ULL<<63)
#define PTE_ADDR 0x000FFFFFFFFFF000ULL
struct frame { uint64_t r15,r14,r13,r12,r11,r10,r9,r8,rdi,rsi,rbp,rdx,rcx,rbx,rax;
    uint64_t vector,error,rip,cs,flags,rsp,ss; };
_Static_assert(sizeof(struct frame)==176,"assembly frame mismatch");
enum { UNUSED,READY,SLEEPING,DEAD };
struct task {
    struct frame frame;
    uint8_t fx[512] __attribute__((aligned(16)));
    uint64_t tables[TABLE_COUNT], mappings[USER_SIZE/4096], heap;
    uint32_t pid, state, wake, caps, quanta;
    int status;
    char name[9];
    int app, width, height, front_width, front_height;
    uint32_t *front,*back;
    struct nova_event events[64];
    unsigned event_read,event_write;
    void *files[16];
    uint32_t close_deadline;
    char document[128];
};
static struct task tasks[MAX_TASKS];
static struct task *current;
static uint32_t next_pid=1, next_slot;
static bool ready;
static int launch_slot = -1;
static int network_owner = -1;
static uint32_t surface_bytes;
static void gui_destroy(struct task *t);
static long integrated_call(struct task *t);
static void catalog_refresh(void);
static int launch(const char *arg);
static int launch_gui(const char *name);
static int install(const char *path);

uint64_t kernel_cr3;
static uint8_t trap_stack[65536] __attribute__((aligned(16)));
static uint8_t double_fault_stack[16384] __attribute__((aligned(16)));
struct tss64 { uint32_t reserved0; uint64_t rsp[3],reserved1,ist[7],reserved2;uint16_t reserved3,iomap; } __attribute__((packed));
static struct tss64 tss;
static uint64_t gdt[7] __attribute__((aligned(16)));
struct descriptor { uint16_t limit;uint64_t base; } __attribute__((packed));
struct gate { uint16_t low,selector;uint8_t ist,flags;uint16_t middle;uint32_t high,reserved; } __attribute__((packed));
static struct gate idt[256];
extern void *exception_stubs[];
extern void trap_128(void);
extern void arch_user_run(struct frame *,uint64_t,void *);
static void port(uint16_t p,uint8_t v){__asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p));}
static uint8_t port_read(uint16_t p){uint8_t v;__asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p));return v;}
static void quantum_start(void) {
    /* One-shot quantum. Discard a previously pending timer request so slow
       desktop frames cannot starve a process before its first instruction. */
    port(0x43,0x30);port(0x40,11932&255);port(0x40,11932>>8);
    port(0x20,0x0c);if(port_read(0x20)&0x80)port(0x20,0x20);
    port(0x20,0x0a);
}
static void gate_set(unsigned i,uintptr_t address,unsigned flags,unsigned ist) {
    idt[i]=(struct gate){address,8,ist,flags,address>>16,address>>32,0};
}
static void report(const char *kind,struct task *t,int detail) {
    char text[128];snprintf(text,sizeof text,"%s: pid=%u name=%s value=%d\n",kind,t->pid,t->name,detail);
    terminal_write(text);desktop_mark_dirty();
}
static void destroy(struct task *t) {
    gui_destroy(t);
    for(unsigned i=0;i<USER_SIZE/4096;++i) if(t->mappings[i]) page_free(t->mappings[i]&PTE_ADDR);
    for(unsigned i=0;i<TABLE_COUNT;++i) if(t->tables[i]) page_free(t->tables[i]);
    memset(t,0,sizeof *t);
}
static bool address_space(struct task *t) {
    for(unsigned i=0;i<TABLE_COUNT;++i) {t->tables[i]=page_alloc();if(!t->tables[i])return false;}
    uint64_t *kernel=(uint64_t *)(uintptr_t)kernel_cr3;
    uint64_t *old_pdpt=(uint64_t *)(uintptr_t)(kernel[0]&PTE_ADDR);
    uint64_t *pml4=(uint64_t *)(uintptr_t)t->tables[0];
    uint64_t *pdpt=(uint64_t *)(uintptr_t)t->tables[1];
    uint64_t *pd=(uint64_t *)(uintptr_t)t->tables[2];
    memcpy(pml4,kernel,4096);memcpy(pdpt,old_pdpt,4096);
    memcpy(pd,(void *)(uintptr_t)(old_pdpt[1]&PTE_ADDR),4096);
    pml4[0]=t->tables[1]|7;pdpt[1]=t->tables[2]|7;
    for(unsigned i=0;i<USER_SIZE/(2u*1024u*1024u);++i) pd[i]=t->tables[3+i]|7;
    return true;
}
static bool map_page(struct task *t,uint64_t va,bool writable,bool executable) {
    if(va<BASE || va>=BASE+USER_SIZE || (va&4095))return false;
    unsigned i=(va-BASE)/4096;
    if(t->mappings[i])return false;
    uint64_t page=page_alloc();if(!page)return false;
    uint64_t entry=page|5|(writable?2:0)|(executable?0:NX);
    t->mappings[i]=entry;
    ((uint64_t *)(uintptr_t)t->tables[3+i/512])[i%512]=entry;
    return true;
}
static bool user_range(struct task *t,uint64_t va,uint64_t n,bool write) {
    if(va<BASE || va>=BASE+USER_SIZE || n>BASE+USER_SIZE-va)return false;
    if(!n)return true;
    unsigned first=(va-BASE)/4096,last=(va+n-1-BASE)/4096;
    for(unsigned i=first;i<=last;++i) if(!t->mappings[i] || (write && !(t->mappings[i]&2)))return false;
    return true;
}
/* Copy through supervisor physical aliases; never dereference user pointers. */
static bool user_copy(struct task *t,uint64_t va,void *buffer,size_t n,bool to_user) {
    if(!user_range(t,va,n,to_user))return false;
    uint8_t *b=buffer;
    while(n){unsigned i=(va-BASE)/4096,offset=va&4095;size_t take=4096-offset;if(take>n)take=n;
        uint8_t *physical=(uint8_t *)(uintptr_t)(t->mappings[i]&PTE_ADDR)+offset;
        if(to_user)memcpy(physical,b,take);else memcpy(b,physical,take);
        n-=take;b+=take;va+=take;
    }return true;
}
struct elf_header {uint8_t ident[16];uint16_t type,machine;uint32_t version;uint64_t entry,phoff,shoff;uint32_t flags;uint16_t ehsize,phentsize,phnum,shentsize,shnum,shstrndx;};
struct program_header {uint32_t type,flags;uint64_t offset,vaddr,paddr,filesz,memsz,align;};
_Static_assert(sizeof(struct elf_header)==64 && sizeof(struct program_header)==56,"ELF ABI");
static bool elf_valid(const uint8_t *data,size_t size) {
    if(size<64 || size>MAX_ELF)return false;
    const struct elf_header *e=(const void *)data;
    if(memcmp(e->ident,"\177ELF\2\1\1",7) || e->type!=2 || e->machine!=62 || e->version!=1 ||
       e->ehsize!=64 || e->phentsize!=56 || !e->phnum || e->phnum>32 || e->phoff>size ||
       (uint64_t)e->phnum*56>size-e->phoff || (e->phoff&7))return false;
    const struct program_header *ph=(const void *)(data+e->phoff);bool entry=false;
    for(unsigned i=0;i<e->phnum;++i){const struct program_header *h=ph+i;
        if(h->type==2 || h->type==3 || h->type==7)return false; /* no dynamic linker/TLS */
        if(h->type!=1 || !h->memsz)continue;
        if(!(h->flags&4) || (h->flags&~7u) || (h->flags&3)==3 || h->filesz>h->memsz ||
           h->offset>size || h->filesz>size-h->offset || h->vaddr<BASE || h->vaddr>=BASE+IMAGE_SIZE ||
           h->memsz>BASE+IMAGE_SIZE-h->vaddr)return false;
        if(h->align>1 && ((h->align&(h->align-1)) || h->align>0x200000 ||
           (h->vaddr&(h->align-1))!=(h->offset&(h->align-1))))return false;
        uint64_t first=h->vaddr&~4095ULL,last=(h->vaddr+h->memsz+4095)&~4095ULL;
        for(unsigned j=0;j<i;++j)if(ph[j].type==1 && ph[j].memsz){
            uint64_t other=ph[j].vaddr&~4095ULL,end=(ph[j].vaddr+ph[j].memsz+4095)&~4095ULL;
            if(first<end && other<last)return false;
        }
        if((h->flags&1) && e->entry>=h->vaddr && e->entry-h->vaddr<h->filesz)entry=true;
    }return entry;
}
static uint8_t *read_file(const char *path,size_t *size,size_t maximum) {
    void *f=fat32_open(path,"rb");if(!f)return NULL;
    *size=fat32_length(f);if(!*size || *size>maximum){fat32_close(f);return NULL;}
    uint8_t *data=malloc(*size);
    if(data && fat32_read(f,data,1,*size)!=*size){free(data);data=NULL;}
    fat32_close(f);return data;
}
static int spawn(const char *path,const char *name,uint32_t caps) {
    if(!ready)return -1;
    unsigned count=0;for(unsigned i=0;i<MAX_TASKS;++i)if(tasks[i].state && tasks[i].app<0)++count;
    if(launch_slot<0 && count>=8)return -2;
    struct task *t=NULL;for(unsigned i=0;i<MAX_TASKS;++i)if(!tasks[i].state){t=tasks+i;break;}
    if(!t)return -2;
    size_t size=0;uint8_t *data=read_file(path,&size,MAX_ELF);
    if(!data || !elf_valid(data,size)){free(data);return -3;}
    memset(t,0,sizeof *t);t->app=launch_slot;
    if(!address_space(t))goto fail;
    const struct elf_header *e=(const void *)data;
    const struct program_header *ph=(const void *)(data+e->phoff);
    for(unsigned i=0;i<e->phnum;++i)if(ph[i].type==1 && ph[i].memsz){
        for(uint64_t v=ph[i].vaddr&~4095ULL;v<ph[i].vaddr+ph[i].memsz;v+=4096)
            if(!map_page(t,v,(ph[i].flags&2)!=0,(ph[i].flags&1)!=0))goto fail;
        uint64_t v=ph[i].vaddr,remaining=ph[i].filesz,off=ph[i].offset;
        while(remaining){unsigned index=(v-BASE)/4096,offset=v&4095;size_t n=4096-offset;if(n>remaining)n=remaining;
            memcpy((void *)(uintptr_t)((t->mappings[index]&PTE_ADDR)+offset),data+off,n);v+=n;off+=n;remaining-=n;}
    }
    for(uint64_t v=STACK_LOW;v<BASE+USER_SIZE;v+=4096)if(!map_page(t,v,true,false))goto fail;
    t->frame.rdi=launch_slot<0?UINT64_MAX:(uint64_t)launch_slot;t->frame.rip=e->entry;t->frame.cs=0x23;t->frame.ss=0x1b;t->frame.flags=0x202;t->frame.rsp=BASE+USER_SIZE-16;
    /* Clean x87/SSE state; do not inherit kernel register contents. */
    *(uint16_t *)(t->fx)=0x037f;*(uint32_t *)(t->fx+24)=0x1f80;
    t->heap=HEAP_START;t->caps=caps;t->pid=next_pid++;if(!next_pid)next_pid=1;
    strncpy(t->name,name,8);t->name[8]=0;t->state=READY;
    free(data);report("PROCESS_START",t,3);return (int)t->pid;
fail:free(data);destroy(t);return -4;
}
static bool local_name(struct task *t,uint64_t pointer,char out[13]) {
    unsigned dots=0,base=0,ext=0;
    for(unsigned i=0;i<13;++i){char c;if(!user_copy(t,pointer+i,&c,1,false))return false;
        if(!c){out[i]=0;return base>0 && (!dots || ext>0);}
        if(c=='.'){if(dots++ || !base)return false;}
        else {if(!((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'))return false;
            if(dots){if(++ext>3)return false;}else if(++base>8)return false;}
        out[i]=c;
    }return false;
}
static long file_call(struct task *t,uint64_t name,uint64_t address,uint64_t n,bool write) {
    if(!(t->caps&NOVA_CAP_STORAGE))return -13;
    if(n>4096)return -22;
    if(!user_range(t,address,n,!write))return -14;
    char leaf[13],path[64],directory[40];if(!local_name(t,name,leaf))return -22;
    snprintf(directory,sizeof directory,"/APPDATA/%s",t->name);
    if(!fat32_mkdir(directory))return -5;
    snprintf(path,sizeof path,"%s/%s",directory,leaf);
    uint8_t buffer[4096];if(write && !user_copy(t,address,buffer,n,false))return -14;
    void *f=fat32_open(path,write?"wb":"rb");if(!f)return -2;
    size_t done=write?fat32_write(f,buffer,1,n):fat32_read(f,buffer,1,n);fat32_close(f);
    if(!write && !user_copy(t,address,buffer,done,true))return -14;
    return write && done!=n ? -5 : (long)done;
}
static long syscall(struct task *t) {
    uint64_t a=t->frame.rdi,b=t->frame.rsi,c=t->frame.rdx;
    switch(t->frame.rax){
    case NOVA_WRITE: {
        if(!(t->caps&NOVA_CAP_CONSOLE))return -13;
        if(b>4096)return -22;
        char text[4096];if(!user_copy(t,a,text,b,false))return -14;
        for(size_t i=0;i<b;++i) if(text[i]=='\n'||text[i]=='\t'||(text[i]>=32&&text[i]<=126))terminal_putc(text[i]);
        desktop_mark_dirty();return b;
    }
    case NOVA_EXIT:t->status=(int)a;t->state=DEAD;return 0;
    case NOVA_YIELD:return 0;
    case NOVA_SLEEP:if(a>60000)return -22;t->wake=system_ms()+(uint32_t)a;t->state=SLEEPING;return 0;
    case NOVA_TIME:return system_ms();
    case NOVA_PID:return t->pid;
    case NOVA_READFILE:return file_call(t,a,b,c,false);
    case NOVA_WRITEFILE:return file_call(t,a,b,c,true);
    case NOVA_ALLOC:{
        if(!a || a>HEAP_END-t->heap)return -12;
        uint64_t size=(a+4095)&~4095ULL,base=t->heap,offset=0;
        for(;offset<size;offset+=4096)if(!map_page(t,base+offset,true,false))break;
        if(offset<size){for(uint64_t o=0;o<offset;o+=4096){unsigned i=(base+o-BASE)/4096;page_free(t->mappings[i]&PTE_ADDR);t->mappings[i]=0;((uint64_t *)(uintptr_t)t->tables[3+i/512])[i%512]=0;}return -12;}
        t->heap+=size;return base;
    }
    default:return integrated_call(t);
    }
}
void process_trap(struct frame *frame,void *fx) {
    if((frame->cs&3)!=3 || !current){
        char message[128];snprintf(message,sizeof message,"KERNEL_PANIC: vector=%llu rip=%llx error=%llx\n",(unsigned long long)frame->vector,(unsigned long long)frame->rip,(unsigned long long)frame->error);terminal_log(message);
        for(;;)__asm__ volatile("cli; hlt");
    }
    current->frame=*frame;memcpy(current->fx,fx,512);
    if(frame->vector==32){port(0x20,0x20);++current->quanta;}
    else if(frame->vector==128)current->frame.rax=(uint64_t)syscall(current);
    else {report("PROCESS_FAULT",current,(int)frame->vector);current->status=128+(int)frame->vector;current->state=DEAD;}
}
void process_tick(void) {
    if(!ready)return;
    for(unsigned i=0;i<MAX_TASKS;++i){struct task *t=tasks+i;
        if(t->close_deadline && (int32_t)(system_ms()-t->close_deadline)>=0){t->state=DEAD;t->status=0;}
        if(t->state==SLEEPING && (int32_t)(system_ms()-t->wake)>=0)t->state=READY;
        if(t->state==DEAD){report("PROCESS_EXIT",t,t->status);destroy(t);}
    }
    for(unsigned n=0;n<MAX_TASKS;++n){unsigned i=(next_slot+n)%MAX_TASKS;struct task *t=tasks+i;
        if(t->state!=READY)continue;
        next_slot=(i+1)%MAX_TASKS;current=t;
        quantum_start();
        arch_user_run(&t->frame,t->tables[0],t->fx);
        current=NULL;break; /* Return to the desktop after every quantum/syscall. */
    }
}
void process_init(uint32_t multiboot) {
    pages_init(multiboot);
    __asm__ volatile("mov %%cr3,%0":"=r"(kernel_cr3));
    uint32_t a,b,c,d;__asm__ volatile("cpuid":"=a"(a),"=b"(b),"=c"(c),"=d"(d):"a"(0x80000001),"c"(0));
    if(!(d&(1u<<20)) || pages_available()<128){terminal_log("PROCESS_DISABLED: NX or RAM unavailable\n");return;}
    uint32_t low,high;__asm__ volatile("rdmsr":"=a"(low),"=d"(high):"c"(0xC0000080));
    low|=1u<<11;__asm__ volatile("wrmsr"::"a"(low),"d"(high),"c"(0xC0000080));
    uint64_t cr0;__asm__ volatile("mov %%cr0,%0":"=r"(cr0));cr0|=1u<<16;__asm__ volatile("mov %0,%%cr0"::"r"(cr0):"memory");
    gdt[0]=0;gdt[1]=0x00209A000000FFFFULL;gdt[2]=0x00CF92000000FFFFULL;
    gdt[3]=0x00CFF2000000FFFFULL;gdt[4]=0x0020FA000000FFFFULL;
    memset(&tss,0,sizeof tss);tss.rsp[0]=(uintptr_t)(trap_stack+sizeof trap_stack);
    tss.ist[0]=(uintptr_t)(double_fault_stack+sizeof double_fault_stack);tss.iomap=sizeof tss;
    uint64_t base=(uintptr_t)&tss,limit=sizeof tss-1;
    gdt[5]=(limit&0xffff)|((base&0xffffff)<<16)|(0x89ULL<<40)|(((limit>>16)&15)<<48)|(((base>>24)&255)<<56);
    gdt[6]=base>>32;
    struct descriptor gd={sizeof gdt-1,(uintptr_t)gdt},id={sizeof idt-1,(uintptr_t)idt};
    __asm__ volatile("lgdt %0"::"m"(gd):"memory");
    __asm__ volatile("mov $0x10,%%ax; mov %%ax,%%ss; mov %%ax,%%ds; mov %%ax,%%es; mov $0x28,%%ax; ltr %%ax":::"rax","memory");
    for(unsigned i=0;i<256;++i)gate_set(i,(uintptr_t)exception_stubs[31],0x8e,0);
    for(unsigned i=0;i<33;++i)gate_set(i,(uintptr_t)exception_stubs[i],0x8e,i==8?1:0);
    gate_set(128,(uintptr_t)trap_128,0xee,0);
    __asm__ volatile("lidt %0"::"m"(id):"memory");
    /* Remap PIC; kernel remains non-preemptible with IF clear. Only user mode
       enables IRQ0. Existing keyboard/network drivers continue to poll. */
    port(0x20,0x11);port(0xa0,0x11);port(0x21,0x20);port(0xa1,0x28);
    port(0x21,4);port(0xa1,2);port(0x21,1);port(0xa1,1);port(0x21,0xfe);port(0xa1,0xff);
    quantum_start();
    ready=true;terminal_log("PROCESS_READY: ring3 ELF64 NX PIT10MS ABI1\n");
}
/* .nova v1: fixed header + one static ELF. Checksum detects corruption;
 * it is not a signature or a claim of publisher trust. */
struct package {char magic[8],name[8];uint32_t version,abi,caps,length,checksum,reserved;};
_Static_assert(sizeof(struct package)==40,"package header");
static uint32_t checksum(const uint8_t *p,size_t n){uint32_t h=2166136261u;while(n--){h^=*p++;h*=16777619u;}return h;}
static bool app_name(const char *input,char out[9]) {
    size_t n=0;while(input[n] && n<9){char c=input[n];if(c>='a'&&c<='z')c-=32;
        if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_')||n==8)return false;out[n++]=c;}
    out[n]=0;return n>0;
}
static bool package_header(const struct package *h,char name[9]) {
    char raw[9];memcpy(raw,h->name,8);raw[8]=0;
    if(memcmp(h->magic,"NOVAPKG1",8)||h->abi!=1||h->caps&~127u||!h->version||h->reserved||!app_name(raw,name))return false;
    char canonical[8]={0};memcpy(canonical,name,strlen(name));return !memcmp(canonical,h->name,8);
}
static bool write_file(const char *path,const void *data,size_t length) {
    void *f=fat32_open(path,"wb");if(!f)return false;bool ok=fat32_write(f,data,1,length)==length;
    if(fat32_close(f)!=0)ok=false;if(!ok)fat32_remove(path);return ok;
}
static int install(const char *path) {
    size_t size=0;uint8_t *data=read_file(path,&size,MAX_ELF+40);if(!data)return -1;
    struct package *h=(void *)data;char name[9],elf[40],meta[40];int result=-2;
    if(size<40 || !package_header(h,name) || h->length!=size-40 || checksum(data+40,size-40)!=h->checksum || !elf_valid(data+40,size-40))goto out;
    snprintf(elf,sizeof elf,"/APPS/%s.ELF",name);snprintf(meta,sizeof meta,"/APPS/%s.MET",name);
    if(fat32_exists(elf)||fat32_exists(meta)){result=-3;goto out;}
    if(!fat32_mkdir("/APPS")||!write_file(elf,data+40,size-40)){result=-4;goto out;}
    if(!write_file(meta,h,40)){fat32_remove(elf);result=-4;goto out;}
    catalog_refresh();terminal_write("PACKAGE_INSTALLED: ");terminal_write(name);terminal_putc('\n');result=0;
out:free(data);return result;
}
static int launch(const char *arg) {
    if(*arg=='/'){
        if(!strncmp(arg,"/APPS/",6)){char name[9];const char *dot=strstr(arg+6,".ELF");if(dot && !dot[4] && dot-arg-6>0 && dot-arg-6<=8){memcpy(name,arg+6,dot-arg-6);name[dot-arg-6]=0;return launch(name);}}
        return spawn(arg,"RAW",NOVA_CAP_CONSOLE);
    }
    char name[9],elf[40],meta[40];if(!app_name(arg,name))return -1;
    snprintf(elf,sizeof elf,"/APPS/%s.ELF",name);snprintf(meta,sizeof meta,"/APPS/%s.MET",name);
    size_t n;uint8_t *data=read_file(meta,&n,40);char stored[9];
    if(!data||n!=40||!package_header((void *)data,stored)||strcmp(stored,name)){free(data);return -2;}
    struct package h;memcpy(&h,data,40);free(data);
    data=read_file(elf,&n,MAX_ELF);if(!data||n!=h.length||checksum(data,n)!=h.checksum){free(data);return -3;}free(data);
    if(launch_slot<0 && (h.caps&NOVA_CAP_GUI))return launch_gui(name);
    return spawn(elf,name,h.caps);
}
bool process_command(const char *command,const char *arg) {
    char line[160];int result=0;
    if(!strcmp(command,"run")){result=launch(arg);if(result>0)return true;}
    else if(!strcmp(command,"install")){result=install(arg);if(!result){catalog_refresh();return true;}}
    else if(!strcmp(command,"packages")){
        fat32_dirent entries[64];int n=fat32_list("/APPS",entries,64);terminal_write("Installed packages:\n");
        for(int i=0;i<n;++i){char *extension=strstr(entries[i].name,".MET");
            if(extension && !extension[4]){*extension=0;terminal_write(entries[i].name);terminal_putc('\n');}}return true;
    }
    else if(!strcmp(command,"remove")){
        char name[9],path[40];if(!app_name(arg,name))result=-1;
        else {for(unsigned i=0;i<MAX_TASKS;++i)if(tasks[i].state&&!strcmp(tasks[i].name,name)){terminal_write("Package is running; kill it first.\n");return true;}
            snprintf(path,sizeof path,"/APPS/%s.MET",name);result=fat32_remove(path);
            if(!result){snprintf(path,sizeof path,"/APPS/%s.ELF",name);result=fat32_remove(path);}
            if(!result){catalog_refresh();terminal_write("PACKAGE_REMOVED: ");terminal_write(name);terminal_putc('\n');return true;}}
    }
    else if(!strcmp(command,"ps")){
        terminal_write("PID NAME STATE TIMER_QUANTA\n");
        for(unsigned i=0;i<MAX_TASKS;++i)if(tasks[i].state){snprintf(line,sizeof line,"%u %s %s %u\n",tasks[i].pid,tasks[i].name,tasks[i].state==SLEEPING?"sleep":"ready",tasks[i].quanta);terminal_write(line);}return true;
    }
    else if(!strcmp(command,"kill")){
        char *end;unsigned long pid=strtoul(arg,&end,10);if(!*arg||*end||!pid||pid>UINT32_MAX)result=-1;
        else {result=-2;for(unsigned i=0;i<MAX_TASKS;++i)if(tasks[i].state&&tasks[i].pid==pid){tasks[i].state=DEAD;tasks[i].status=137;return true;}}
    }
    else if(!strcmp(command,"memcheck")) {
        void *a=malloc(128),*b=malloc(256);bool ok=a && b;
        if(a)memset(a,0x5a,128);
        void *large=malloc(SIZE_MAX),*overflow=calloc(SIZE_MAX,2);
        if(large || overflow)ok=false;
        free(large);free(overflow);
        void *grown=a?realloc(a,512):NULL;
        if(!grown)ok=false;else for(unsigned i=0;i<128;++i)if(((uint8_t *)grown)[i]!=0x5a)ok=false;
        if(grown)a=grown;
        free(a);free(a);free((void *)(uintptr_t)1);free(b);
        void *zero=calloc(64,8);if(!zero)ok=false;
        else for(unsigned i=0;i<512;++i)if(((uint8_t *)zero)[i])ok=false;
        free(zero);terminal_write(ok?"HEAP_CHECK: PASS\n":"HEAP_CHECK: FAIL\n");return true;
    }
    else if(!strcmp(command,"mem")){snprintf(line,sizeof line,"MEMORY: free_pages=%u page_size=4096\n",pages_available());terminal_write(line);return true;}
    else return false;
    snprintf(line,sizeof line,"APP_ERROR: %s result=%d\n",command,result);terminal_write(line);return true;
}

/* Desktop catalog: original slots remain stable, installed GUI apps occupy
 * additional slots. Program state and all app callbacks live in ring 3. */
static char catalog_id[APP_COUNT][9];
static char catalog_title[APP_COUNT][48];
static const char *const builtins[APP_BUILTIN_COUNT]={"TERMINAL","NOTES","CALC","FILES","SETTINGS","ABOUT","BROWSER","NETWORK","CLOCK","PAINT","SNAKE","TICTAC","MINES","SYSMON","RACER","DOOM"};
static const char *const builtin_titles[APP_BUILTIN_COUNT]={"Terminal","Notes","Calculator","Files","Settings","About Nova","Browser","Network","Clock","Paint","Snake","Tic-Tac-Toe","Minesweeper","System Monitor","Racer","DOOM"};
static struct task *app_task(AppId app){for(unsigned i=0;i<MAX_TASKS;++i)if(tasks[i].state && tasks[i].app==(int)app)return tasks+i;return NULL;}
static void catalog_refresh(void){
    for(int i=0;i<APP_BUILTIN_COUNT;++i){strcpy(catalog_id[i],builtins[i]);strcpy(catalog_title[i],builtin_titles[i]);}
    fat32_dirent entries[64];int n=fat32_list("/APPS",entries,64);
    for(int slot=APP_BUILTIN_COUNT;slot<APP_COUNT;++slot)if(!app_task((AppId)slot)){catalog_id[slot][0]=0;catalog_title[slot][0]=0;}
    for(int i=0;i<n;++i){char *ext=strstr(entries[i].name,".MET");if(!ext || ext[4])continue;*ext=0;
        bool found=false;for(int j=0;j<APP_COUNT;++j)if(!strcmp(catalog_id[j],entries[i].name))found=true;if(found)continue;
        char path[40];snprintf(path,sizeof path,"/APPS/%s.MET",entries[i].name);size_t size;uint8_t *data=read_file(path,&size,40);
        char name[9];bool gui=data && size==40 && package_header((void *)data,name) && (((struct package *)data)->caps&NOVA_CAP_GUI);free(data);if(!gui)continue;
        for(int j=APP_BUILTIN_COUNT;j<APP_COUNT;++j)if(!catalog_id[j][0]){strcpy(catalog_id[j],name);strcpy(catalog_title[j],name);break;}
    }
    desktop_mark_dirty();
}
static int launch_gui(const char *name){for(int i=0;i<APP_COUNT;++i)if(!strcmp(catalog_id[i],name)){desktop_open((AppId)i);struct task *t=app_task((AppId)i);return t?(int)t->pid:-12;}return -2;}
void process_catalog_init(void){catalog_refresh();}
int process_catalog_count(void){int n=APP_BUILTIN_COUNT;for(int i=APP_BUILTIN_COUNT;i<APP_COUNT;++i)if(catalog_id[i][0])n=i+1;return n;}
const char *process_app_title(AppId app){return app>=0 && app<APP_COUNT && catalog_title[app][0]?catalog_title[app]:"Application";}
static void enqueue(struct task *t,const struct nova_event *event){
    /* Coalesce repeated redraws and motion without dropping keys or close. */
    if(event->type==NOVA_EV_REDRAW || event->type==NOVA_EV_POINTER){
        for(unsigned i=t->event_read;i!=t->event_write;i=(i+1)%64)if(t->events[i].type==event->type){t->events[i]=*event;return;}
    }
    unsigned next=(t->event_write+1)%64;
    if(next==t->event_read){if(event->type==NOVA_EV_CLOSE)t->event_read=(t->event_read+1)%64;else return;}
    t->events[t->event_write]=*event;t->event_write=next;
    if(t->state==SLEEPING)t->state=READY;
}
void process_app_event(AppId app,const struct nova_event *event){struct task *t=app_task(app);if(t && t->state!=DEAD)enqueue(t,event);}
void process_notify_all(void){struct nova_event e={.type=NOVA_EV_REDRAW};for(unsigned i=0;i<MAX_TASKS;++i)if(tasks[i].state && tasks[i].app>=0)enqueue(tasks+i,&e);}
static bool surface_resize(struct task *t,int width,int height){
    if(width<64 || height<64 || width>1280 || height>800)return false;
    if(t->width==width && t->height==height && t->back)return true;
    uint32_t size=(uint32_t)width*height*4,old=(uint32_t)t->width*t->height*8;
    if(surface_bytes-old+size*2>40u*1024u*1024u)return false;
    uint32_t *front=malloc(size),*back=malloc(size);if(!front||!back){free(front);free(back);return false;}
    memset(front,0,size);memset(back,0,size);free(t->front);free(t->back);
    surface_bytes=surface_bytes-old+size*2;t->front=front;t->back=back;t->width=width;t->height=height;t->front_width=width;t->front_height=height;
    return true;
}
bool process_app_ensure(AppId app,int width,int height){
    if(app<0 || app>=APP_COUNT || !catalog_id[app][0])return false;
    struct task *t=app_task(app);
    if(t && (t->state==DEAD || t->close_deadline)){destroy(t);t=NULL;}
    if(!t){launch_slot=app;int pid=launch(catalog_id[app]);launch_slot=-1;if(pid<0){terminal_log("APP_LAUNCH_FAILED: ");terminal_log(catalog_id[app]);terminal_log("\n");return false;}t=app_task(app);}
    if(!t || !surface_resize(t,width,height))return false;
    struct nova_event e={.type=NOVA_EV_REDRAW};enqueue(t,&e);return true;
}
void process_app_draw(AppId app,UiRect area){
    struct task *t=app_task(app);
    if(!t || !t->front){gfx_rect(area.x,area.y,area.width,area.height,0x111A29);gfx_text(area.x+16,area.y+16,"Application unavailable. Check /APPS.",0xF38BA8,0x111A29);return;}
    int rows=t->front_height<area.height?t->front_height:area.height;
    int cols=t->front_width<area.width?t->front_width:area.width;
    gfx_rect(area.x,area.y,area.width,area.height,0x111A29);
    for(int y=0;y<rows;++y)gfx_blit(area.x,area.y+y,cols,1,t->front+y*t->front_width);
}
void process_app_resize(AppId app,int width,int height){struct task *t=app_task(app);if(t && surface_resize(t,width,height)){struct nova_event e={.type=NOVA_EV_REDRAW};enqueue(t,&e);}}
void process_app_close(AppId app){struct task *t=app_task(app);if(t){struct nova_event e={.type=NOVA_EV_CLOSE};enqueue(t,&e);t->close_deadline=system_ms()+2000;}}
void process_app_focus(AppId app,bool focus){struct task *t=app_task(app);if(t){struct nova_event e={.type=NOVA_EV_FOCUS,.a=focus};enqueue(t,&e);}}
static void gui_destroy(struct task *t){
    if(t->front || t->back){surface_bytes-=(uint32_t)t->width*t->height*8;free(t->front);free(t->back);}
    for(unsigned i=0;i<16;++i)if(t->files[i])fat32_close(t->files[i]);
    if(network_owner==(int)t->pid)network_owner=-1;
    if(t->state && t->app>=0)desktop_process_exited((AppId)t->app);
}
static bool user_string(struct task *t,uint64_t pointer,char *out,size_t capacity){
    for(size_t i=0;i<capacity;++i){if(!user_copy(t,pointer+i,out+i,1,false))return false;if(!out[i])return true;}out[capacity-1]=0;return false;
}
static bool safe_path(struct task *t,const char *input,char *out,bool write){
    size_t n=strnlen(input,128);if(!n||n>=128||strstr(input,"..")||strchr(input,':')||strchr(input,'\\'))return false;
    if(t->caps&NOVA_CAP_FILES){
        if(input[0]!='/')return false;
        if(!strncmp(input,"/HOME/NOVA",10) && (input[10]=='/'||!input[10])){strcpy(out,input);return true;}
        if(!write && (!strcmp(input,"/")||!strcmp(input,"/HOME")||!strncmp(input,"/APPS",5)||!strncmp(input,"/PKGS",5))){strcpy(out,input);return true;}
        return false;
    }
    if(!(t->caps&NOVA_CAP_STORAGE))return false;
    while(*input=='/' || (*input=='.'&&input[1]=='/'))input+=*input=='/'?1:2;
    if(strlen(input)>95)return false;
    snprintf(out,128,"/APPDATA/%s/%s",t->name,input);return true;
}
static long fs_call(struct task *t,uint64_t address){
    struct nova_fs r;if(!user_copy(t,address,&r,sizeof r,false))return -14;
    if(!(t->caps&(NOVA_CAP_FILES|NOVA_CAP_STORAGE)))return -13;
    char path[128],other[128];bool valid=false;
    if(r.op==NOVA_FS_OPEN||r.op>=NOVA_FS_EXISTS){
        if(!memchr(r.path,0,sizeof r.path))return -22;
        valid=safe_path(t,r.path,path,r.op==NOVA_FS_OPEN?r.flags!=0:(r.op>=NOVA_FS_MKDIR && r.op<=NOVA_FS_RENAME));
    }
    void *f=r.handle>0 && r.handle<=16?t->files[r.handle-1]:NULL;
    switch(r.op){
    case NOVA_FS_OPEN:{if(!valid||r.flags<0||r.flags>2)return -13;
        for(unsigned i=0;i<16;++i)if(!t->files[i]){t->files[i]=fat32_open(path,r.flags==0?"rb":r.flags==1?"wb":"ab");return t->files[i]?(long)i+1:-2;}return -24;}
    case NOVA_FS_CLOSE:if(!f)return -9;fat32_close(f);t->files[r.handle-1]=NULL;return 0;
    case NOVA_FS_READ:case NOVA_FS_WRITE:{
        bool write=r.op==NOVA_FS_WRITE;if(!f)return -9;
        if(r.length>65536 || !user_range(t,r.buffer,r.length,!write))return -14;
        if(!r.length)return 0;
        uint8_t *buf=malloc(r.length);if(!buf)return -12;
        size_t n;if(write){user_copy(t,r.buffer,buf,r.length,false);n=fat32_write(f,buf,1,r.length);}else{n=fat32_read(f,buf,1,r.length);user_copy(t,r.buffer,buf,n,true);}free(buf);return n;}
    case NOVA_FS_SEEK:return f?fat32_seek(f,r.offset,r.whence):-9;
    case NOVA_FS_TELL:return f?fat32_tell(f):-9;
    case NOVA_FS_LENGTH:return f?(long)fat32_length(f):-9;
    case NOVA_FS_EXISTS:return valid && fat32_exists(path);
    case NOVA_FS_MKDIR:return valid && fat32_mkdir(path);
    case NOVA_FS_REMOVE:return valid?fat32_remove(path):-13;
    case NOVA_FS_RENAME:if(!valid||!memchr(r.other,0,sizeof r.other)||!safe_path(t,r.other,other,true))return -13;return fat32_rename(path,other);
    case NOVA_FS_LIST:{if(!valid||r.length>64||!user_range(t,r.buffer,r.length*sizeof(fat32_dirent),true))return -14;
        fat32_dirent entries[64];int n=fat32_list(path,entries,r.length);user_copy(t,r.buffer,entries,(size_t)n*sizeof entries[0],true);return n;}
    case NOVA_FS_USAGE:{uint32_t values[3];fat32_disk_usage(values,values+1,values+2);return user_copy(t,r.buffer,values,sizeof values,true)?0:-14;}
    default:return -38;
    }
}
static long info_call(struct task *t,uint64_t address){
    struct nova_info i={0};i.width=gfx_width();i.height=gfx_height();i.memory=system_memory_mib();i.uptime=system_uptime();i.ms=system_ms();
    i.rx=network_rx_packets();i.tx=network_tx_packets();i.netflags=(network_present()?1:0)|(network_link_up()?2:0);
    i.app=t->app;i.width_client=t->width;i.height_client=t->height;i.focused=desktop_process_focused((AppId)t->app);i.visible=desktop_process_visible((AppId)t->app);
    const UiTheme *theme=desktop_theme();memcpy(i.theme,theme,7*4);i.theme[7]=theme->dark;
    i.wallpaper=desktop_wallpaper();i.preferences=(desktop_snow()?1:0)|(desktop_clock_widget()?2:0)|(desktop_battery_widget()?4:0)|(desktop_icons_visible()?8:0)|(desktop_clock_24h()?16:0);
    snprintf(i.cpu,sizeof i.cpu,"%s",system_cpu());system_time(i.time);system_date(i.date);
    snprintf(i.ip,sizeof i.ip,"%s",network_ip());snprintf(i.gateway,sizeof i.gateway,"%s",network_gateway());snprintf(i.dns,sizeof i.dns,"%s",network_dns());snprintf(i.netstatus,sizeof i.netstatus,"%s",network_status());
    if(network_owner==(int)t->pid){i.generation=network_generation();i.busy=network_busy();i.status=network_status_code();i.length=network_response_length();snprintf(i.error,sizeof i.error,"%s",network_error());snprintf(i.redirect,sizeof i.redirect,"%s",network_redirect());}
    return user_copy(t,address,&i,sizeof i,true)?0:-14;
}
static long desktop_call(struct task *t,uint64_t op,uint64_t value,uint64_t pointer){
    if(!(t->caps&NOVA_CAP_GUI))return -13;
    if(op==NOVA_DESK_OPEN){if(value>=APP_COUNT)return -22;desktop_open((AppId)value);return 0;}
    char text[128];
    if(op==NOVA_DESK_DOCUMENT){if(!(t->caps&NOVA_CAP_FILES)||!user_string(t,pointer,text,sizeof text))return -13;char checked[128];if(!safe_path(t,text,checked,false))return -13;
        desktop_open(APP_NOTES);struct task *notes=app_task(APP_NOTES);if(!notes)return -2;struct nova_event e={.type=NOVA_EV_DOCUMENT};strcpy(e.text,checked);enqueue(notes,&e);return 0;}
    if(op==NOVA_DESK_INSTALL||op==NOVA_DESK_EXEC){if(!(t->caps&NOVA_CAP_FILES)||!user_string(t,pointer,text,sizeof text))return -13;char checked[128];if(!safe_path(t,text,checked,false))return -13;return op==NOVA_DESK_INSTALL?install(checked):launch(checked);}
    if(!(t->caps&NOVA_CAP_SETTINGS))return -13;
    switch(op){
    case NOVA_DESK_DARK:desktop_set_dark(value!=0);break;case NOVA_DESK_ACCENT:desktop_set_accent(value);break;
    case NOVA_DESK_SNOW:desktop_set_snow(value!=0);break;case NOVA_DESK_WALL:desktop_set_wallpaper(value);break;
    case NOVA_DESK_CLOCK:desktop_set_clock_widget(value!=0);break;case NOVA_DESK_BATTERY:desktop_set_battery_widget(value!=0);break;
    case NOVA_DESK_ICONS:desktop_set_icons(value!=0);break;case NOVA_DESK_24H:desktop_set_clock_24h(value!=0);break;
    case NOVA_DESK_PASSWORD:if(!user_string(t,pointer,text,64))return -14;return login_set_password(text);
    default:return -38;
    }process_notify_all();return 0;
}
static long integrated_call(struct task *t){
    uint64_t a=t->frame.rdi,b=t->frame.rsi,c=t->frame.rdx;
    switch(t->frame.rax){
    case NOVA_INFO:return info_call(t,a);
    case NOVA_FS:return fs_call(t,a);
    case NOVA_DESKTOP:return desktop_call(t,a,b,c);
    case NOVA_COMMAND:{if(!(t->caps&NOVA_CAP_SHELL))return -13;char cmd[32],arg[192];if(!user_string(t,a,cmd,sizeof cmd)||!user_string(t,b,arg,sizeof arg))return -14;return process_command(cmd,arg);}
    case NOVA_LOG:{char text[512];if(!user_string(t,a,text,sizeof text))return -14;terminal_log(text);return 0;}
    case NOVA_NET:{if(!(t->caps&NOVA_CAP_NETWORK))return -13;
        if(a==1){char url[1800];if(!user_string(t,b,url,sizeof url))return -14;if(network_busy()&&network_owner!=(int)t->pid)return -16;network_owner=t->pid;return network_fetch(url);}
        if(a==2){if(network_owner!=(int)t->pid)return -13;size_t n=network_response_length();if(c<n)return -22;return user_copy(t,b,(void *)network_response(),n,true)?(long)n:-14;}return -38;}
    case NOVA_TERM:{if(!(t->caps&NOVA_CAP_SHELL))return -13;
        if(a==1)terminal_init();else if(a==2)terminal_clear();else if(a==3)terminal_color(b);else if(a==4)terminal_background(b);else if(a==5)terminal_putc((char)b);else return -38;desktop_mark_dirty();return 0;}
    default:break;
    }
    if(!(t->caps&NOVA_CAP_GUI))return -38;
    switch(t->frame.rax){
    case NOVA_GUI_OPEN:{struct nova_window w;if(!user_copy(t,a,&w,sizeof w,false)||!memchr(w.title,0,sizeof w.title))return -14;
        if(t->app>=0)return t->app; /* The desktop already opened this process; do not steal focus at startup. */
        if(t->app<0){int slot=-1;for(int i=APP_BUILTIN_COUNT;i<APP_COUNT;++i)if(!app_task((AppId)i)&&(!catalog_id[i][0]||!strcmp(catalog_id[i],t->name))){slot=i;break;}if(slot<0)return -12;t->app=slot;strcpy(catalog_id[slot],t->name);snprintf(catalog_title[slot],48,"%s",w.title);desktop_external_rectangle((AppId)slot,w.width,w.height);}
        desktop_open((AppId)t->app);return t->app;}
    case NOVA_GUI_EVENT:{struct nova_event e={0};if(t->event_read==t->event_write)return 0;e=t->events[t->event_read];if(!user_copy(t,a,&e,sizeof e,true))return -14;t->event_read=(t->event_read+1)%64;return 1;}
    case NOVA_GUI_PRESENT:{if(!t->back)return -22;uint32_t *swap=t->front;t->front=t->back;t->back=swap;t->front_width=t->width;t->front_height=t->height;desktop_mark_dirty();return 0;}
    case NOVA_GUI_DRAW:{if(!t->back || b>64 || !user_range(t,a,b*sizeof(struct nova_draw),false))return -14;
        struct nova_draw commands[64];if(!user_copy(t,a,commands,b*sizeof commands[0],false))return -14;
        for(size_t j=0;j<b;++j){struct nova_draw *d=commands+j;int32_t *values=&d->x;for(unsigned k=0;k<6;++k)if(values[k]<-16384||values[k]>16384)return -22;d->text[95]=0;}
        gfx_surface_begin(t->back,t->width,t->height);
        for(size_t j=0;j<b;++j){struct nova_draw *d=commands+j;switch(d->op){
        case NOVA_DRAW_RECT:gfx_rect(d->x,d->y,d->w,d->h,d->fg);break;
        case NOVA_DRAW_LINE:gfx_line(d->x,d->y,d->w,d->h,d->fg);break;
        case NOVA_DRAW_CIRCLE:if(d->w>=0 && d->w<=1024)gfx_circle(d->x,d->y,d->w,d->fg);break;
        case NOVA_DRAW_TRIANGLE:gfx_triangle(d->x,d->y,d->w,d->h,d->a,d->b,d->fg);break;
        case NOVA_DRAW_ROUND:gfx_round_rect(d->x,d->y,d->w,d->h,d->a,d->fg);break;
        case NOVA_DRAW_TEXT:gfx_text_scaled(d->x,d->y,d->text,d->fg,d->bg,d->a);break;
        case NOVA_DRAW_CONSOLE:if(t->caps&NOVA_CAP_SHELL)terminal_render((UiRect){0,0,t->width,t->height},d->a!=0,desktop_theme());break;
        default:break;}}
        gfx_surface_end();return b;}
    case NOVA_GUI_BLIT:{struct nova_blit v;if(!t->back || !user_copy(t,a,&v,sizeof v,false))return -14;
        if(v.width<=0||v.height<=0||v.width>1280||v.height>800||v.x < -1280||v.x>1280||v.y < -800||v.y>800)return -22;
        if(!user_range(t,v.pixels,(uint64_t)v.width*v.height*4,false))return -14;
        int left=v.x<0?-v.x:0,right=v.width;if(v.x+right>t->width)right=t->width-v.x;
        for(int row=0;row<v.height;++row){int y=v.y+row;if(y<0||y>=t->height||right<=left)continue;
            user_copy(t,v.pixels+((uint64_t)row*v.width+left)*4,t->back+y*t->width+v.x+left,(right-left)*4,false);}return 0;}
    default:return -38;
    }
}
