#include "gfxati_card.h"

void gfxati_card_init(struct gfxati_card *c, uint32_t device_id,
		      const struct gfxati_flash_chip *chip, uint8_t *rom_mem)
{
	c->device_id = device_id;
	c->rom = rom_mem;
	c->mm_index = 0;
	c->mm_index_legacy = 0;
	c->seprom_cntl1 = 0;
	c->seprom_cntl2 = 0;
	c->seprom_window = 0;
	c->rom_cntl = 0;
	c->rom_cntl2 = 0;
	c->rom_base_direct = 0;
	c->bus_cntl = 0;
	c->reg_0e4 = 0;
	c->seprom_r5_cntl1 = 0;
	c->seprom_r5_cntl2 = 0;
	c->seprom_r5_data = 0;
	c->seprom_r5_status = 0;
	c->seprom_r5_bits = 0;
	c->seprom_r5_byte = 0;
	c->seprom_r5_tx = 0;
	c->seprom_r5_tx_left = 0;
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
	if (val & GFXATI_SEPROM_WINDOW_TAG) {
		c->seprom_window = val & ~1u;
	}
	c->seprom_cntl1 = val;
}

static uint32_t card_index_read(struct gfxati_card *c, uint32_t index)
{
	switch (index) {
	case GFXATI_SEPROM_CNTL1:
	case GFXATI_SEPROM_CNTL1_LEGACY:
		return c->seprom_cntl1;
	case GFXATI_SEPROM_CNTL2:
	case GFXATI_SEPROM_CNTL2_LEGACY:
		return c->seprom_cntl2;
	default:
		return 0;
	}
}

static void card_index_write(struct gfxati_card *c, uint32_t index,
			     uint32_t val)
{
	switch (index) {
	case GFXATI_SEPROM_CNTL1:
	case GFXATI_SEPROM_CNTL1_LEGACY:
		seprom_cntl1_write(c, val);
		break;
	case GFXATI_SEPROM_CNTL2:
	case GFXATI_SEPROM_CNTL2_LEGACY:
		c->seprom_cntl2 = val;
		break;
	default:
		break;
	}
}

static uint32_t card_data_at(struct gfxati_card *c, uint32_t data_off)
{
	switch (data_off) {
	case GFXATI_MM_DATA:
		return card_index_read(c, c->mm_index);
	case GFXATI_MM_DATA_LEGACY:
		return card_index_read(c, c->mm_index_legacy);
	case GFXATI_I2C_CTL:
		return c->i2c_ctl;
	case GFXATI_I2C_LEN:
		return c->i2c_len;
	case GFXATI_I2C_DATA:
		return c->i2c_fifo;
	case GFXATI_ROM_CNTL:
		return c->rom_cntl;
	case GFXATI_ROM_CNTL2:
		return c->rom_cntl2;
	case GFXATI_ROM_BASE_DIRECT:
		return c->rom_base_direct;
	case GFXATI_BUS_CNTL:
		return c->bus_cntl;
	case GFXATI_REG_0E4:
		return 0x80 | (c->reg_0e4 & 0xf);
	case GFXATI_SEPROM_CNTL1_R5:
		return c->seprom_r5_cntl1;
	case GFXATI_SEPROM_CNTL2_R5:
		return c->seprom_r5_cntl2;
	case GFXATI_SEPROM_DATA_R5:
		return c->seprom_r5_data;
	case GFXATI_SEPROM_STATUS_R5:
		return c->seprom_r5_status;
	default:
		return 0;
	}
}

uint32_t gfxati_card_mmio_read(struct gfxati_card *c, uint32_t off)
{
	switch (off) {
	case GFXATI_MM_INDEX:
		return c->mm_index;
	case GFXATI_MM_INDEX_LEGACY:
		return c->mm_index_legacy;
	default:
		return card_data_at(c, off);
	}
}

static void card_data_write(struct gfxati_card *c, uint32_t data_off,
			    uint32_t val)
{
	switch (data_off) {
	case GFXATI_MM_DATA:
		card_index_write(c, c->mm_index, val);
		break;
	case GFXATI_MM_DATA_LEGACY:
		card_index_write(c, c->mm_index_legacy, val);
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
	case GFXATI_ROM_CNTL:
		c->rom_cntl = val;
		break;
	case GFXATI_ROM_CNTL2:
		c->rom_cntl2 = val;
		break;
	case GFXATI_ROM_BASE_DIRECT:
		c->rom_base_direct = val;
		break;
	case GFXATI_BUS_CNTL:
		c->bus_cntl = val;
		break;
	case GFXATI_REG_0E4:
		c->reg_0e4 = val;
		break;
	case GFXATI_SEPROM_CNTL1_R5:
		c->seprom_r5_cntl1 = val;
		c->seprom_r5_status = 0x1;
		c->seprom_r5_bits = 0;
		c->seprom_r5_byte = 0;
		c->seprom_r5_tx_left = 0;
		break;
	case GFXATI_SEPROM_CNTL2_R5:
		c->seprom_r5_cntl2 = val;
		c->seprom_r5_status = 0x1;
		break;
	case GFXATI_SEPROM_DATA_R5: {
		uint32_t old = c->seprom_r5_data;
		c->seprom_r5_data = val;
		/* clock = bit 0, data out = bit 8; sample on the rising edge.
		 * Advance the MISO stream (bit 8 of STATUS) on the same edge. */
		if (!(old & 1) && (val & 1)) {
			c->seprom_r5_byte = (c->seprom_r5_byte << 1) |
					    ((val >> 8) & 1);
			if (c->seprom_r5_tx_left) {
				c->seprom_r5_status =
					0x1 | ((c->seprom_r5_tx >> 7) & 1) << 8;
				c->seprom_r5_tx <<= 1;
				c->seprom_r5_tx_left--;
			}
			c->seprom_r5_bits++;
			if (c->seprom_r5_bits == 8) {
				uint8_t resp = gfxati_flash_spi_xfer(&c->flash,
								    c->seprom_r5_byte);
				c->seprom_r5_tx = resp;
				c->seprom_r5_tx_left = 8;
				c->seprom_r5_bits = 0;
				c->seprom_r5_byte = 0;
			}
		}
		break;
	}
	default:
		break;
	}
}

void gfxati_card_mmio_write(struct gfxati_card *c, uint32_t off, uint32_t val)
{
	switch (off) {
	case GFXATI_MM_INDEX:
		c->mm_index = val;
		break;
	case GFXATI_MM_INDEX_LEGACY:
		c->mm_index_legacy = val;
		break;
	default:
		card_data_write(c, off, val);
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
