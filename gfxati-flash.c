#include <string.h>

#include "gfxati-flash.h"

#define GFXATI_STATUS_WEL	0x02

enum gfxati_spi_state {
	SPI_IDLE,
	SPI_ADDR,
	SPI_ADDR_DUMMY,
	SPI_READ,
	SPI_AT45_READ,
	SPI_PP,
	SPI_RDSR,
	SPI_WRSR,
	SPI_ID,
	SPI_ATMEL_CE,	/* the Atmel chip-erase pair: 62 armed, 87 fires */
	SPI_REMS,
	SPI_RES_DUMMY,
	SPI_RES_OUT,
	SPI_AAI_DATA,
	SPI_AT45_BUF_ADDR,
	SPI_AT45_BUF_WRITE,
	SPI_AT45_STATUS,
	SPI_AT45_PAGE_ADDR,
};

enum gfxati_par_cycle {
	PAR_IDLE,
	PAR_UNLOCK1_55,
	PAR_CMD,
	PAR_UNLOCK2_AA,
	PAR_UNLOCK2_55,
	PAR_ERASE_CMD,
};

static void erase_block(struct gfxati_flash *f, uint32_t addr, uint32_t len)
{
	if (addr >= f->chip->size) {
		return;
	}
	if (addr + len > f->chip->size) {
		len = f->chip->size - addr;
	}
	memset(f->mem + addr, 0xFF, len);
}

static void erase_region(struct gfxati_flash *f, uint8_t opcode, uint32_t addr)
{
	switch (opcode) {
	case 0x20:
		erase_block(f, addr & ~(4u * 1024 - 1), 4 * 1024);
		return;
	case 0x52:
		erase_block(f, addr & ~(32u * 1024 - 1), 32 * 1024);
		return;
	case 0x62:
		erase_block(f, 0, f->chip->size);
		return;
	case 0xD8:
		if (f->chip->erase_cmds & GFXATI_ERASE_D8_32K) {
			erase_block(f, addr & ~(32u * 1024 - 1), 32 * 1024);
		} else {
			erase_block(f, addr & ~(64u * 1024 - 1), 64 * 1024);
		}
		return;
	case 0x30:
		if (f->chip->erase_cmds & GFXATI_ERASE_16K) {
			erase_block(f, addr & ~(16u * 1024 - 1), 16 * 1024);
		} else if (f->chip->erase_cmds & GFXATI_ERASE_4K) {
			erase_block(f, addr & ~(4u * 1024 - 1), 4 * 1024);
		} else if (f->chip->erase_cmds & GFXATI_ERASE_64K) {
			erase_block(f, addr & ~(64u * 1024 - 1), 64 * 1024);
		} else {
			erase_block(f, 0, f->chip->size);
		}
		return;
	default:
		return;
	}
}

static int chip_has_erase(const struct gfxati_flash_chip *chip, uint8_t opcode)
{
	switch (opcode) {
	case 0x20:
		return !!(chip->erase_cmds & GFXATI_ERASE_4K);
	case 0x52:
		return !!(chip->erase_cmds & GFXATI_ERASE_32K);
	case 0xD8:
		return !!(chip->erase_cmds & GFXATI_ERASE_64K);
	case 0x62:
	case 0xC7:
	case 0x60:
		return !!(chip->erase_cmds & GFXATI_ERASE_CHIP);
	default:
		return 0;
	}
}

static uint32_t at45_page_count(const struct gfxati_flash_chip *chip)
{
	return chip->size / 256;
}

static uint32_t at45_linear(const struct gfxati_flash *f)
{
	uint32_t page = (f->addr >> 8) % at45_page_count(f->chip);
	uint32_t offset = f->addr & 0xFF;

	if (offset >= 256) {
		offset = 263;
	}
	return page * 256 + offset;
}

static void at45_flush_buffer_to_page(struct gfxati_flash *f, int with_erase)
{
	uint32_t page = f->at45_page;

	if (page >= at45_page_count(f->chip)) {
		return;
	}
	if (with_erase) {
		memset(f->mem + page * 256, 0xFF, 256);
	}
	for (uint32_t i = 0; i < 256; i++) {
		f->mem[page * 256 + i] &= f->at45_buffer[i];
	}
}

void gfxati_flash_init(struct gfxati_flash *f, const struct gfxati_flash_chip *chip,
		       uint8_t *mem)
{
	memset(f, 0, sizeof(*f));
	f->chip = chip;
	f->mem = mem;
	f->state = SPI_IDLE;
	f->cmd_cycle = PAR_IDLE;
}

void gfxati_flash_cs(struct gfxati_flash *f, int asserted)
{
	if (asserted) {
		return;
	}
	if (f->state == SPI_AT45_PAGE_ADDR) {
		at45_flush_buffer_to_page(f, 1);
	}
	if (f->state == SPI_PP || f->state == SPI_AAI_DATA ||
	    f->state == SPI_AT45_BUF_WRITE || f->state == SPI_AT45_PAGE_ADDR ||
	    f->state == SPI_AT45_BUF_ADDR) {
		f->wren = 0;
		if (!(f->chip->quirks & GFXATI_Q_AAI)) {
			f->aai_active = 0;
		}
	}
	f->state = SPI_IDLE;
	f->addr_bytes_left = 0;
	f->dummy_bytes_left = 0;
	f->out_idx = 0;
}

static void spi_begin(struct gfxati_flash *f, int dummies)
{
	f->addr = 0;
	f->addr_bytes_left = 3;
	f->dummy_bytes_left = dummies;
	f->state = SPI_ADDR;
}

static uint8_t spi_id_next_byte(struct gfxati_flash *f)
{
	if (f->out_idx < f->chip->id_len) {
		return f->chip->id_bytes[f->out_idx++];
	}
	return 0;
}

static void spi_addr_complete(struct gfxati_flash *f)
{
	if (f->dummy_bytes_left > 0) {
		f->state = SPI_ADDR_DUMMY;
		return;
	}
	f->addr %= f->chip->size;
	switch (f->cmd) {
	case 0x03:
	case 0x0B:
	case 0x3B:
		f->state = (f->chip->quirks & GFXATI_Q_AT45) ? SPI_AT45_READ : SPI_READ;
		break;
	case 0x02:
		f->state = SPI_PP;
		break;
	case 0x20:
	case 0x52:
	case 0xD8:
		if (f->wren && chip_has_erase(f->chip, f->cmd)) {
			erase_region(f, f->cmd, f->addr);
			f->wren = 0;
		}
		f->state = SPI_IDLE;
		break;
	case 0xAD:
		if (f->wren) {
			f->aai_active = 1;
			f->out_idx = 0;
			f->state = SPI_AAI_DATA;
		} else {
			f->state = SPI_IDLE;
		}
		break;
	case 0x90:
		f->out_idx = 0;
		f->state = SPI_REMS;
		break;
	case 0x81:
		if (f->chip->quirks & GFXATI_Q_AT45) {
			/* the AT45 parts have no WREN - erases take the
			 * page address directly */
			erase_block(f, ((f->addr >> 8) % at45_page_count(f->chip)) * 256, 256);
		}
		f->state = SPI_IDLE;
		break;
	case 0x50:
		if (f->chip->quirks & GFXATI_Q_AT45) {
			erase_block(f, ((f->addr >> 8) % at45_page_count(f->chip)) * 256, 8 * 256);
		} else if (f->wren && chip_has_erase(f->chip, 0x50)) {
			erase_region(f, 0x20, f->addr);
			f->wren = 0;
		}
		f->state = SPI_IDLE;
		break;
	case 0x84:
		f->at45_page = 0;
		f->addr &= 0xFF;
		if (f->addr >= 256) {
			f->addr = 0;
		}
		f->out_idx = f->addr;
		/* a fill at offset 0 starts a fresh buffer; a fill at a
		 * higher offset continues (flashprog chunks a page
		 * across multiple buffer-write commands) */
		if (f->addr == 0) {
			memset(f->at45_buffer, 0xFF, sizeof(f->at45_buffer));
		}
		f->state = SPI_AT45_BUF_WRITE;
		break;
	case 0x83:
	case 0x88:
		/* buffer1 to page (0x83 with erase, 0x88 without -
		 * flashprog erases first, so both flush identically) */
		f->at45_page = (f->addr >> 8) % at45_page_count(f->chip);
		f->state = SPI_AT45_PAGE_ADDR;
		break;
	case 0xD2:
		f->state = SPI_AT45_READ;
		break;
	default:
		f->state = SPI_IDLE;
		break;
	}
}

uint8_t gfxati_flash_spi_xfer(struct gfxati_flash *f, uint8_t in)
{
	uint8_t out = 0xFF;

	switch (f->state) {
	case SPI_IDLE:
		f->cmd = in;
		f->out_idx = 0;
		switch (in) {
		case 0x06:
			f->wren = 1;
			break;
		case 0x04:
			f->wren = 0;
			f->aai_active = 0;
			break;
		case 0x05:
			f->state = SPI_RDSR;
			break;
		case 0x01:
			if (f->chip->quirks & GFXATI_Q_EWSR) {
				if (f->ewsr) {
					f->ewsr = 0;
					f->state = SPI_WRSR;
				}
			} else {
				f->state = SPI_WRSR;
			}
			break;
		case 0x50:
			if (f->chip->quirks & GFXATI_Q_EWSR) {
				f->ewsr = 1;
			} else if (f->chip->quirks & GFXATI_Q_AT45) {
				spi_begin(f, 2);
			}
			break;
		case 0x03:
			spi_begin(f, (f->chip->quirks & GFXATI_Q_AT45) ? 2 : 0);
			break;
		case 0x0B:
		case 0x3B:
			spi_begin(f, 1);
			break;
		case 0x02:
		case 0x20:
		case 0x52:
		case 0xD8:
		case 0x81:
			spi_begin(f, 0);
			break;
		case 0x84:
		case 0x83:
		case 0x88:
		case 0xD2:
			if (f->chip->quirks & GFXATI_Q_AT45) {
				spi_begin(f, 0);
			}
			break;
		case 0xD7:
			/* the AT45 status register read: READY,
			 * READY, unprotected, POWEROF2 - 256B pages*/
			if (f->chip->quirks & GFXATI_Q_AT45) {
				f->state = SPI_AT45_STATUS;
			}
			break;
		case 0xAD:
			if (f->chip->quirks & GFXATI_Q_AAI && f->aai_active) {
				f->out_idx = 0;
				f->state = SPI_AAI_DATA;
			} else if (f->chip->quirks & GFXATI_Q_AAI) {
				spi_begin(f, 0);
			}
			break;
		case 0x62:
			/* the Atmel chip-erase pair: 62 arms, 87 fires,
			 * within one chip-select cycle */
			if (f->wren && chip_has_erase(f->chip, 0x62)) {
				f->state = SPI_ATMEL_CE;
			}
			break;
		case 0xC7:
		case 0x60:
			if (f->wren && chip_has_erase(f->chip, 0xC7)) {
				erase_block(f, 0, f->chip->size);
				f->wren = 0;
			}
			break;
		case 0x9F:
			if (f->chip->id_scheme == GFXATI_ID_RDID &&
			    !(f->chip->quirks & GFXATI_Q_NO_RDID)) {
				f->state = SPI_ID;
			}
			break;
		case 0x15:
			if (f->chip->id_scheme == GFXATI_ID_AT25F) {
				f->state = SPI_ID;
			}
			break;
		case 0x90:
			if (f->chip->id_scheme == GFXATI_ID_RDID ||
			    f->chip->id_scheme == GFXATI_ID_AT25F) {
				spi_begin(f, 0);
			}
			break;
		case 0xAB:
			f->dummy_bytes_left = 3;
			f->state = SPI_RES_DUMMY;
			break;
		case 0xB9:
			break;
		default:
			break;
		}
		break;

	case SPI_ADDR:
		f->addr = (f->addr << 8) | in;
		f->addr_bytes_left--;
		if (f->addr_bytes_left == 0) {
			spi_addr_complete(f);
		}
		break;

	case SPI_ADDR_DUMMY:
		f->dummy_bytes_left--;
		if (f->dummy_bytes_left == 0) {
			f->state = (f->chip->quirks & GFXATI_Q_AT45) ? SPI_AT45_READ : SPI_READ;
		}
		break;

	case SPI_READ:
		out = f->mem[f->addr];
		f->addr = (f->addr + 1) % f->chip->size;
		break;

	case SPI_AT45_READ:
		out = f->mem[at45_linear(f)];
		f->addr++;
		break;

	case SPI_PP: {
		uint32_t page = f->addr & ~(f->chip->page_size - 1);

		if (f->wren) {
			f->mem[f->addr] &= in;
		}
		f->addr = page | ((f->addr + 1) & (f->chip->page_size - 1));
		break;
	}

	case SPI_RDSR:
		out = f->status | (f->wren ? GFXATI_STATUS_WEL : 0);
		break;

	case SPI_AT45_STATUS:
		out = 0xAD;
		break;
		/* READY | POWEROF2: 256-byte pages */

	case SPI_WRSR:
		f->status = in & 0x7C;
		f->state = SPI_IDLE;
		break;

	case SPI_ATMEL_CE:
		if (in == 0x87) {
			erase_block(f, 0, f->chip->size);
			f->wren = 0;
		}
		f->state = SPI_IDLE;
		break;

	case SPI_ID:
		out = spi_id_next_byte(f);
		break;

	case SPI_REMS:
		if (f->out_idx == 0) {
			out = f->chip->id_bytes[0];
			f->out_idx = 1;
		} else {
			out = f->chip->id_bytes[f->chip->id_len - 1];
		}
		break;

	case SPI_RES_DUMMY:
		f->dummy_bytes_left--;
		if (f->dummy_bytes_left == 0) {
			f->state = SPI_RES_OUT;
		}
		break;

	case SPI_RES_OUT:
		out = f->chip->res_sig;
		break;

	case SPI_AAI_DATA:
		if (f->out_idx & 1) {
			f->mem[f->addr + 1] &= in;
			f->addr = (f->addr + 2) % f->chip->size;
			f->out_idx = 0;
		} else {
			f->mem[f->addr] &= in;
			f->out_idx = 1;
		}
		break;

	case SPI_AT45_BUF_WRITE:
		if (f->out_idx < 264) {
			f->at45_buffer[f->out_idx] = in;
		}
		f->out_idx++;
		break;

	case SPI_AT45_PAGE_ADDR:
	case SPI_AT45_BUF_ADDR:
	default:
		break;
	}

	return out;
}

/* The JEDEC command decoder sees the low address bits: the unlock
 * cycles arrive at 0x5555/0x2AAA (wide decode) or at 0x555/0x2AA
 * (narrow decode, which flashprog's FEATURE_ADDR_2AA masking
 * drives for the parts whose bus decodes that way). */
#define PAR_ADDR_AA(addr)	(((addr) & 0x7ff) == 0x555)
#define PAR_ADDR_55(addr)	(((addr) & 0x7ff) == 0x2aa)

static void gfxati_par_page_commit(struct gfxati_flash *f)
{
	int i;
	for (i = 0; i < f->par_load_count; i++)
		f->mem[f->par_load_addr[i]] &= f->par_load_val[i];
	f->par_load_count = 0;
	f->program_mode = 0;
}

void gfxati_flash_parallel_write(struct gfxati_flash *f, uint32_t addr, uint8_t val)
{
	addr %= f->chip->size;

	if (f->chip->bus == GFXATI_PARALLEL && f->program_mode &&
	    f->par_load_count > 0 &&
	    !((addr & ~(f->chip->page_size - 1)) ==
	      (f->par_load_addr[0] & ~(f->chip->page_size - 1))))
		gfxati_par_page_commit(f);

	if (f->in_id_mode) {
		if (PAR_ADDR_AA(addr) && val == 0xF0) {
			f->in_id_mode = 0;
			f->cmd_cycle = PAR_IDLE;
		}
		return;
	}

	if (f->program_mode) {
		if (f->chip->bus == GFXATI_PARALLEL &&
		    f->chip->page_size <= 1) {
			/* flashprog streams write chunks (the DB page_size
			 * convention) after one unlock+A0; the real byte
			 * programming families take each consecutive data
			 * byte, and command decoding returns only on an
			 * address discontinuity (the next unlock cycle's
			 * jump) - mid-stream data cannot false-trigger */
			if (!f->par_stream_live || addr == f->par_stream_last) {
				f->mem[addr] &= val;
				f->par_stream_last = addr + 1;
				f->par_stream_live = 1;
				return;
			}
			/* a pending unlock AA meets its pair here: the
			 * command decoder acts on the COMPLETE pair, and
			 * the stream truly ends */
			if (f->par_unlock_pending) {
				if (PAR_ADDR_55(addr) && val == 0x55) {
					f->par_unlock_pending = 0;
					f->program_mode = 0;
					f->par_stream_live = 0;
					f->cmd_cycle = PAR_CMD;
					return;
				}
				/* unpaired: the pending AA was data */
				f->mem[f->par_pending_addr] &= 0xAA;
				f->par_unlock_pending = 0;
			}
			/* a discontinuous AA at the command address is
			 * held pending (it may start the next unlock) */
			if (PAR_ADDR_AA(addr) && val == 0xAA) {
				f->par_unlock_pending = 1;
				f->par_pending_addr = addr;
				return;
			}
			/* any other discontinuous write is stream data
			 * (the FF-skip in flashprog's page writer jumps
			 * addresses) */
			f->mem[addr] &= val;
			f->par_stream_last = addr + 1;
			return;
		} else if (f->chip->bus == GFXATI_PARALLEL && f->chip->page_size > 1) {
			/* page-load: accumulate within one page; a write
			 * to another page (or a full page) commits */
			uint32_t page = f->chip->page_size;
			if (f->par_load_count > 0 &&
			    (addr & ~(page - 1)) !=
				    (f->par_load_addr[0] & ~(page - 1)))
				gfxati_par_page_commit(f);
			f->par_load_addr[f->par_load_count] = addr;
			f->par_load_val[f->par_load_count] = val;
			f->par_load_count++;
			if (f->par_load_count >= (int)page)
				gfxati_par_page_commit(f);
			return;
		}
		f->mem[addr] &= val;
		f->program_mode = 0;
		return;
	}

	switch (f->cmd_cycle) {
	case PAR_IDLE:
		if (PAR_ADDR_AA(addr) && val == 0xAA) {
			if (f->par_load_count > 0)
				gfxati_par_page_commit(f);
			f->cmd_cycle = PAR_UNLOCK1_55;
		}
		break;
	case PAR_UNLOCK1_55:
		if (f->par_unlock_pending && PAR_ADDR_55(addr) && val == 0x55) {
			/* the pair completed: the stream truly ended */
			f->par_unlock_pending = 0;
			f->program_mode = 0;
			f->par_stream_live = 0;
			f->cmd_cycle = PAR_CMD;
		} else if (!f->par_unlock_pending && PAR_ADDR_55(addr) &&
			   val == 0x55) {
			f->cmd_cycle = PAR_CMD;
		} else {
			f->cmd_cycle = PAR_IDLE;
		}
		break;
	case PAR_CMD:
		switch (val) {
		case 0x90:
			f->in_id_mode = 1;
			f->cmd_cycle = PAR_IDLE;
			break;
		case 0xA0:
			f->program_mode = 1;
			f->par_stream_live = 0;
			f->cmd_cycle = PAR_IDLE;
			break;
		case 0x80:
			f->cmd_cycle = PAR_UNLOCK2_AA;
			break;
		default:
			f->cmd_cycle = PAR_IDLE;
			break;
		}
		break;
	case PAR_UNLOCK2_AA:
		if (PAR_ADDR_AA(addr) && val == 0xAA) {
			f->cmd_cycle = PAR_UNLOCK2_55;
		} else {
			f->cmd_cycle = PAR_IDLE;
		}
		break;
	case PAR_UNLOCK2_55:
		if (PAR_ADDR_55(addr) && val == 0x55) {
			f->cmd_cycle = PAR_ERASE_CMD;
		} else {
			f->cmd_cycle = PAR_IDLE;
		}
		break;
	case PAR_ERASE_CMD:
		if (PAR_ADDR_AA(addr) && val == 0x10) {
			erase_block(f, 0, f->chip->size);
		} else if (val == 0x30) {
			erase_region(f, 0x30, addr);
		}
		f->cmd_cycle = PAR_IDLE;
		break;
	default:
		f->cmd_cycle = PAR_IDLE;
		break;
	}
}

uint8_t gfxati_flash_parallel_read(struct gfxati_flash *f, uint32_t addr)
{
	/* A read while a load pends closes the load window: the real
	 * parts start their internal cycle on their own timing, and
	 * any host observation (flashprog's toggle poll, the verify
	 * pass) comes after the load ended. The same read ends a byte
	 * stream: flashprog's per-byte writer programs one byte then
	 * polls, and the next unlock's AA must decode as a command,
	 * not stream data - without this, a data stream whose last
	 * address is exactly 0x5554 swallows the next unlock's AA at
	 * 0x5555 as consecutive data (the ~35 failing bytes in the
	 * byte-family scenarios). */
	if (f->chip->bus == GFXATI_PARALLEL && f->program_mode) {
		if (f->par_load_count > 0)
			gfxati_par_page_commit(f);
		else
			f->program_mode = 0;
		f->par_stream_live = 0;
	}

	addr %= f->chip->size;

	if (f->in_id_mode) {
		switch (addr) {
		case 0:
			return f->chip->id_bytes[0];
		case 1:
			return f->chip->id_bytes[1];
		case 0x0E:
		case 0x0F:
			return f->chip->id_bytes[2];
		default:
			return 0;
		}
	}
	return f->mem[addr];
}

const struct gfxati_flash_chip gfxati_flash_chips[] = {
	{ "AT25F512", 64 * 1024, 256, GFXATI_SPI, GFXATI_ID_AT25F, { 0x1F, 0x60, 0 }, 2, 0, GFXATI_ERASE_32K | GFXATI_ERASE_CHIP, 0 },
	{ "AT25F512A", 64 * 1024, 128, GFXATI_SPI, GFXATI_ID_AT25F, { 0x1F, 0x65, 0 }, 2, 0, GFXATI_ERASE_32K | GFXATI_ERASE_CHIP, 0 },
	{ "AT25F512B", 64 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0x1F, 0x65, 0x00 }, 3, 0, GFXATI_ERASE_4K | GFXATI_ERASE_CHIP, 0 },
	{ "AT25F1024", 128 * 1024, 256, GFXATI_SPI, GFXATI_ID_AT25F, { 0x1F, 0x60, 0 }, 2, 0, GFXATI_ERASE_32K | GFXATI_ERASE_CHIP, 0 },
	{ "AT25F2048", 256 * 1024, 256, GFXATI_SPI, GFXATI_ID_AT25F, { 0x1F, 0x63, 0 }, 2, 0, GFXATI_ERASE_64K | GFXATI_ERASE_CHIP, 0 },
	{ "AT25F4096", 512 * 1024, 256, GFXATI_SPI, GFXATI_ID_AT25F, { 0x1F, 0x64, 0 }, 2, 0, GFXATI_ERASE_64K | GFXATI_ERASE_CHIP, 0 },
	{ "AT25S010N", 128 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0x1F, 0x66, 0x01 }, 3, 0, GFXATI_ERASE_4K, 0 },
	{ "M25P05", 64 * 1024, 256, GFXATI_SPI, GFXATI_ID_RES_ONLY, { 0x20, 0x20, 0x10 }, 3, 0x05, GFXATI_ERASE_64K, 0 },
	{ "M25P10", 128 * 1024, 256, GFXATI_SPI, GFXATI_ID_RES_ONLY, { 0x20, 0x20, 0x11 }, 3, 0x10, GFXATI_ERASE_64K, 0 },
	{ "M25P20", 256 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0x20, 0x20, 0x12 }, 3, 0, GFXATI_ERASE_64K, 0 },
	{ "M25P40", 512 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0x20, 0x20, 0x13 }, 3, 0, GFXATI_ERASE_64K, 0 },
	{ "MX25L512", 64 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xC2, 0x20, 0x10 }, 3, 0, GFXATI_ERASE_4K | GFXATI_ERASE_64K, 0 },
	{ "MX25L5121E", 64 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xC2, 0x22, 0x10 }, 3, 0, GFXATI_ERASE_4K | GFXATI_ERASE_64K, 0 },
	{ "MX25L1005", 128 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xC2, 0x20, 0x11 }, 3, 0, GFXATI_ERASE_4K | GFXATI_ERASE_64K, 0 },
	{ "MX25L1024lE", 128 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xC2, 0x20, 0x11 }, 3, 0, GFXATI_ERASE_4K | GFXATI_ERASE_64K, 0 },
	{ "MX25L2005", 256 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xC2, 0x20, 0x12 }, 3, 0, GFXATI_ERASE_4K | GFXATI_ERASE_64K, 0 },
	{ "SST25VF512", 64 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xBF, 0x25, 0x48 }, 3, 0, GFXATI_ERASE_4K | GFXATI_ERASE_32K | GFXATI_ERASE_64K, 0 },
	{ "SST25VF010", 128 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xBF, 0x25, 0x49 }, 3, 0, GFXATI_ERASE_4K | GFXATI_ERASE_32K | GFXATI_ERASE_64K, 0 },
	{ "SST25VF020", 256 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xBF, 0x25, 0x4A }, 3, 0x43, GFXATI_ERASE_4K | GFXATI_ERASE_32K | GFXATI_ERASE_64K, 0 },
	{ "SST25VF040", 512 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xBF, 0x25, 0x4B }, 3, 0x44, GFXATI_ERASE_4K | GFXATI_ERASE_32K | GFXATI_ERASE_64K, 0 },
	{ "SST25VF040B", 512 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xBF, 0x25, 0x8D }, 3, 0x8D, GFXATI_ERASE_4K | GFXATI_ERASE_32K | GFXATI_ERASE_64K, GFXATI_Q_AAI | GFXATI_Q_EWSR },
	{ "W25P10", 128 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xEF, 0x30, 0 }, 2, 0x10, GFXATI_ERASE_64K | GFXATI_ERASE_CHIP, GFXATI_Q_NO_RDID },
	{ "W25P20", 256 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xEF, 0x31, 0 }, 2, 0x11, GFXATI_ERASE_64K | GFXATI_ERASE_CHIP, GFXATI_Q_NO_RDID },
	{ "W25P40", 512 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xEF, 0x32, 0 }, 2, 0x12, GFXATI_ERASE_64K | GFXATI_ERASE_CHIP, GFXATI_Q_NO_RDID },
	{ "W25Q40", 512 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xEF, 0x40, 0x13 }, 3, 0, GFXATI_ERASE_4K | GFXATI_ERASE_32K | GFXATI_ERASE_64K, 0 },
	{ "W25X10", 128 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xEF, 0x30, 0x11 }, 3, 0x10, GFXATI_ERASE_4K | GFXATI_ERASE_64K, 0 },
	{ "W25X20", 256 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xEF, 0x30, 0x12 }, 3, 0x11, GFXATI_ERASE_4K | GFXATI_ERASE_64K, 0 },
	{ "W25X40", 512 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xEF, 0x30, 0x13 }, 3, 0x12, GFXATI_ERASE_4K | GFXATI_ERASE_64K, 0 },
	{ "W25X80", 1024 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0xEF, 0x30, 0x14 }, 3, 0x13, GFXATI_ERASE_4K | GFXATI_ERASE_64K, 0 },
	{ "S25FL001D", 128 * 1024, 256, GFXATI_SPI, GFXATI_ID_RES_ONLY, { 0, 0, 0 }, 0, 0x10, GFXATI_ERASE_D8_32K | GFXATI_ERASE_CHIP, 0 },
	{ "S25FL002D", 256 * 1024, 256, GFXATI_SPI, GFXATI_ID_RES_ONLY, { 0, 0, 0 }, 0, 0x11, GFXATI_ERASE_64K | GFXATI_ERASE_CHIP, 0 },
	{ "S25FL004A", 512 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0x01, 0x02, 0x12 }, 3, 0, GFXATI_ERASE_64K | GFXATI_ERASE_CHIP, 0 },
	{ "AT45DB011D", 128 * 1024, 256, GFXATI_SPI, GFXATI_ID_RDID, { 0x1F, 0x22, 0x00 }, 3, 0, GFXATI_ERASE_CHIP, GFXATI_Q_AT45 },
	{ "AT29C256", 32 * 1024, 64, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0, 0, 0 }, 0, 0, GFXATI_ERASE_CHIP, 0 },
	{ "AT29C512", 64 * 1024, 128, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0x1F, 0x5D, 0 }, 2, 0, GFXATI_ERASE_CHIP, 0 },
	{ "AT29C010A", 128 * 1024, 128, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0x1F, 0xD5, 0 }, 2, 0, GFXATI_ERASE_CHIP, 0 },
	{ "AT29C020", 256 * 1024, 256, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0x1F, 0xDA, 0 }, 2, 0, GFXATI_ERASE_CHIP, 0 },
	{ "AT29C040A", 512 * 1024, 256, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0x1F, 0xA4, 0 }, 2, 0, GFXATI_ERASE_CHIP, 0 },
	{ "AT49F512", 64 * 1024, 0, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0, 0, 0 }, 0, 0, GFXATI_ERASE_CHIP, 0 },
	{ "AT49F001N", 128 * 1024, 0, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0x1F, 0x05, 0 }, 2, 0, GFXATI_ERASE_CHIP, 0 },
	{ "AT49F001T", 128 * 1024, 0, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0x1F, 0x04, 0 }, 2, 0, GFXATI_ERASE_CHIP, 0 },
	{ "AT49LV010", 128 * 1024, 0, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0x1F, 0x17, 0 }, 2, 0, GFXATI_ERASE_CHIP, 0 },
	{ "MX29F001B", 128 * 1024, 0, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0xC2, 0x19, 0 }, 2, 0, GFXATI_ERASE_16K, 0 },
	{ "MX29F001T", 128 * 1024, 0, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0xC2, 0x18, 0 }, 2, 0, GFXATI_ERASE_16K, 0 },
	{ "MX29F512", 64 * 1024, 0, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0x20, 0x24, 0 }, 2, 0, GFXATI_ERASE_CHIP, 0 },
	{ "Pm39LV512R", 64 * 1024, 0, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0, 0, 0 }, 0, 0, GFXATI_ERASE_CHIP, 0 },
	{ "Pm39LV010R", 128 * 1024, 0, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0, 0, 0 }, 0, 0, GFXATI_ERASE_CHIP, 0 },
	{ "SST39SF512", 64 * 1024, 0, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0xBF, 0xB4, 0 }, 2, 0, GFXATI_ERASE_4K | GFXATI_ERASE_64K, 0 },
	{ "SST39SF010", 128 * 1024, 0, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0xBF, 0xB5, 0 }, 2, 0, GFXATI_ERASE_4K, 0 },
	{ "SST39VF512", 64 * 1024, 0, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0xBF, 0xD4, 0 }, 2, 0, GFXATI_ERASE_4K | GFXATI_ERASE_64K, 0 },
	{ "SST39VF010", 128 * 1024, 0, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0xBF, 0xD5, 0 }, 2, 0, GFXATI_ERASE_4K, 0 },
	{ "SST45LF010", 128 * 1024, 0, GFXATI_PARALLEL, GFXATI_ID_JEDEC_PARALLEL, { 0xBF, 0x42, 0 }, 2, 0, GFXATI_ERASE_4K | GFXATI_ERASE_64K, GFXATI_Q_FWH },
};

const unsigned int gfxati_flash_chip_count =
	sizeof(gfxati_flash_chips) / sizeof(gfxati_flash_chips[0]);
