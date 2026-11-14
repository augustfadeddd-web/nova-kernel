#include "pages.h"
#include <string.h>
#define LIMIT (512u * 1024u * 1024u / 4096u)
static uint8_t available[LIMIT / 8], owned[LIMIT / 8];
static uint32_t free_count, cursor;
extern char _kernel_physical_end[];
static void reserve(uint64_t start, uint64_t end)
{
    uint64_t last=(end+4095)/4096;
    if (last>LIMIT) last=LIMIT;
    for(uint64_t i=start/4096;i<last;++i) available[i/8]&=~(1u<<(i%8));
}
void pages_init(uint32_t address)
{
    memset(available,0,sizeof available); memset(owned,0,sizeof owned);
    free_count=cursor=0;
    if(!address) return;
    uint8_t *base=(uint8_t *)(uintptr_t)address;
    uint32_t total=*(uint32_t *)base;
    if(total<16 || total>1024*1024 || (uint64_t)address+total>0x100000000ULL) return;
    for(uint32_t off=8;off+8<=total;) {
        uint32_t *tag=(uint32_t *)(base+off), size=tag[1];
        if(size<8 || size>total-off || tag[0]==0) break;
        if(tag[0]==6 && size>=16 && tag[2]>=24) {
            for(uint32_t pos=16;pos<=size && tag[2]<=size-pos;pos+=tag[2]) {
                uint64_t start,length; uint32_t kind;
                memcpy(&start,base+off+pos,8);memcpy(&length,base+off+pos+8,8);
                memcpy(&kind,base+off+pos+16,4);
                if(kind!=1 || start>=LIMIT*4096ULL || length>UINT64_MAX-start) continue;
                uint64_t end=(start+length)/4096;
                if(end>LIMIT) end=LIMIT;
                for(uint64_t i=(start+4095)/4096;i<end;++i) available[i/8]|=1u<<(i%8);
            }
        }
        off+=(size+7)&~7u;
    }
    reserve(0,(uintptr_t)_kernel_physical_end);
    reserve(address,(uint64_t)address+total);
    for(uint32_t off=8;off+8<=total;) {
        uint32_t *tag=(uint32_t *)(base+off),size=tag[1];
        if(size<8 || size>total-off || !tag[0]) break;
        if(tag[0]==3 && size>=16 && tag[3]>=tag[2]) reserve(tag[2],tag[3]);
        off+=(size+7)&~7u;
    }
    for(uint32_t i=0;i<LIMIT;++i) if(available[i/8]&(1u<<(i%8))) ++free_count;
}
uint64_t page_alloc(void)
{
    if(!free_count) return 0;
    for(uint32_t n=0;n<LIMIT;++n) {
        uint32_t i=(cursor+n)%LIMIT; uint8_t bit=1u<<(i%8);
        if(available[i/8]&bit) {
            available[i/8]&=~bit;owned[i/8]|=bit;--free_count;cursor=(i+1)%LIMIT;
            uint64_t address=(uint64_t)i*4096;memset((void *)(uintptr_t)address,0,4096);return address;
        }
    }
    return 0;
}
void page_free(uint64_t page)
{
    if((page&4095) || page/4096>=LIMIT) return;
    uint32_t i=page/4096;uint8_t bit=1u<<(i%8);
    if(!(owned[i/8]&bit)) return;
    memset((void *)(uintptr_t)page,0,4096);
    owned[i/8]&=~bit;available[i/8]|=bit;++free_count;
}
uint32_t pages_available(void) { return free_count; }
