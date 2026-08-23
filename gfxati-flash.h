#ifndef GFXATI_FLASH_H
#define GFXATI_FLASH_H

#include <stdint.h>

enum gfxati_bus {
	GFXATI_SPI,
	GFXATI_PARALLEL,
};

enum gfxati_id_scheme {
	GFXATI_ID_RDID,
	GFXATI_ID_AT25F,
	GFXATI_ID_RES_ONLY,
	GFXATI_ID_JEDEC_PARALLEL,
};

#define GFXATI_ERASE_4K		(1u << 0)
#define GFXATI_ERASE_32K	(1u << 1)
#define GFXATI_ERASE_64K	(1u << 2)
#define GFXATI_ERASE_CHIP	(1u << 3)
#define GFXATI_ERASE_16K	(1u << 4)
#define GFXATI_ERASE_D8_32K	(1u << 5)

#define GFXATI_Q_AAI		(1u << 0)
#define GFXATI_Q_EWSR		(1u << 1)
#define GFXATI_Q_AT45		(1u << 2)
#define GFXATI_Q_FWH		(1u << 3)
#define GFXATI_Q_NO_RDID	(1u << 4)

struct gfxati_flash_chip {
	const char *name;
	uint32_t size;
	uint32_t page_size;
	uint8_t bus;
	uint8_t id_scheme;
	uint8_t id_bytes[3];
	uint8_t id_len;
	uint8_t res_sig;
	uint16_t erase_cmds;
	uint16_t quirks;
};

extern const struct gfxati_flash_chip gfxati_flash_chips[];
extern const unsigned int gfxati_flash_chip_count;

struct gfxati_flash {
	const struct gfxati_flash_chip *chip;
	uint8_t *mem;

	uint8_t status;
	int wren;
	int ewsr;

	int state;
	uint8_t cmd;
	uint32_t addr;
	int addr_bytes_left;
	int dummy_bytes_left;
	int out_idx;
	int aai_active;

	uint8_t at45_buffer[264];
	uint32_t at45_page;

	int cmd_cycle;
	int in_id_mode;
	int program_mode;

	/* AT29C-style page-load programming: after the unlock+A0
	 * cycle, up to page_size bytes accumulate and program together
	 * (the parts have no byte-wise mode). */
	uint32_t par_load_addr[256];
	uint8_t par_load_val[256];
	int par_load_count;
	uint32_t par_stream_last;	/* last byte-stream address + 1 */
	int par_stream_live;
	int par_unlock_pending;	/* a discontinuous AA seen mid-stream */
	uint32_t par_pending_addr;
	uint8_t par_pending_val;
};

void gfxati_flash_init(struct gfxati_flash *f, const struct gfxati_flash_chip *chip,
		       uint8_t *mem);
void gfxati_flash_cs(struct gfxati_flash *f, int asserted);
uint8_t gfxati_flash_spi_xfer(struct gfxati_flash *f, uint8_t in);
void gfxati_flash_parallel_write(struct gfxati_flash *f, uint32_t addr, uint8_t val);
uint8_t gfxati_flash_parallel_read(struct gfxati_flash *f, uint32_t addr);

#endif
