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

	/* The GPIO-block I2C bus: emulate the atiflash master (af349's
	 * det_si2ccfg polarity - SCL and SDA are EN bits, set = driven
	 * low) through a full address byte and check the slave ACKs the
	 * 0x39 device (byte 0x72) by pulling SDA low on the 9th clock. */
	{
		int bit;

		gfxati_card_mmio_write(&card, GFXATI_GPIO_EN, 0);
		gfxati_card_mmio_write(&card, GFXATI_GPIO_EN,
				       GFXATI_GPIO_SDA_EN);
		check(1, "START condition accepted");

		for (bit = 7; bit >= 0; bit--) {
			int level = (0x72 >> bit) & 1;
			uint32_t data = level ? 0 : GFXATI_GPIO_SDA_EN;

			gfxati_card_mmio_write(&card, GFXATI_GPIO_EN,
					       GFXATI_GPIO_SCL_EN | data);
			gfxati_card_mmio_write(&card, GFXATI_GPIO_EN, data);
		}
		/* 9th clock: master releases SDA, the slave pulls it low */
		gfxati_card_mmio_write(&card, GFXATI_GPIO_EN, 0);
		check(!(gfxati_card_mmio_read(&card, GFXATI_GPIO_Y) &
			GFXATI_GPIO_SDA_EN),
		      "I2C slave ACKs device address 0x72");
		gfxati_card_mmio_write(&card, GFXATI_GPIO_EN, GFXATI_GPIO_SCL_EN);
		check((gfxati_card_mmio_read(&card, GFXATI_GPIO_Y) &
		       GFXATI_GPIO_SDA_EN),
		      "SDA released after the ACK clock");
	}

	/* The SPI command window on a serial chip (the R580's own flash
	 * family): status window, opcode triggers, and the identification
	 * latch that serves an ID-class trigger's answer through the
	 * array window (plan/0004#transport). */
	{
		const struct gfxati_flash_chip *spi = &gfxati_flash_chips[3];

		gfxati_card_init(&card, GFXATI_DEV_R580_A, spi, rom);
		check(card.flash.chip == spi, "SPI chip bound");

		/* status window: RDSR with no WREN yet */
		gfxati_card_mmio_write(&card, GFXATI_MM_INDEX,
				       GFXATI_SEPROM_CNTL1);
		gfxati_card_mmio_write(&card, GFXATI_MM_DATA,
				       0x09000000 | 0x010);
		check(gfxati_card_rom_read(&card, 0) == 0,
		      "status window answers RDSR (idle)");

		/* WREN via opcode trigger, then the status window shows WEL */
		gfxati_card_mmio_write(&card, GFXATI_MM_INDEX,
				       GFXATI_SEPROM_CNTL2);
		gfxati_card_mmio_write(&card, GFXATI_MM_DATA, 0x06 << 16);
		gfxati_card_mmio_write(&card, GFXATI_MM_INDEX,
				       GFXATI_SEPROM_CNTL1);
		gfxati_card_mmio_write(&card, GFXATI_MM_DATA,
				       0x09000000 | 0x001);
		gfxati_card_rom_write(&card, 0, 0);
		gfxati_card_mmio_write(&card, GFXATI_MM_DATA,
				       0x09000000 | 0x010);
		check(gfxati_card_rom_read(&card, 0) & 0x02,
		      "WREN trigger sets WEL in the status window");

		/* identification: AT25F product-ID trigger (15H scheme),
		 * answer latched and served through the array window */
		gfxati_card_mmio_write(&card, GFXATI_MM_INDEX,
				       GFXATI_SEPROM_CNTL2);
		gfxati_card_mmio_write(&card, GFXATI_MM_DATA, 0x15 << 16);
		gfxati_card_mmio_write(&card, GFXATI_MM_INDEX,
				       GFXATI_SEPROM_CNTL1);
		gfxati_card_mmio_write(&card, GFXATI_MM_DATA,
				       0x09000000 | 0x001);
		gfxati_card_rom_write(&card, 0, 0);
		gfxati_card_mmio_write(&card, GFXATI_MM_DATA, GFXATI_CS_BIT);
		check(gfxati_card_rom_read(&card, 0) == 0x1F &&
		      gfxati_card_rom_read(&card, 1) == 0x60,
		      "product-ID trigger latches 1F 60 for the array window");

		/* REMS trigger normalizes to manufacturer+device */
		gfxati_card_mmio_write(&card, GFXATI_MM_INDEX,
				       GFXATI_SEPROM_CNTL2);
		gfxati_card_mmio_write(&card, GFXATI_MM_DATA, 0x90 << 16);
		gfxati_card_mmio_write(&card, GFXATI_MM_INDEX,
				       GFXATI_SEPROM_CNTL1);
		gfxati_card_mmio_write(&card, GFXATI_MM_DATA,
				       0x09000000 | 0x001);
		gfxati_card_rom_write(&card, 0, 0);
		gfxati_card_mmio_write(&card, GFXATI_MM_DATA, GFXATI_CS_BIT);
		check(gfxati_card_rom_read(&card, 0) == 0x1F &&
		      gfxati_card_rom_read(&card, 1) == 0x60,
		      "REMS trigger latches the same id pair");

		/* a window write ends the identification */
		gfxati_card_rom_write(&card, 0x10000, 0);
		check(gfxati_card_rom_read(&card, 0) == rom[0],
		      "identification ends on window write");

		/* program stream: arm, write, close - and the byte lands */
		rom[0x80] = 0xFF;
		gfxati_card_mmio_write(&card, GFXATI_MM_INDEX,
				       GFXATI_SEPROM_CNTL1);
		gfxati_card_mmio_write(&card, GFXATI_MM_DATA,
				       0x09000000 | 0x200 | (1 << 16));
		gfxati_card_rom_write(&card, 0x80, 0x5A);
		gfxati_card_mmio_write(&card, GFXATI_MM_DATA, GFXATI_CS_BIT);
		check(gfxati_card_rom_read(&card, 0x80) == 0x5A,
		      "program stream writes through the window");
	}

	if (failures) {
		printf("%d failure(s)\n", failures);
		return 1;
	}
	printf("gfxati card ok\n");
	return 0;
}
