#include "gfxati_card.h"

/* set by the QEMU glue when debug=on; logs the command-window events */
void (*gfxati_card_trace)(const char *fmt, ...);

#define TRACE(...) do { \
	if (gfxati_card_trace) { \
		gfxati_card_trace(__VA_ARGS__); \
	} \
} while (0)

/*
 * The R5xx GPIO block and its bit-banged I2C bus. af349's det_si2ccfg:
 * SwSetSCL(x) / SwSetSDA(x) set or clear the EN bit (set = driven low,
 * clear = released, pulled up), SwGetSCL/SwGetSDA read the Y register
 * (SCL = bit 0, SDA = bit 8, no inversion on the read side). The master
 * only changes the lines through EN writes, so the slave state machine
 * advances there.
 */
static void i2c_idle(struct gfxati_card *c)
{
	c->i2c_state = GFXATI_I2C_IDLE;
	c->i2c_bit = 0;
	c->i2c_byte = 0;
	c->i2c_sda_low = 0;
}

static void i2c_next_tx_bit(struct gfxati_card *c)
{
	c->i2c_sda_low = !((c->i2c_tx_byte >> 7) & 1);
	c->i2c_tx_byte <<= 1;
}

static void gpio_i2c_update(struct gfxati_card *c)
{
	int scl = !(c->gpio_en & GFXATI_GPIO_SCL_EN);
	int sda_master = (c->gpio_en & GFXATI_GPIO_SDA_EN) ? 0 : 1;
	int sda = c->i2c_sda_low ? 0 : (sda_master ? 1 : 0);
	int scl_rose = scl && !c->i2c_scl_prev;
	int scl_fell = !scl && c->i2c_scl_prev;

	if (scl) {
		/* START / STOP are SDA transitions while SCL is high */
		if (c->i2c_sda_prev && !sda) {
			i2c_idle(c);
			c->i2c_state = GFXATI_I2C_ADDR;
		} else if (!c->i2c_sda_prev && sda && !scl_rose &&
			   c->i2c_state != GFXATI_I2C_IDLE) {
			i2c_idle(c);
		}
	}

	if (scl_rose) {
		switch (c->i2c_state) {
		case GFXATI_I2C_ADDR:
		case GFXATI_I2C_WRDATA:
			c->i2c_byte = (c->i2c_byte << 1) | sda;
			if (++c->i2c_bit == 8) {
				c->i2c_bit = 0;
				if (c->i2c_state == GFXATI_I2C_ADDR) {
					int read = c->i2c_byte & 1;
					int hit = (c->i2c_byte & 0xfe) == 0x72;
					if (hit) {
						c->i2c_state = GFXATI_I2C_ACK;
						c->i2c_sda_low = 1;
					} else {
						i2c_idle(c);
					}
					if (hit && read) {
						/* first data byte a read
						 * returns; refined from
						 * the rig traces */
						c->i2c_tx_byte = 0x00;
					}
				} else {
					/* every written byte is ACKed */
					c->i2c_state = GFXATI_I2C_ACK;
					c->i2c_sda_low = 1;
				}
			}
			break;
		case GFXATI_I2C_ACK:
			/* 9th clock sampled; release after the fall */
			break;
		case GFXATI_I2C_TXDATA:
			if (++c->i2c_bit == 8) {
				c->i2c_bit = 0;
				c->i2c_state = GFXATI_I2C_RXACK;
				c->i2c_sda_low = 0;
			}
			break;
		default:
			break;
		}
	}

	if (scl_fell) {
		switch (c->i2c_state) {
		case GFXATI_I2C_ACK:
			c->i2c_sda_low = 0;
			if (c->i2c_byte & 1) {
				/* repeated START read: first bit out */
				i2c_next_tx_bit(c);
				c->i2c_state = GFXATI_I2C_TXDATA;
				c->i2c_bit = 1;
			} else {
				c->i2c_state = GFXATI_I2C_WRDATA;
				c->i2c_bit = 0;
				c->i2c_byte = 0;
			}
			break;
		case GFXATI_I2C_TXDATA:
			if (c->i2c_bit > 0 && c->i2c_bit < 8) {
				i2c_next_tx_bit(c);
			}
			break;
		default:
			break;
		}
	}

	c->i2c_scl_prev = scl;
	c->i2c_sda_prev = sda;
}

static uint32_t gpio_read_y(struct gfxati_card *c)
{
	/* driven pins show the A latch, released pins are pulled high */
	uint32_t y = (c->gpio_en & c->gpio_a) | ~c->gpio_en;

	if (c->i2c_sda_low) {
		y &= ~GFXATI_GPIO_SDA_EN;
	}
	return y;
}

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
	c->strap = 8;
	c->gpio_mask = 0;
	c->gpio_a = 0;
	c->gpio_en = 0;
	c->i2c_state = GFXATI_I2C_IDLE;
	c->i2c_bit = 0;
	c->i2c_byte = 0;
	c->i2c_tx_byte = 0;
	c->i2c_sda_low = 0;
	c->i2c_scl_prev = 1;
	c->i2c_sda_prev = 1;
	c->win_mode = 0;
	c->win_stream_open = 0;
	c->spi_id_latch = 0;
	c->spi_id[0] = c->spi_id[1] = c->spi_id[2] = 0;
	c->i2c_ctl = 0;
	c->i2c_len = 0;
	c->i2c_fifo = 0;
	c->i2c_state = GFXATI_I2C_IDLE;
	c->i2c_bit = 0;
	c->i2c_byte = 0;
	c->i2c_tx_byte = 0;
	c->i2c_sda_low = 0;
	c->i2c_scl_prev = 1;
	c->i2c_sda_prev = 1;
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
	if ((val & 0x0f000000) == 0x09000000) {
		uint32_t sub = val & ~GFXATI_CS_BIT & 0xffff;
		if (c->win_stream_open) {
			/* the page-program stream closes when the mode ends */
			gfxati_flash_cs(&c->flash, 1);
			c->win_stream_open = 0;
		}
		/* a mode re-arm ends any latched identification */
		c->spi_id_latch = 0;
		if (sub == 0x00000 || sub == 0x00200) {
			/* program stream armed (0x200 marks a burst; the
			 * byte count rides bits 16-23) */
			c->win_mode = 1;
		} else if (sub == 0x00010) {
			c->win_mode = 2;	/* status window */
		} else {
			c->win_mode = 3;	/* one-shot opcode (WREN et al.) */
		}
	} else {
		c->win_mode = 0;
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
		return ((uint32_t)c->strap << 4) | (c->reg_0e4 & 0xf);
	case GFXATI_GPIO_MASK:
		return c->gpio_mask;
	case GFXATI_GPIO_A:
		return c->gpio_a;
	case GFXATI_GPIO_EN:
		return c->gpio_en;
	case GFXATI_GPIO_Y:
		return gpio_read_y(c);
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
	case GFXATI_SEPROM_CNTL1:
	case GFXATI_SEPROM_CNTL2:
		/* the R6xx-class direct registers */
		return card_index_read(c, off);
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
	case GFXATI_SEPROM_CNTL1:
	case GFXATI_SEPROM_CNTL2:
		card_index_write(c, data_off, val);
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
	case GFXATI_GPIO_MASK:
		c->gpio_mask = val;
		break;
	case GFXATI_GPIO_A:
		c->gpio_a = val;
		gpio_i2c_update(c);
		break;
	case GFXATI_GPIO_EN:
		c->gpio_en = val;
		gpio_i2c_update(c);
		break;
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
	if (c->win_mode == 2) {
		/* status window: a fresh RDSR */
		uint8_t s;
		gfxati_flash_cs(&c->flash, 0);
		gfxati_flash_spi_xfer(&c->flash, 0x05);
		s = gfxati_flash_spi_xfer(&c->flash, 0xff);
		gfxati_flash_cs(&c->flash, 1);
		TRACE("win status read => %02x\n", s);
		return s;
	}
	if (c->spi_id_latch && c->win_mode == 0) {
		/* SPI identification served through the array window,
		 * mirroring the parallel family's in_id_mode convention */
		switch (addr) {
		case 0:
			return c->spi_id[0];
		case 1:
			return c->spi_id[1];
		case 0x0E:
		case 0x0F:
			return c->spi_id[2];
		default:
			return 0;
		}
	}
	return gfxati_flash_parallel_read(&c->flash, addr);
}

void gfxati_card_rom_write(struct gfxati_card *c, uint32_t addr, uint8_t val)
{
	/* any window write ends a latched identification (the parallel
	 * family's convention: writes exit ID mode) */
	c->spi_id_latch = 0;
	switch (c->win_mode) {
	case 1:
		/* program stream: one long page-program through the window;
		 * the command "reset" write (offset 0, value 0) does not
		 * start one */
		if (!c->win_stream_open && (addr != 0 || val != 0)) {
			TRACE("win program stream open at %05x\n", addr);
		gfxati_flash_cs(&c->flash, 0);
			gfxati_flash_spi_xfer(&c->flash, 0x02);
			gfxati_flash_spi_xfer(&c->flash, (addr >> 16) & 0xff);
			gfxati_flash_spi_xfer(&c->flash, (addr >> 8) & 0xff);
			gfxati_flash_spi_xfer(&c->flash, addr & 0xff);
			c->win_stream_open = 1;
		}
		gfxati_flash_spi_xfer(&c->flash, val);
		break;
	case 3: {
		/* opcode trigger (WREN/WRDS/erase/... from CNTL2): each
		 * trigger is its own clean chip-select cycle */
		uint8_t op = (c->seprom_cntl2 >> 16) & 0xff;

		TRACE("win opcode trigger %02x\n", op);
		gfxati_flash_cs(&c->flash, 0);
		gfxati_flash_spi_xfer(&c->flash, op);
		if (op == 0x62) {
			/* the card completes the Atmel chip-erase pair
			 * (atiflash sends 0x62 alone; the flash takes
			 * 62 87 per flashprog's AT25F1024 entry) */
			gfxati_flash_spi_xfer(&c->flash, 0x87);
		}
		/* address-carrying erases take their sector address as
		 * CNTL2 bits 0-15 = addr[23:8] plus the trigger byte as
		 * addr[7:0] (the byte is the operand slot, unused by
		 * erases; the opcode keeps its attested bits 16-23) */
		switch (op) {
		case 0x20:	/* SE 4KB */
		case 0x52:	/* AT25F sector erase */
		case 0x81:	/* AT45 block erase */
		case 0x94:	/* AT45 sector erase */
		case 0xd8:	/* BE 64KB */
			gfxati_flash_spi_xfer(&c->flash,
					      (c->seprom_cntl2 >> 8) & 0xff);
			gfxati_flash_spi_xfer(&c->flash,
					      c->seprom_cntl2 & 0xff);
			gfxati_flash_spi_xfer(&c->flash, val & 0xff);
			break;
		default:
			gfxati_flash_spi_xfer(&c->flash, val);
			break;
		}
		gfxati_flash_cs(&c->flash, 1);
		/* ID-class triggers latch their answer for the array
		 * window (the trigger path discards MISO) */
		switch (op) {
		case 0x9f:
		case 0x15:
			/* RDID / AT25F product ID: the id bytes in order */
			c->spi_id[0] = c->flash.chip->id_bytes[0];
			c->spi_id[1] = c->flash.chip->id_bytes[1];
			c->spi_id[2] = c->flash.chip->id_bytes[2];
			c->spi_id_latch = 1;
			break;
		case 0x90:
			/* REMS: manufacturer then device, repeating */
			c->spi_id[0] = c->flash.chip->id_bytes[0];
			c->spi_id[1] = c->flash.chip->id_bytes[c->flash.chip->id_len - 1];
			c->spi_id[2] = 0;
			c->spi_id_latch = 1;
			break;
		case 0xab:
			/* RES: the electronic signature */
			c->spi_id[0] = c->flash.chip->res_sig;
			c->spi_id[1] = 0;
			c->spi_id[2] = 0;
			c->spi_id_latch = 1;
			break;
		default:
			c->spi_id_latch = 0;
			break;
		}
		break;
	}
	default:
		gfxati_flash_parallel_write(&c->flash, addr, val);
		break;
	}
}