#ifndef GFXATI_CARD_H
#define GFXATI_CARD_H

#include <stdint.h>
#include "../gfxati-flash.h"

/*
 * The R5xx (Rialto) flash interface, from plan/0003#gfxati-interface.
 * Grounded MMIO map (atiflash 3.49 decompile + rig traces):
 *   - MM_INDEX 0xA0 / MM_DATA 0xA4 indirect pair (Rialto)
 *   - MM_INDEX 0x008 / MM_DATA 0x00C legacy pair (R300-compat, the
 *     R580's descriptor engine uses it for the SEPROM at indices 0x80/0x81)
 *   - SEPROM_CNTL1 = indirect 0x1C0, SEPROM_CNTL2 = indirect 0x1C4
 *   - SEPROM_CNTL1/2 at the legacy indices 0x80/0x81, with read-back
 *   - ROM window base = indirect 0x30 (atiflash's COsDepend::MapRomAperture
 *     reads it, maps 128KB there, enables the PCI ROM BAR)
 *   - I2C engine: 0x3E0 control, 0x3E4 length/flags, 0x3E8 data byte
 *   - Direct ROM/PCI controls with read-back: 0x0E4, 0x198 (ROM_CNTL),
 *     0x19C, 0x1A0, 0x04C (BUS_CNTL) - the R580 descriptor engine
 *     probes these; every read must echo what was written.
 *   - SEPROM_CNTL1 bit 0x400 = the flash chip-select; bits 0x1100 = busy
 */

#define GFXATI_MM_INDEX		0xA0
#define GFXATI_MM_DATA		0xA4
#define GFXATI_MM_INDEX_LEGACY	0x008
#define GFXATI_MM_DATA_LEGACY	0x00C
#define GFXATI_I2C_CTL		0x3E0
#define GFXATI_I2C_LEN		0x3E4
#define GFXATI_I2C_DATA		0x3E8
#define GFXATI_ROM_CNTL		0x198
#define GFXATI_ROM_CNTL2	0x19C
#define GFXATI_ROM_BASE_DIRECT	0x1A0
#define GFXATI_BUS_CNTL		0x04C
#define GFXATI_REG_0E4		0x0E4

/* The R5xx SEPROM engine (R580's serial flash path): the driver bit-bangs
 * an I2C-style EEPROM at address 0xA0 through SEPROM_DATA: bit 8 = clock,
 * bit 0 = data out; SEPROM_STATUS bit 0 = data in (ACK low), bit 8 = ready. */
#define GFXATI_SEPROM_CNTL1_R5	0x7E60
#define GFXATI_SEPROM_CNTL2_R5	0x7E64
#define GFXATI_SEPROM_DATA_R5	0x7E68
#define GFXATI_SEPROM_STATUS_R5	0x7E6C

#define GFXATI_SEPROM_R5_READY	0x100

#define GFXATI_SEPROM_CNTL1	0x1C0
#define GFXATI_SEPROM_CNTL2	0x1C4
#define GFXATI_SEPROM_CNTL1_LEGACY	0x80
#define GFXATI_SEPROM_CNTL2_LEGACY	0x81

/* The ROM window base register: MM_INDEX 0x30 via the MM_INDEX/MM_DATA
 * pair. atiflash's COsDepend::MapRomAperture reads it to find where the
 * card's flash window lives, maps 128KB there, and enables the PCI ROM
 * BAR (config reg 0x30). The value is the card's own ROM BAR base. */
#define GFXATI_ROM_BASE_INDEX	0x30

#define GFXATI_CS_BIT		0x400
#define GFXATI_BUSY_BITS	0x1100

/* The R300-era SEPROM window: the driver writes SEPROM_CNTL1 =
 * 0x2000000 | enabled (base tag 0x2000000 = guest-physical 0x2000000,
 * 128KB), and then reads/writes the flash through that window. The
 * device must serve the flash there, distinct from the PCI ROM BAR. */
#define GFXATI_SEPROM_WINDOW_TAG	0x2000000

#define GFXATI_MMIO_SIZE	0x8000

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
	uint32_t mm_index_legacy;
	uint32_t seprom_cntl1;
	uint32_t seprom_cntl2;
	uint32_t seprom_window;
	uint32_t rom_cntl;
	uint32_t rom_cntl2;
	uint32_t rom_base_direct;
	uint32_t bus_cntl;
	uint32_t reg_0e4;
	uint32_t seprom_r5_cntl1;
	uint32_t seprom_r5_cntl2;
	uint32_t seprom_r5_data;
	uint32_t seprom_r5_status;
	uint32_t seprom_r5_bits;
	uint32_t seprom_r5_byte;
	uint32_t seprom_r5_tx;
	uint32_t seprom_r5_tx_left;
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
