#include "ata.h"
#include "terminal.h"

#include <stddef.h>

/* ATA primary bus register offsets from the 0x1F0 base. */
enum {
    ATA_DATA = 0,
    ATA_ERROR = 1,
    ATA_SECTOR_COUNT = 2,
    ATA_LBA_LOW = 3,
    ATA_LBA_MID = 4,
    ATA_LBA_HIGH = 5,
    ATA_DRIVE = 6,
    ATA_STATUS = 7,
    ATA_COMMAND = 7, /* write-only alias of the status register */
};

enum {
    ATA_SR_BSY = 0x80,
    ATA_SR_DRDY = 0x40,
    ATA_SR_DRQ = 0x08,
    ATA_SR_ERR = 0x01,
};

enum {
    ATA_CMD_READ_SECTORS = 0x20,
    ATA_CMD_WRITE_SECTORS = 0x30,
    ATA_CMD_FLUSH = 0xE7,
    ATA_CMD_IDENTIFY = 0xEC,
};

enum { ATA_SPIN_LIMIT = 500000000, SECTOR_SIZE = 512 };

static bool disk_present;
static uint32_t total_sectors;
static uint8_t selected_unit;

static void outb(uint16_t port, uint8_t value)
{
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}

static uint8_t inb(uint16_t port)
{
    uint8_t value;
    __asm__ volatile ("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void outw(uint16_t port, uint16_t value)
{
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}

static uint16_t inw(uint16_t port)
{
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static uint8_t status(void)
{
    return inb(0x1F0 + ATA_STATUS);
}

static bool wait_not_busy(void)
{
    for (unsigned int i = 0; i < ATA_SPIN_LIMIT; ++i) {
        uint8_t current = status();
        if (current == 0xFF)
            return false; /* Nothing attached: a floating bus reads 0xFF. */
        if (!(current & ATA_SR_BSY))
            return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static bool wait_data_ready(void)
{
    for (unsigned int i = 0; i < ATA_SPIN_LIMIT; ++i) {
        uint8_t current = status();
        if (current == 0xFF)
            return false;
        if (current & ATA_SR_ERR)
            return false;
        if (!(current & ATA_SR_BSY) && (current & ATA_SR_DRQ))
            return true;
        __asm__ volatile ("pause");
    }
    return false;
}

static void select_drive(uint32_t lba)
{
    outb(0x1F0 + ATA_DRIVE, (uint8_t)(0xE0 | (selected_unit << 4) | ((lba >> 24) & 0x0F)));
}

static bool program_lba(uint32_t lba, uint8_t count)
{
    if (!wait_not_busy())
        return false;
    select_drive(lba);
    if (!wait_not_busy())
        return false;
    outb(0x1F0 + ATA_SECTOR_COUNT, count);
    outb(0x1F0 + ATA_LBA_LOW, (uint8_t)lba);
    outb(0x1F0 + ATA_LBA_MID, (uint8_t)(lba >> 8));
    outb(0x1F0 + ATA_LBA_HIGH, (uint8_t)(lba >> 16));
    return true;
}

void ata_init(void)
{
    static uint16_t identify[256];
    disk_present = false;
    total_sectors = 0;

    uint32_t sectors=0;
    for(unsigned unit=0;unit<2;++unit){
        selected_unit=(uint8_t)unit;
        outb(0x1F0+ATA_DRIVE,(uint8_t)(0xA0|(unit<<4)));
        for(unsigned delay=0;delay<4;++delay)(void)inb(0x3F6);
        if(status()==0 || status()==0xFF || !wait_not_busy())continue;
        outb(0x1F0+ATA_SECTOR_COUNT,0);outb(0x1F0+ATA_LBA_LOW,0);
        outb(0x1F0+ATA_LBA_MID,0);outb(0x1F0+ATA_LBA_HIGH,0);
        outb(0x1F0+ATA_COMMAND,ATA_CMD_IDENTIFY);
        if(!status() || !wait_not_busy())continue;
        /* An ATAPI CD returns a signature instead of an ATA identify block. */
        if(inb(0x1F0+ATA_LBA_MID)||inb(0x1F0+ATA_LBA_HIGH)||!wait_data_ready())continue;
        for(unsigned i=0;i<256;++i)identify[i]=inw(0x1F0+ATA_DATA);
        sectors=(uint32_t)identify[60]|((uint32_t)identify[61]<<16);
        if(!sectors || sectors>0x0FFFFFFF)continue;
        disk_present=true;total_sectors=sectors;break;
    }
    if(!disk_present)return;
    terminal_log("ATA_DISK: ");
    {
        char digits[12], text[12];
        uint32_t value = sectors;
        unsigned int count = 0, written = 0;
        do { digits[count++] = (char)('0' + value % 10); value /= 10; } while (value);
        while (count) text[written++] = digits[--count];
        text[written] = 0;
        terminal_log(text);
    }
    terminal_log(" sectors\n");
}

bool ata_present(void)
{
    return disk_present;
}

uint32_t ata_total_sectors(void)
{
    return disk_present ? total_sectors : 0;
}

bool ata_read_sectors(uint32_t lba, uint32_t count, void *buffer)
{
    uint16_t *words = buffer;
    if (!disk_present || !buffer || count == 0)
        return false;
    while (count > 0) {
        uint8_t chunk = count > 255 ? 255 : (uint8_t)count;
        if (!program_lba(lba, chunk))
            return false;
        outb(0x1F0 + ATA_COMMAND, ATA_CMD_READ_SECTORS);
        for (uint8_t s = 0; s < chunk; ++s) {
            if (!wait_data_ready())
                return false;
            for (unsigned int i = 0; i < 256; ++i)
                *words++ = inw(0x1F0 + ATA_DATA);
        }
        lba += chunk;
        count -= chunk;
    }
    return true;
}

bool ata_write_sectors(uint32_t lba, uint32_t count, const void *buffer)
{
    const uint16_t *words = buffer;
    if (!disk_present || !buffer || count == 0)
        return false;
    while (count > 0) {
        uint8_t chunk = count > 255 ? 255 : (uint8_t)count;
        if (!program_lba(lba, chunk))
            return false;
        outb(0x1F0 + ATA_COMMAND, ATA_CMD_WRITE_SECTORS);
        for (uint8_t s = 0; s < chunk; ++s) {
            if (!wait_data_ready())
                return false;
            for (unsigned int i = 0; i < 256; ++i)
                outw(0x1F0 + ATA_DATA, *words++);
        }
        lba += chunk;
        count -= chunk;
    }
    return true;
}

bool ata_flush(void)
{
    if (!disk_present)
        return false;
    if (!wait_not_busy())
        return false;
    outb(0x1F0 + ATA_COMMAND, ATA_CMD_FLUSH);
    return wait_not_busy();
}
