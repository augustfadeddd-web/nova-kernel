#ifndef NOVA_PAGES_H
#define NOVA_PAGES_H
#include <stdint.h>
#include <stdbool.h>
void pages_init(uint32_t multiboot);
uint64_t page_alloc(void);
void page_free(uint64_t page);
uint32_t pages_available(void);
#endif
