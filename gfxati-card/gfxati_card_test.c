#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "gfxati_card.h"

static int failures;

static void check(int cond, const char *what)
{
	if (!cond) {
		printf("FAIL: %s\n", what);
		failures++;
	} else {
		printf("ok: %s\n", what);
	}
}

int main(void)
{
	static uint8_t rom[256 * 1024];
	const struct gfxati_flash_chip *chip = &gfxati_flash_chips[9];
	struct gfxati_card card;

	for (unsigned i = 0; i < sizeof(rom); i++) {
		rom[i] = 0xAA;
	}

	gfxati_card_init(&card, GFXATI_DEV_R580_A, chip, rom);

	check(card.device_id == GFXATI_DEV_R580_A, "device identity 0x7240");

	gfxati_card_mmio_write(&card, GFXATI_MM_INDEX, GFXATI_SEPROM_CNTL1);
	check(gfxati_card_mmio_read(&card, GFXATI_MM_INDEX) == GFXATI_SEPROM_CNTL1,
	      "MM_INDEX returns the index");
	gfxati_card_mmio_write(&card, GFXATI_MM_DATA, 0x1234);
	check(gfxati_card_mmio_read(&card, GFXATI_MM_DATA) == 0x1234,
	      "SEPROM_CNTL1 roundtrip through MM_DATA");

	gfxati_card_mmio_write(&card, GFXATI_MM_INDEX, GFXATI_SEPROM_CNTL2);
	gfxati_card_mmio_write(&card, GFXATI_MM_DATA, 0x5678);
	check(gfxati_card_mmio_read(&card, GFXATI_MM_DATA) == 0x5678,
	      "SEPROM_CNTL2 roundtrip through MM_DATA");

	gfxati_card_mmio_write(&card, GFXATI_MM_INDEX, GFXATI_SEPROM_CNTL1);
	gfxati_card_mmio_write(&card, GFXATI_MM_DATA, GFXATI_CS_BIT);
	check(gfxati_card_mmio_read(&card, GFXATI_MM_DATA) == GFXATI_CS_BIT,
	      "SEPROM_CNTL1 carries the CS bit 0x400");

	gfxati_card_mmio_write(&card, GFXATI_I2C_CTL, 0xE7);
	gfxati_card_mmio_write(&card, GFXATI_I2C_LEN, 0xF90100);
	gfxati_card_mmio_write(&card, GFXATI_I2C_DATA, 0x42);
	check(gfxati_card_mmio_read(&card, GFXATI_I2C_CTL) == 0xE7,
	      "I2C control register responds");
	check(gfxati_card_mmio_read(&card, GFXATI_I2C_LEN) == 0xF90100,
	      "I2C length/flags register responds");
	check(gfxati_card_mmio_read(&card, GFXATI_I2C_DATA) == 0x42,
	      "I2C data byte responds");

	gfxati_card_mmio_write(&card, 0x3C0, 1);
	check(gfxati_card_mmio_read(&card, 0x3C0) == 0, "unmapped MMIO reads zero");

	check(gfxati_card_rom_read(&card, 0) == 0xAA, "ROM window serves the array");
	check(gfxati_card_rom_read(&card, chip->size - 1) == 0xAA,
	      "ROM window serves the array at the top");

	gfxati_card_rom_write(&card, 0x5555, 0xAA);
	gfxati_card_rom_write(&card, 0x2AAA, 0x55);
	gfxati_card_rom_write(&card, 0x5555, 0x90);
	check(gfxati_card_rom_read(&card, 0) == chip->id_bytes[0],
	      "JEDEC id mode serves the manufacturer byte");
	check(gfxati_card_rom_read(&card, 1) == chip->id_bytes[1],
	      "JEDEC id mode serves the device byte");
	gfxati_card_rom_write(&card, 0x5555, 0xF0);
	check(gfxati_card_rom_read(&card, 0) == 0xAA,
	      "id mode exits back to the array");

	gfxati_card_rom_write(&card, 0x5555, 0xAA);
	gfxati_card_rom_write(&card, 0x2AAA, 0x55);
	gfxati_card_rom_write(&card, 0x5555, 0xA0);
	gfxati_card_rom_write(&card, 0x100, 0x0A);
	check(gfxati_card_rom_read(&card, 0x100) == 0x0A,
	      "byte program through the ROM window (AND semantics)");

	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("gfxati card ok\n");
	return 0;
}
