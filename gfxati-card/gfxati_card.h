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

/* The R5xx GPIO block at 0x7E60: MASK / A (output latch) / EN (pin
 * direction: set = driven) / Y (pin state, read-only). atiflash 3.49's
 * det_si2ccfg for the R580 bit-bangs I2C here with SCL = pin 0 and
 * SDA = pin 8, open-drain: the driver's SwSetSCL/SwSetSDA toggle the EN
 * bit (set = driven low, clear = released, pulled up) and SwGetSDA reads
 * Y bit 8. The detection script probes an I2C device at 7-bit address
 * 0x39 (byte 0x72 write / 0x73 read) and expects an ACK before the
 * flash type table is bound and CParallel::RomID runs. */
#define GFXATI_GPIO_MASK	0x7E60
#define GFXATI_GPIO_A		0x7E64
#define GFXATI_GPIO_EN		0x7E68
#define GFXATI_GPIO_Y		0x7E6C

#define GFXATI_GPIO_SCL_EN	0x001
#define GFXATI_GPIO_SDA_EN	0x100

enum gfxati_i2c_state {
	GFXATI_I2C_IDLE,
	GFXATI_I2C_ADDR,
	GFXATI_I2C_ACK,		/* slave pulls SDA low for the 9th clock */
	GFXATI_I2C_WRDATA,	/* master writes data bytes */
	GFXATI_I2C_TXDATA,	/* slave transmits data bytes */
	GFXATI_I2C_RXACK,	/* master ACKs each slave byte */
};

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
	uint8_t strap;	/* the 0xE4 flash-strap high nibble (8=Parallel/VID, 9=AT25F1024/C, ...) */
	uint32_t gpio_mask;
	uint32_t gpio_a;
	uint32_t gpio_en;
	uint32_t i2c_ctl;
	uint32_t i2c_len;
	uint8_t i2c_fifo;

	/* The R6xx-style command window (CR6Serial, atiflash 3.49's serial
	 * path): CNTL1 = 0x9000000|sub arms a window mode, CNTL2 carries the
	 * opcode<<16 / erase address, a window write at offset 0 triggers,
	 * and CNTL1 read-back & 0x1100 is the busy status. */
	int win_mode;		/* 0 array, 1 program stream, 2 status, 3 opcode, 4 erase */
	int win_stream_open;

	/* SPI identification latch (plan/0004#transport): the opcode
	 * trigger discards the flash's MISO bytes, so an ID-class trigger
	 * (9F RDID, 15H AT25F product ID, 90 REMS, AB RES) latches a
	 * normalized 3-byte id that the array window serves at offsets 0,
	 * 1 and 0xE/0xF - mirroring the parallel family's in_id_mode
	 * convention (90H then array read). Cleared by the next window
	 * write or mode re-arm. */
	uint8_t spi_id_latch;
	uint8_t spi_id[3];

	/* GPIO-block I2C slave state (SCL pin 0, SDA pin 8) */
	enum gfxati_i2c_state i2c_state;
	uint32_t i2c_bit;
	uint32_t i2c_byte;
	uint32_t i2c_tx_byte;
	int i2c_sda_low;	/* the slave is pulling SDA low */
	int i2c_scl_prev;
	int i2c_sda_prev;
};

void gfxati_card_init(struct gfxati_card *c, uint32_t device_id,
		      const struct gfxati_flash_chip *chip, uint8_t *rom_mem);
uint32_t gfxati_card_mmio_read(struct gfxati_card *c, uint32_t off);
void gfxati_card_mmio_write(struct gfxati_card *c, uint32_t off, uint32_t val);
uint8_t gfxati_card_rom_read(struct gfxati_card *c, uint32_t addr);
void gfxati_card_rom_write(struct gfxati_card *c, uint32_t addr, uint8_t val);

#endif
