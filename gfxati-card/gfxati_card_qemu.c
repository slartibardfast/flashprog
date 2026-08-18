/*
 * QEMU glue for the gfxati card: a qdev PCI device presenting the R520/R580
 * identity, the MMIO register surface, and the ROM window backed by the
 * flash model. This file compiles only inside a QEMU tree (it is not part
 * of the plain self-test build); the boot attestation recipe is in README.md.
 */
#include "qemu/osdep.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci.h"
#include "hw/qdev-properties.h"
#include "exec/address-spaces.h"
#include "qapi/error.h"
#include "qom/object.h"

#include "gfxati_card.h"

#define TYPE_GFXATI_CARD "gfxati-card"
OBJECT_DECLARE_SIMPLE_TYPE(GfxAtiCard, GFXATI_CARD)

struct GfxAtiCard {
	PCIDevice parent_obj;
	struct gfxati_card card;
	uint8_t *rom_mem;
	MemoryRegion mmio;
	MemoryRegion rom;
	MemoryRegion seprom_win;
	uint32_t device_id;
	uint32_t chip;
	bool debug;
	bool seprom_window_mapped;
};

static void gfxati_seprom_window_update(GfxAtiCard *s)
{
	bool want = (s->card.seprom_cntl1 & GFXATI_SEPROM_WINDOW_TAG) &&
		    (s->card.seprom_cntl1 & 1);
	if (want == s->seprom_window_mapped) {
		return;
	}
	if (want) {
		memory_region_add_subregion(get_system_memory(),
					    s->card.seprom_window,
					    &s->seprom_win);
	} else {
		memory_region_del_subregion(get_system_memory(), &s->seprom_win);
	}
	s->seprom_window_mapped = want;
}

static uint64_t gfxati_mmio_read(void *opaque, hwaddr offset, unsigned size)
{
	GfxAtiCard *s = opaque;
	uint32_t cur;
	unsigned shift = (offset % 4) * 8;
	bool window = (offset == GFXATI_MM_DATA &&
		       s->card.mm_index == GFXATI_ROM_BASE_INDEX) ||
		      (offset == GFXATI_MM_DATA_LEGACY &&
		       s->card.mm_index_legacy == GFXATI_ROM_BASE_INDEX) ||
		      offset == GFXATI_ROM_BASE_DIRECT;
	if (window) {
		cur = pci_get_long(PCI_DEVICE(s)->config + PCI_ROM_ADDRESS) & ~1u;
	} else {
		cur = gfxati_card_mmio_read(&s->card, offset);
	}
	if (s->debug) {
		fprintf(stderr, "gfxati mmio read  %03" HWADDR_PRIx " idx=%02x => %08x (size %u)\n",
			offset, s->card.mm_index, cur, size);
	}
	return (cur >> shift) & ((1ull << (8 * size)) - 1);
}

static void gfxati_mmio_write(void *opaque, hwaddr offset, uint64_t val,
			      unsigned size)
{
	GfxAtiCard *s = opaque;
	uint32_t cur = gfxati_card_mmio_read(&s->card, offset);
	unsigned shift = (offset % 4) * 8;
	uint32_t mask = ((1ull << (8 * size)) - 1) << shift;
	if (s->debug) {
		fprintf(stderr, "gfxati mmio write %03" HWADDR_PRIx " idx=%02x <= %08x (size %u)\n",
			offset, s->card.mm_index, (uint32_t)val, size);
	}
	if ((offset == GFXATI_MM_DATA && s->card.mm_index == GFXATI_ROM_BASE_INDEX) ||
	    (offset == GFXATI_MM_DATA_LEGACY &&
	     s->card.mm_index_legacy == GFXATI_ROM_BASE_INDEX)) {
		return;
	}
	cur = (cur & ~mask) | ((val << shift) & mask);
	gfxati_card_mmio_write(&s->card, offset, cur);
	gfxati_seprom_window_update(s);
}

static uint64_t gfxati_rom_read(void *opaque, hwaddr offset, unsigned size)
{
	GfxAtiCard *s = opaque;
	uint64_t v = 0;
	unsigned i;
	if (s->debug) {
		fprintf(stderr, "gfxati rom  read  %05" HWADDR_PRIx " size %u\n", offset, size);
	}
	for (i = 0; i < size; i++) {
		v |= (uint64_t)gfxati_card_rom_read(&s->card, offset + i)
		     << (8 * i);
	}
	if (s->debug) {
		fprintf(stderr, "gfxati rom  read  %05" HWADDR_PRIx " => %08" PRIx64 "\n", offset, v);
	}
	return v;
}

static void gfxati_rom_write(void *opaque, hwaddr offset, uint64_t val,
			     unsigned size)
{
	GfxAtiCard *s = opaque;
	unsigned i;
	if (s->debug) {
		fprintf(stderr, "gfxati rom  write %05" HWADDR_PRIx " <= %08" PRIx64 " size %u\n",
			offset, val, size);
	}
	for (i = 0; i < size; i++) {
		gfxati_card_rom_write(&s->card, offset + i, val >> (8 * i));
	}
}

static const MemoryRegionOps gfxati_mmio_ops = {
	.read = gfxati_mmio_read,
	.write = gfxati_mmio_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.impl.min_access_size = 1,
	.impl.max_access_size = 4,
};

static const MemoryRegionOps gfxati_rom_ops = {
	.read = gfxati_rom_read,
	.write = gfxati_rom_write,
	.endianness = DEVICE_LITTLE_ENDIAN,
	.impl.min_access_size = 1,
	.impl.max_access_size = 4,
};

static void gfxati_config_write(PCIDevice *pci_dev, uint32_t addr,
				uint32_t val, int len)
{
	GfxAtiCard *s = GFXATI_CARD(pci_dev);
	if (s->debug) {
		fprintf(stderr, "gfxati config write %02x <= %08x (len %d)\n", addr, val, len);
	}
	pci_default_write_config(pci_dev, addr, val, len);
}

static uint32_t gfxati_config_read(PCIDevice *pci_dev, uint32_t addr, int len)
{
	GfxAtiCard *s = GFXATI_CARD(pci_dev);
	uint32_t val = pci_default_read_config(pci_dev, addr, len);
	if (s->debug && (addr == 0x30 || addr == 0x04 || addr == 0x02)) {
		fprintf(stderr, "gfxati config read  %02x => %08x (len %d)\n", addr, val, len);
	}
	return val;
}

static void gfxati_realize(PCIDevice *pci_dev, Error **errp)
{
	GfxAtiCard *s = GFXATI_CARD(pci_dev);
	const struct gfxati_flash_chip *chip = NULL;
	hwaddr rom_size;
	unsigned i;

	for (i = 0; i < gfxati_flash_chip_count; i++) {
		if (gfxati_flash_chips[i].bus == GFXATI_PARALLEL) {
			chip = &gfxati_flash_chips[i];
			break;
		}
	}
	if (s->chip < gfxati_flash_chip_count) {
		chip = &gfxati_flash_chips[s->chip];
	}
	if (!chip) {
		error_setg(errp, "no flash chip");
		return;
	}

	s->rom_mem = g_malloc0(chip->size);
	memset(s->rom_mem, 0xFF, chip->size);
	gfxati_card_init(&s->card, s->device_id, chip, s->rom_mem);

	memory_region_init_io(&s->mmio, OBJECT(s), &gfxati_mmio_ops, s,
			      "gfxati-mmio", GFXATI_MMIO_SIZE);
	pci_register_bar(pci_dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->mmio);

	rom_size = 1;
	while (rom_size < chip->size) {
		rom_size <<= 1;
	}
	memory_region_init_io(&s->rom, OBJECT(s), &gfxati_rom_ops, s,
			      "gfxati-rom", rom_size);
	pci_register_bar(pci_dev, PCI_ROM_SLOT, PCI_BASE_ADDRESS_SPACE_MEMORY,
			 &s->rom);

	memory_region_init_io(&s->seprom_win, OBJECT(s), &gfxati_rom_ops, s,
			      "gfxati-seprom-window", rom_size);

	pci_config_set_vendor_id(pci_dev->config, GFXATI_VENDOR_ID);
	pci_config_set_device_id(pci_dev->config, s->device_id);
	pci_config_set_class(pci_dev->config, PCI_CLASS_DISPLAY_VGA);
}

static Property gfxati_props[] = {
	DEFINE_PROP_UINT32("device_id", GfxAtiCard, device_id, GFXATI_DEV_R580_A),
	DEFINE_PROP_UINT32("chip", GfxAtiCard, chip, UINT32_MAX),
	DEFINE_PROP_BOOL("debug", GfxAtiCard, debug, false),
	DEFINE_PROP_END_OF_LIST(),
};

static void gfxati_class_init(ObjectClass *klass, void *data)
{
	DeviceClass *dc = DEVICE_CLASS(klass);
	PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

	k->realize = gfxati_realize;
	k->config_write = gfxati_config_write;
	k->config_read = gfxati_config_read;
	k->vendor_id = GFXATI_VENDOR_ID;
	k->device_id = GFXATI_DEV_R580_A;
	k->class_id = PCI_CLASS_DISPLAY_VGA;
	set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
	device_class_set_props(dc, gfxati_props);
}

static const TypeInfo gfxati_info = {
	.name = TYPE_GFXATI_CARD,
	.parent = TYPE_PCI_DEVICE,
	.instance_size = sizeof(GfxAtiCard),
	.class_init = gfxati_class_init,
	.interfaces = (InterfaceInfo[]) {
		{ INTERFACE_CONVENTIONAL_PCI_DEVICE },
		{ },
	},
};

static void gfxati_register_types(void)
{
	type_register_static(&gfxati_info);
}

type_init(gfxati_register_types)
