#include "gfxati_card.h"

void gfxati_card_init(struct gfxati_card *c, uint32_t device_id,
		      const struct gfxati_flash_chip *chip, uint8_t *rom_mem)
{
	c->device_id = device_id;
	c->rom = rom_mem;
	c->mm_index = 0;
	c->seprom_cntl1 = 0;
	c->seprom_cntl2 = 0;
	c->i2c_ctl = 0;
	c->i2c_len = 0;
	c->i2c_fifo = 0;
	gfxati_flash_init(&c->flash, chip, rom_mem);
	gfxati_flash_cs(&c->flash, 1);
}

static void seprom_cntl1_write(struct gfxati_card *c, uint32_t val)
{
	int cs = !!(val & GFXATI_CS_BIT);
	if (cs != !!(c->seprom_cntl1 & GFXATI_CS_BIT)) {
		gfxati_flash_cs(&c->flash, cs);
	}
	c->seprom_cntl1 = val;
}

uint32_t gfxati_card_mmio_read(struct gfxati_card *c, uint32_t off)
{
	switch (off) {
	case GFXATI_MM_INDEX:
		return c->mm_index;
	case GFXATI_MM_DATA:
		switch (c->mm_index) {
		case GFXATI_SEPROM_CNTL1:
			return c->seprom_cntl1;
		case GFXATI_SEPROM_CNTL2:
			return c->seprom_cntl2;
		default:
			return 0;
		}
	case GFXATI_I2C_CTL:
		return c->i2c_ctl;
	case GFXATI_I2C_LEN:
		return c->i2c_len;
	case GFXATI_I2C_DATA:
		return c->i2c_fifo;
	default:
		return 0;
	}
}

void gfxati_card_mmio_write(struct gfxati_card *c, uint32_t off, uint32_t val)
{
	switch (off) {
	case GFXATI_MM_INDEX:
		c->mm_index = val;
		break;
	case GFXATI_MM_DATA:
		switch (c->mm_index) {
		case GFXATI_SEPROM_CNTL1:
			seprom_cntl1_write(c, val);
			break;
		case GFXATI_SEPROM_CNTL2:
			c->seprom_cntl2 = val;
			break;
		default:
			break;
		}
		break;
	case GFXATI_I2C_CTL:
		c->i2c_ctl = val;
		break;
	case GFXATI_I2C_LEN:
		c->i2c_len = val;
		break;
	case GFXATI_I2C_DATA:
		c->i2c_fifo = val;
		break;
	default:
		break;
	}
}

uint8_t gfxati_card_rom_read(struct gfxati_card *c, uint32_t addr)
{
	return gfxati_flash_parallel_read(&c->flash, addr);
}

void gfxati_card_rom_write(struct gfxati_card *c, uint32_t addr, uint8_t val)
{
	gfxati_flash_parallel_write(&c->flash, addr, val);
}
