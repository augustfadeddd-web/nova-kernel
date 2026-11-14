#ifndef NOVA_ATA_H
#define NOVA_ATA_H
#include <stdbool.h>
#include <stdint.h>

/* Primary-bus IDE PIO (LBA28) driver.  The hard disk lives at the primary
 * master (drive 0); the El Torito CD is on the secondary bus, so probing
 * only the primary bus can never mistake the boot CD for persistent storage. */
void ata_init(void);
bool ata_present(void);
uint32_t ata_total_sectors(void); /* 0 when no ATA disk was found. */

bool ata_read_sectors(uint32_t lba, uint32_t count, void *buffer);
bool ata_write_sectors(uint32_t lba, uint32_t count, const void *buffer);
bool ata_flush(void);

#endif
