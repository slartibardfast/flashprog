#include <stdio.h>
#include <string.h>

#include "gfxati-flash.h"

/* The obligations checker resolves test definitions by `fn <name>(` (Rust) or
   `name() {` (shell); C has neither. A `fn` macro that preprocesses to nothing
   lets the C test functions be written in the checker's resolvable form while
   staying ordinary C. */
#define fn static void

static uint8_t mem[1024 * 1024];
static int failures;

static void fail(const struct gfxati_flash_chip *chip, const char *what)
{
	fprintf(stderr, "FAIL %-11s %s\n", chip->name, what);
	failures++;
}

static uint8_t spi_tx(struct gfxati_flash *f, const uint8_t *in, size_t len)
{
	uint8_t out = 0xFF;

	gfxati_flash_cs(f, 1);
	for (size_t i = 0; i < len; i++) {
		out = gfxati_flash_spi_xfer(f, in[i]);
	}
	gfxati_flash_cs(f, 0);
	return out;
}

#define CMD(f, ...) spi_tx(f, (uint8_t[]){ __VA_ARGS__ }, sizeof((uint8_t[]){ __VA_ARGS__ }))

static uint8_t rdsr(struct gfxati_flash *f)
{
	return CMD(f, 0x05, 0x00);
}

static void wren(struct gfxati_flash *f)
{
	CMD(f, 0x06);
}

static void pp(struct gfxati_flash *f, uint32_t addr, uint8_t val)
{
	CMD(f, 0x02, (addr >> 16) & 0xFF, (addr >> 8) & 0xFF, addr & 0xFF, val);
}

static int probe_id(struct gfxati_flash *f, const struct gfxati_flash_chip *chip,
		    uint8_t got[3])
{
	gfxati_flash_cs(f, 1);

	switch (chip->id_scheme) {
	case GFXATI_ID_AT25F:
		gfxati_flash_spi_xfer(f, 0x15);
		break;
	case GFXATI_ID_RES_ONLY:
		gfxati_flash_spi_xfer(f, 0xAB);
		gfxati_flash_spi_xfer(f, 0);
		gfxati_flash_spi_xfer(f, 0);
		gfxati_flash_spi_xfer(f, 0);
		got[0] = gfxati_flash_spi_xfer(f, 0);
		gfxati_flash_cs(f, 0);
		return 0;
	default:
		if (chip->quirks & GFXATI_Q_NO_RDID) {
			gfxati_flash_spi_xfer(f, 0x90);
			for (int i = 0; i < 3; i++) {
				gfxati_flash_spi_xfer(f, 0);
			}
		} else {
			gfxati_flash_spi_xfer(f, 0x9F);
		}
		break;
	}

	for (int i = 0; i < 3; i++) {
		got[i] = gfxati_flash_spi_xfer(f, 0);
	}
	gfxati_flash_cs(f, 0);
	return 0;
}

static int all_ff(const uint8_t *p, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (p[i] != 0xFF) {
			return 0;
		}
	}
	return 1;
}

fn test_spi_chip(const struct gfxati_flash_chip *chip)
{
	struct gfxati_flash f;
	uint8_t id[3] = { 0xFF, 0xFF, 0xFF };

	gfxati_flash_init(&f, chip, mem);
	memset(mem, 0xFF, chip->size);

	if (probe_id(&f, chip, id)) {
		return;
	}

	switch (chip->id_scheme) {
	case GFXATI_ID_RES_ONLY:
		if (id[0] != chip->res_sig) {
			fail(chip, "RES signature mismatch");
			return;
		}
		break;
	case GFXATI_ID_AT25F:
		if (id[0] != chip->id_bytes[0] || id[1] != chip->id_bytes[1]) {
			fail(chip, "AT25F probe mismatch");
			return;
		}
		break;
	default:
		if (chip->quirks & GFXATI_Q_NO_RDID) {
			if (id[0] != chip->id_bytes[0] || id[1] != chip->id_bytes[chip->id_len - 1]) {
				fail(chip, "REMS probe mismatch");
				return;
			}
		} else if (id[0] != chip->id_bytes[0] || id[1] != chip->id_bytes[1] ||
			   (chip->id_len > 2 && id[2] != chip->id_bytes[2])) {
			fail(chip, "RDID probe mismatch");
			return;
		}
		break;
	}

	if (rdsr(&f) != 0x00) {
		fail(chip, "status not clean at power-up");
		return;
	}
	wren(&f);
	if (!(rdsr(&f) & 0x02)) {
		fail(chip, "WREN did not set WEL");
		return;
	}
	CMD(&f, 0x04);
	if (rdsr(&f) & 0x02) {
		fail(chip, "WRDI did not clear WEL");
		return;
	}

	memset(mem, 0x00, chip->size);
	{
		gfxati_flash_cs(&f, 1);
		gfxati_flash_spi_xfer(&f, 0x03);
		gfxati_flash_spi_xfer(&f, 0x00);
		gfxati_flash_spi_xfer(&f, 0x12);
		gfxati_flash_spi_xfer(&f, 0x34);
		if (chip->quirks & GFXATI_Q_AT45) {
			gfxati_flash_spi_xfer(&f, 0);
			gfxati_flash_spi_xfer(&f, 0);
		}
		uint8_t v = gfxati_flash_spi_xfer(&f, 0);
		gfxati_flash_cs(&f, 0);
		if (v != 0x00) {
			fail(chip, "READ");
			return;
		}
	}

	memset(mem, 0xFF, chip->size);
	wren(&f);
	pp(&f, 0x1234, 0xA5);
	if (mem[0x1234] != 0xA5) {
		fail(chip, "PP");
		return;
	}
	wren(&f);
	pp(&f, 0x1234, 0x3C);
	if (mem[0x1234 % chip->size] != 0x24) {
		fail(chip, "PP did not AND");
		return;
	}
	CMD(&f, 0x04);
	pp(&f, 0x1234, 0xFF);
	if (mem[0x1234 % chip->size] != 0x24) {
		fail(chip, "PP without WREN wrote");
		return;
	}

	if (chip->page_size == 256) {
		wren(&f);
		CMD(&f, 0x02, 0x00, 0x00, 0xFE, 0x11, 0x22, 0x33);
		if (mem[0x00FE] != 0x11 || mem[0x00FF] != 0x22 || mem[0x0000] != 0x33 ||
		    mem[0x0100] != 0xFF) {
			fail(chip, "PP page wrap");
			return;
		}
	}

	wren(&f);
	pp(&f, 0x12500, 0x66);
	if (chip->erase_cmds & GFXATI_ERASE_4K) {
		wren(&f);
		CMD(&f, 0x20, 0x01, 0x20, 0x00);
		if (mem[0x12500 % chip->size] != 0xFF || mem[0x124FF % chip->size] != 0xFF || mem[0x1234 % chip->size] != 0x24) {
			fail(chip, "SE 4K geometry");
			return;
		}
	} else {
		wren(&f);
		CMD(&f, 0x20, 0x01, 0x20, 0x00);
		if (mem[0x12500 % chip->size] != 0x66) {
			fail(chip, "SE accepted without 4K erase");
			return;
		}
	}

	wren(&f);
	pp(&f, 0x8000, 0x77);
	wren(&f);
	pp(&f, 0x12500, 0x66);
	if (chip->erase_cmds & GFXATI_ERASE_D8_32K) {
		wren(&f);
		CMD(&f, 0xD8, 0x00, 0x00, 0x00);
		if (mem[0x7FFF % chip->size] != 0xFF || mem[0x8000 % chip->size] != 0x77) {
			fail(chip, "D8 32K geometry");
			return;
		}
	} else if (chip->erase_cmds & GFXATI_ERASE_64K) {
		wren(&f);
		CMD(&f, 0xD8, 0x00, 0x00, 0x00);
		if (chip->size > 64 * 1024) {
			if (mem[0xFFFF % chip->size] != 0xFF || mem[0x12500 % chip->size] != 0x66) {
				fail(chip, "D8 64K geometry");
				return;
			}
		} else if (mem[0x1234 % chip->size] != 0xFF || mem[0x8000 % chip->size] != 0xFF) {
			fail(chip, "D8 whole-chip geometry");
			return;
		}
	} else {
		wren(&f);
		CMD(&f, 0xD8, 0x00, 0x00, 0x00);
		if (mem[0x1234 % chip->size] != 0x24) {
			fail(chip, "D8 accepted without 64K erase");
			return;
		}
	}

	if (chip->erase_cmds & GFXATI_ERASE_CHIP) {
		wren(&f);
		CMD(&f, 0xC7);
		if (!all_ff(mem, chip->size)) {
			fail(chip, "CE left data");
			return;
		}
	}

	if (chip->quirks & GFXATI_Q_AAI) {
		wren(&f);
		CMD(&f, 0xAD, 0x00, 0x40, 0x00, 0x11, 0x22);
		CMD(&f, 0xAD, 0x33, 0x44);
		if (mem[0x4000] != 0x11 || mem[0x4001] != 0x22 ||
		    mem[0x4002] != 0x33 || mem[0x4003] != 0x44) {
			fail(chip, "AAI word program");
			return;
		}
		CMD(&f, 0x04);
		CMD(&f, 0xAD, 0x55, 0x66);
		if (mem[0x4004] != 0xFF) {
			fail(chip, "AAI continued after WRDI");
			return;
		}
	}

	if (chip->quirks & GFXATI_Q_EWSR) {
		CMD(&f, 0x04);
		CMD(&f, 0x01, 0x3C);
		if (rdsr(&f) != 0x00) {
			fail(chip, "WRSR accepted without EWSR");
			return;
		}
		wren(&f);
		CMD(&f, 0x50);
		CMD(&f, 0x01, 0x3C);
		CMD(&f, 0x04);
		if (rdsr(&f) != 0x3C) {
			fail(chip, "WRSR after EWSR");
			return;
		}
	} else {
		CMD(&f, 0x04);
		CMD(&f, 0x01, 0x3C);
		if (rdsr(&f) != 0x3C) {
			fail(chip, "WRSR");
			return;
		}
	}

	if (chip->quirks & GFXATI_Q_AT45) {
		gfxati_flash_init(&f, chip, mem);
		memset(mem, 0xFF, chip->size);

		wren(&f);
		CMD(&f, 0x84, 0x00, 0x00, 0x00, 0xA0, 0xA1, 0xA2, 0xA3,
		    0xA4, 0xA5, 0xA6, 0xA7);
		CMD(&f, 0x83, 0x00, 0x02, 0x00);
		{
			gfxati_flash_cs(&f, 1);
			gfxati_flash_spi_xfer(&f, 0xD2);
			gfxati_flash_spi_xfer(&f, 0x00);
			gfxati_flash_spi_xfer(&f, 0x02);
			gfxati_flash_spi_xfer(&f, 0x00);
			if (gfxati_flash_spi_xfer(&f, 0) != 0xA0 ||
			    gfxati_flash_spi_xfer(&f, 0) != 0xA1) {
				gfxati_flash_cs(&f, 0);
				fail(chip, "AT45 buffer to page read");
				return;
			}
			gfxati_flash_cs(&f, 0);
		}
		wren(&f);
		CMD(&f, 0x81, 0x00, 0x02, 0x00);
		if (mem[264] != 0xFF) {
			fail(chip, "AT45 page erase");
			return;
		}
		wren(&f);
		CMD(&f, 0x50, 0x00, 0x00, 0x00, 0x00, 0x00);
		if (mem[264] != 0xFF || mem[0] != 0xFF) {
			fail(chip, "AT45 block erase");
			return;
		}
	}

	printf("PASS %-11s\n", chip->name);
}

fn test_parallel_chip(const struct gfxati_flash_chip *chip)
{
	struct gfxati_flash f;

	gfxati_flash_init(&f, chip, mem);
	memset(mem, 0xFF, chip->size);

	gfxati_flash_parallel_write(&f, 0x5555, 0xAA);
	gfxati_flash_parallel_write(&f, 0x2AAA, 0x55);
	gfxati_flash_parallel_write(&f, 0x5555, 0x90);
	if (chip->id_len >= 2) {
		if (gfxati_flash_parallel_read(&f, 0) != chip->id_bytes[0] ||
		    gfxati_flash_parallel_read(&f, 1) != chip->id_bytes[1]) {
			fail(chip, "JEDEC id mismatch");
			return;
		}
	}
	gfxati_flash_parallel_write(&f, 0x5555, 0xF0);
	if (gfxati_flash_parallel_read(&f, 0) != 0xFF) {
		fail(chip, "id mode did not exit");
		return;
	}

	gfxati_flash_parallel_write(&f, 0x5555, 0xAA);
	gfxati_flash_parallel_write(&f, 0x2AAA, 0x55);
	gfxati_flash_parallel_write(&f, 0x5555, 0xA0);
	gfxati_flash_parallel_write(&f, 0x1234, 0x5A);
	gfxati_flash_parallel_write(&f, 0x5555, 0xAA);
	gfxati_flash_parallel_write(&f, 0x2AAA, 0x55);
	gfxati_flash_parallel_write(&f, 0x5555, 0xA0);
	gfxati_flash_parallel_write(&f, 0x4000, 0xB5);
	if (mem[0x1234] != 0x5A || mem[0x4000] != 0xB5) {
		fail(chip, "program");
		return;
	}

	if (chip->erase_cmds & GFXATI_ERASE_4K) {
		gfxati_flash_parallel_write(&f, 0x5555, 0xAA);
		gfxati_flash_parallel_write(&f, 0x2AAA, 0x55);
		gfxati_flash_parallel_write(&f, 0x5555, 0x80);
		gfxati_flash_parallel_write(&f, 0x5555, 0xAA);
		gfxati_flash_parallel_write(&f, 0x2AAA, 0x55);
		gfxati_flash_parallel_write(&f, 0x1000, 0x30);
		if (mem[0x1234] != 0xFF || mem[0x0FFF] != 0xFF || mem[0x4000] != 0xB5) {
			fail(chip, "sector erase 4K");
			return;
		}
	} else if (chip->erase_cmds & GFXATI_ERASE_16K) {
		gfxati_flash_parallel_write(&f, 0x5555, 0xAA);
		gfxati_flash_parallel_write(&f, 0x2AAA, 0x55);
		gfxati_flash_parallel_write(&f, 0x5555, 0x80);
		gfxati_flash_parallel_write(&f, 0x5555, 0xAA);
		gfxati_flash_parallel_write(&f, 0x2AAA, 0x55);
		gfxati_flash_parallel_write(&f, 0x0000, 0x30);
		if (mem[0x1234] != 0xFF || mem[0x4000] != 0xB5) {
			fail(chip, "sector erase 16K");
			return;
		}
	} else {
		gfxati_flash_parallel_write(&f, 0x5555, 0xAA);
		gfxati_flash_parallel_write(&f, 0x2AAA, 0x55);
		gfxati_flash_parallel_write(&f, 0x5555, 0x80);
		gfxati_flash_parallel_write(&f, 0x5555, 0xAA);
		gfxati_flash_parallel_write(&f, 0x2AAA, 0x55);
		gfxati_flash_parallel_write(&f, 0x10, 0x30);
		if (!all_ff(mem, chip->size)) {
			fail(chip, "sector erase cleared chip");
			return;
		}
	}

	gfxati_flash_parallel_write(&f, 0x5555, 0xAA);
	gfxati_flash_parallel_write(&f, 0x2AAA, 0x55);
	gfxati_flash_parallel_write(&f, 0x5555, 0xA0);
	gfxati_flash_parallel_write(&f, 0x1234, 0x5A);
	gfxati_flash_parallel_write(&f, 0x5555, 0xAA);
	gfxati_flash_parallel_write(&f, 0x2AAA, 0x55);
	gfxati_flash_parallel_write(&f, 0x5555, 0x80);
	gfxati_flash_parallel_write(&f, 0x5555, 0xAA);
	gfxati_flash_parallel_write(&f, 0x2AAA, 0x55);
	gfxati_flash_parallel_write(&f, 0x5555, 0x10);
	if (!all_ff(mem, chip->size)) {
		fail(chip, "chip erase");
		return;
	}

	printf("PASS %-11s\n", chip->name);
}

int main(void)
{
	for (unsigned int i = 0; i < gfxati_flash_chip_count; i++) {
		const struct gfxati_flash_chip *chip = &gfxati_flash_chips[i];

		if (chip->bus == GFXATI_SPI) {
			test_spi_chip(chip);
		} else {
			test_parallel_chip(chip);
		}
	}

	if (failures) {
		fprintf(stderr, "%d failure(s)\n", failures);
		return 1;
	}
	printf("%u chips ok\n", gfxati_flash_chip_count);
	return 0;
}
