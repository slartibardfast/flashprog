#ifndef GFXATI_CARD_H
#define GFXATI_CARD_H

#include <stdint.h>
#include "../gfxati-flash.h"

/*
 * The R5xx (Rialto) flash interface, from plan/0003#gfxati-interface.
 * Grounded MMIO map (atiflash 3.49 decompile):
 *   - MM_INDEX 0xA0 / MM_DATA 0xA4 indirect pair
 *   - SEPROM_CNTL1 = indirect 0x1C0, SEPROM_CNTL2 = indirect 0x1C4
 *   - I2C engine: 0x3E0 control, 0x3E4 length/flags, 0x3E8 data byte
 *   - SEPROM_CNTL1 bit 0x400 = the flash chip-select; bits 0x1100 = busy
 * Open item: the MMIO offset of the flash window address register (the
 * vtable slot that receives the 0x9000000-relative window values); it is
 * not in the C listing, so this device cut serves the flash through the
 * ROM window directly and leaves the window register for the rig round.
 */

#define GFXATI_MM_INDEX		0xA0
#define GFXATI_MM_DATA		0xA4
#define GFXATI_I2C_CTL		0x3E0
#define GFXATI_I2C_LEN		0x3E4
#define GFXATI_I2C_DATA		0x3E8

#define GFXATI_SEPROM_CNTL1	0x1C0
#define GFXATI_SEPROM_CNTL2	0x1C4

#define GFXATI_CS_BIT		0x400
#define GFXATI_BUSY_BITS	0x1100

#define GFXATI_MMIO_SIZE	0x400

/* PCI identity, from plan/0002: R520 0x7100/0x7210, R580 0x7240/0x7243/0x7248 */
#define GFXATI_VENDOR_ID	0x1002

enum gfxati_card_device {
	GFXATI_DEV_R520_A = 0x7100,
	GFXATI_DEV_R520_B = 0x7210,
	GFXATI_DEV_R580_A = 0x7240,
	GFXATI_DEV_R580_B = 0x7243,
	GFXATI_DEV_R580_C = 0x7248,
};

struct gfxati_card {
	uint32_t device_id;
	struct gfxati_flash flash;
	uint8_t *rom;

	uint32_t mm_index;
	uint32_t seprom_cntl1;
	uint32_t seprom_cntl2;
	uint32_t i2c_ctl;
	uint32_t i2c_len;
	uint8_t i2c_fifo;
};

void gfxati_card_init(struct gfxati_card *c, uint32_t device_id,
		      const struct gfxati_flash_chip *chip, uint8_t *rom_mem);
uint32_t gfxati_card_mmio_read(struct gfxati_card *c, uint32_t off);
void gfxati_card_mmio_write(struct gfxati_card *c, uint32_t off, uint32_t val);
uint8_t gfxati_card_rom_read(struct gfxati_card *c, uint32_t addr);
void gfxati_card_rom_write(struct gfxati_card *c, uint32_t addr, uint8_t val);

#endif
