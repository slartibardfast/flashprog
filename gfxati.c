/*
 * This file is part of the flashprog project.
 *
 * Copyright (C) 2026 the agentic-flashprog project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*
 * The gfxati programmer: ATI R5xx-generation GPU ROM flashing through
 * the card's own SEPROM interface (plan/0004, implementing the port
 * specification from plan/0002#port-spec).
 *
 * The interface, rig-verified against the QEMU gfxati-card stub by
 * thirteen builds of the original atiflash (3.25 through 4.07):
 *
 *   - BAR0 MMIO carries the MM_INDEX 0xA0 / MM_DATA 0xA4 indirect
 *     pair; every register access is index -> 0xA0, data -> 0xA4,
 *     re-assert the index.
 *   - SEPROM_CNTL1 / CNTL2 live at indirect indices 0x1C0 / 0x1C4
 *     (the R6xx-class cards map them at the direct MMIO offsets of
 *     the same value; this driver uses the indirect pair, the
 *     R5xx-native route).
 *   - CNTL1 bit 0x400 is the flash chip select (set = deasserted);
 *     CNTL1 read-back AND 0x1100 is the busy status.
 *   - The command window: CNTL1 = 0x09000000 | sub arms a mode -
 *       sub 0x000/0x200  program stream (burst count bits 16-23)
 *       sub 0x010         status window (reads answer RDSR)
 *       anything else     opcode trigger: CNTL2 bits 16-23 carry the
 *                         opcode, and the next window write's byte is
 *                         the command's data operand
 *   - The PCI ROM BAR is the data window: with no mode armed it
 *     serves the flash array directly (the read path); ID-class
 *     triggers latch their answer for array reads at offsets 0, 1
 *     and 0x0E/0x0F (the parallel family's in_id_mode convention).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "programmer.h"
#include "hwaccess_physmap.h"
#include "platform/pci.h"
#include "spi.h"

#define PCI_VENDOR_ID_ATI		0x1002

#define GFXATI_MM_INDEX			0xA0
#define GFXATI_MM_DATA			0xA4
#define GFXATI_SEPROM_CNTL1_INDEX	0x1C0
#define GFXATI_SEPROM_CNTL2_INDEX	0x1C4
#define GFXATI_REG_0E4			0x0E4

#define GFXATI_CS_BIT			0x400
#define GFXATI_BUSY_BITS		0x1100

#define GFXATI_WINDOW_ARM		0x09000000
#define GFXATI_WIN_PROGRAM		0x09000000	/* sub 0x000/0x200 */
#define GFXATI_WIN_STATUS		0x09000010	/* sub 0x010 */
#define GFXATI_WIN_TRIGGER		0x09000001	/* any other sub */

#define GFXATI_MMIO_SIZE		0x8000
#define GFXATI_ROM_WINDOW_SIZE		0x20000

/* The strap register's flash-family table (register 0xE4 shift 4,
 * mask 0xF) - logged as a hint, never trusted as a verdict. */
static const char *const gfxati_strap_names[16] = {
	[0] = "no ROM",
	[1] = "parallel legacy",
	[2] = "Rialto parallel",
	[3] = "parallel + VID",
	[4] = "AT25F-class serial",
	[5] = "STM-class serial",
	[6] = "SST-class serial",
	[7] = "SST-class serial",
	[8] = "parallel/VID",
	[9] = "AT25F1024/C serial (R580)",
	[0xD] = "R600 SPI",
};

static uint8_t *gfxati_mmio;
static uint8_t *gfxati_romwin;

/* The indirect pair: index, data, re-assert the index. */
static uint32_t gfxati_reg_read(uint32_t index)
{
	pci_mmio_writel(index, gfxati_mmio + GFXATI_MM_INDEX);
	pci_mmio_writel(index, gfxati_mmio + GFXATI_MM_INDEX);
	return pci_mmio_readl(gfxati_mmio + GFXATI_MM_DATA);
}

static void gfxati_reg_write(uint32_t index, uint32_t val)
{
	pci_mmio_writel(index, gfxati_mmio + GFXATI_MM_INDEX);
	pci_mmio_writel(val, gfxati_mmio + GFXATI_MM_DATA);
	pci_mmio_writel(index, gfxati_mmio + GFXATI_MM_INDEX);
}

static void gfxati_cntl1(uint32_t val)
{
	gfxati_reg_write(GFXATI_SEPROM_CNTL1_INDEX, val);
}

static void gfxati_cntl2(uint32_t val)
{
	gfxati_reg_write(GFXATI_SEPROM_CNTL2_INDEX, val);
}

/* The busy status never sticks on the rig's cards (every atiflash
 * poll loop observed it clear), but the port must still poll. */
static int gfxati_wait_busy(void)
{
	int i;
	for (i = 0; i < 1000; i++) {
		if (!(gfxati_reg_read(GFXATI_SEPROM_CNTL1_INDEX) & GFXATI_BUSY_BITS))
			return 0;
		internal_delay(10);
	}
	msg_perr("gfxati: SEPROM busy status stuck.\n");
	return 1;
}

/* Arm a window mode, or return to the idle array mode (CS
 * deasserted) when given GFXATI_CS_BIT. */
static void gfxati_arm(uint32_t mode)
{
	gfxati_cntl1(mode);
}

/* Fire an opcode trigger: CNTL2 carries the opcode, one window byte
 * carries the (possible) data operand, and the card closes the
 * chip-select cycle itself. */
static void gfxati_trigger(uint8_t opcode, uint8_t operand)
{
	gfxati_cntl2((uint32_t)opcode << 16);
	gfxati_arm(GFXATI_WIN_TRIGGER);
	gfxati_romwin[0] = operand;
	gfxati_arm(GFXATI_CS_BIT);
}

/* An identification trigger latches its answer; the array window
 * serves it at offsets 0, 1 and (the third byte) 0x0E/0x0F, the
 * parallel family's in_id_mode convention. */
static int gfxati_read_id(uint8_t opcode, unsigned char *readarr, unsigned int readcnt)
{
	unsigned int i;

	gfxati_trigger(opcode, 0);
	gfxati_arm(GFXATI_CS_BIT);
	for (i = 0; i < readcnt; i++) {
		uint32_t off = (i < 2) ? i : 0x0E;
		readarr[i] = gfxati_romwin[off];
	}
	gfxati_arm(GFXATI_CS_BIT);
	return 0;
}

static int gfxati_spi_command(const struct spi_master *mst, unsigned int writecnt,
			      unsigned int readcnt, const unsigned char *writearr,
			      unsigned char *readarr)
{
	uint8_t opcode = writearr[0];

	if (writecnt < 1) {
		msg_perr("gfxati: zero-length command.\n");
		return 1;
	}

	/* The single readable status is RDSR through the status window;
	 * every read answers a fresh transaction. */
	if (opcode == 0x05 && readcnt >= 1 && writecnt == 1) {
		unsigned int i;

		gfxati_arm(GFXATI_WIN_STATUS);
		for (i = 0; i < readcnt; i++)
			readarr[i] = gfxati_romwin[0];
		gfxati_arm(GFXATI_CS_BIT);
		return 0;
	}

	/* Identification: JEDEC RDID, the AT25F 15H product-ID read,
	 * REMS, and RES all latch through the array window. */
	if (readcnt >= 1 && (opcode == 0x9f || opcode == 0x15 ||
			     opcode == 0x90 || opcode == 0xab))
		return gfxati_read_id(opcode, readarr, readcnt);

	/* Array reads: with no mode armed the ROM window serves the
	 * flash directly; the address comes from the command. */
	if (readcnt >= 1 && opcode == 0x03 && writecnt >= 4) {
		uint32_t addr = ((uint32_t)writearr[1] << 16) |
				((uint32_t)writearr[2] << 8) | writearr[3];
		unsigned int i;

		gfxati_arm(GFXATI_CS_BIT);
		for (i = 0; i < readcnt; i++)
			readarr[i] = gfxati_romwin[(addr + i) % GFXATI_ROM_WINDOW_SIZE];
		return 0;
	}

	/* Page program: arm the burst stream with the byte count in
	 * bits 16-23 and write the data at its flash address through
	 * the window; the card opens one long page-program transaction
	 * and the re-arm closes it.
	 * (The window will not open a stream for a write of byte 0 to
	 * offset 0 - the interface neutralizes atiflash's command-reset
	 * write that way; an image whose very first byte is 0x00 at
	 * flash offset 0 cannot be programmed through the stream.) */
	if (opcode == 0x02 && writecnt >= 4) {
		uint32_t addr = ((uint32_t)writearr[1] << 16) |
				((uint32_t)writearr[2] << 8) | writearr[3];
		unsigned int len = writecnt - 4;
		unsigned int i;

		gfxati_arm(GFXATI_WIN_PROGRAM | (len << 16));
		for (i = 0; i < len; i++)
			gfxati_romwin[(addr + i) % GFXATI_ROM_WINDOW_SIZE] = writearr[4 + i];
		gfxati_arm(GFXATI_CS_BIT);
		return gfxati_wait_busy();
	}

	/* Everything else is a single-operand trigger: WREN, WRDI,
	 * WRSR (opcode + value), the Atmel chip erase (the card
	 * completes the 62 87 pair itself), deep power-down. */
	gfxati_trigger(opcode, writecnt > 1 ? writearr[1] : 0);
	return gfxati_wait_busy();
}

static int gfxati_spi_read(struct flashctx *flash, uint8_t *buf,
			   unsigned int start, unsigned int len)
{
	/* The array window is the fast read path: with no mode armed
	 * the ROM BAR is the flash contents as plain memory. */
	gfxati_arm(GFXATI_CS_BIT);
	memcpy(buf, gfxati_romwin + start, len);
	return 0;
}

static const struct spi_master spi_master_gfxati = {
	.max_data_read	= 256,
	.max_data_write	= 256,
	.command	= gfxati_spi_command,
	.read		= gfxati_spi_read,
};

static const struct dev_entry gfxati_devices[] = {
	{0x1002, 0x7100, NT, "ATI", "R520 [ Radeon X1800 ]" },
	{0x1002, 0x7210, NT, "ATI", "R520 [ Radeon X1800 XL/GTO ]" },
	{0x1002, 0x7240, NT, "ATI", "R580 [ Radeon X1900 ]" },
	{0x1002, 0x7243, NT, "ATI", "R580 [ Radeon X1900 GT ]" },
	{0x1002, 0x7248, NT, "ATI", "R580 [ Radeon X1900 XTX ]" },
	{0},
};

static int gfxati_init(struct flashprog_programmer *const prog)
{
	struct pci_dev *dev = NULL;
	uintptr_t mmio_base, rom_base;
	uint32_t rom, strap;
	const char *strap_name;

	dev = pcidev_init(gfxati_devices, PCI_BASE_ADDRESS_0);
	if (!dev)
		return 1;

	mmio_base = pcidev_readbar(dev, PCI_BASE_ADDRESS_0);
	if (!mmio_base)
		return 1;

	gfxati_mmio = rphysmap("ATI R5xx MMIO", mmio_base, GFXATI_MMIO_SIZE);
	if (gfxati_mmio == ERROR_PTR)
		return 1;

	/* The ROM BAR is the command/data window; it must be assigned
	 * and enabled (firmware does this for a VGA-class card - the
	 * rig's SeaBIOS included). */
	rom = pci_read_long(dev, PCI_ROM_ADDRESS);
	if (!(rom & PCI_ROM_ADDRESS_ENABLE)) {
		rom |= PCI_ROM_ADDRESS_ENABLE;
		rpci_write_long(dev, PCI_ROM_ADDRESS, rom);
		rom = pci_read_long(dev, PCI_ROM_ADDRESS);
	}
	rom_base = rom & PCI_ROM_ADDRESS_MASK;
	if (!rom_base) {
		msg_perr("gfxati: ROM BAR has no address assigned; "
			 "cannot reach the flash window.\n");
		return 1;
	}
	gfxati_romwin = rphysmap("ATI R5xx flash window", rom_base,
				 GFXATI_ROM_WINDOW_SIZE);
	if (gfxati_romwin == ERROR_PTR)
		return 1;

	/* The flash strap: a hint from the card about its flash family,
	 * read first and logged, never a verdict (OEM boards
	 * misstrapped; the chip probe decides). */
	strap = pci_mmio_readl(gfxati_mmio + GFXATI_REG_0E4) >> 4 & 0xF;
	strap_name = gfxati_strap_names[strap];
	msg_pinfo("gfxati: strap 0x%X (%s)\n", strap,
		  strap_name ? strap_name : "unknown");

	/* Idle: chip select deasserted, no window mode armed. */
	gfxati_arm(GFXATI_CS_BIT);

	return register_spi_master(&spi_master_gfxati, GFXATI_ROM_WINDOW_SIZE,
				   NULL);
}

const struct programmer_entry programmer_gfxati = {
	.name			= "gfxati",
	.type			= PCI,
	.devs.dev		= gfxati_devices,
	.init			= gfxati_init,
};
