/*
 * Bare-metal probe for the gfxati card device, run under QEMU:
 *   qemu-system-x86_64 -machine pc -device gfxati-card -kernel probe.elf -nographic
 * Boots through the multiboot header, scans PCI for the card, maps the
 * MMIO BAR and the ROM window, and prints one GFXATI-* line per check on
 * the serial port (COM1). The attestation greps the serial output.
 */
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;

#define MULTIBOOT_MAGIC 0x1BADB002
#define MULTIBOOT_FLAGS 0x3

__attribute__((section(".multiboot"), aligned(4)))
__attribute__((used)) static const struct {
	uint32_t magic;
	uint32_t flags;
	uint32_t checksum;
} mb_header = { MULTIBOOT_MAGIC, MULTIBOOT_FLAGS,
		-(MULTIBOOT_MAGIC + MULTIBOOT_FLAGS) };

#define COM1 0x3F8

static inline void outb(uint16_t port, uint8_t val)
{
	__asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
	uint8_t val;
	__asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

static inline void outl(uint16_t port, uint32_t val)
{
	__asm__ volatile("outl %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint32_t inl(uint16_t port)
{
	uint32_t val;
	__asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
	return val;
}

static void serial_init(void)
{
	outb(COM1 + 1, 0x00);
	outb(COM1 + 3, 0x80);
	outb(COM1 + 0, 0x01);
	outb(COM1 + 1, 0x00);
	outb(COM1 + 3, 0x03);
	outb(COM1 + 2, 0x01);
}

static void serial_putc(char c)
{
	while (!(inb(COM1 + 5) & 0x20))
		;
	outb(COM1, c);
}

static void print(const char *s)
{
	while (*s)
		serial_putc(*s++);
}

static void print_hex32(uint32_t v)
{
	int i;
	char buf[11];
	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 8; i++) {
		int nib = (v >> (28 - 4 * i)) & 0xF;
		buf[2 + i] = nib < 10 ? '0' + nib : 'A' + nib - 10;
	}
	buf[10] = 0;
	print(buf);
}

static uint32_t pci_read(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off)
{
	outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) |
		    (func << 8) | (off & ~3));
	return inl(0xCFC);
}

static void pci_write(uint8_t bus, uint8_t dev, uint8_t func, uint8_t off,
		      uint32_t val)
{
	outl(0xCF8, 0x80000000u | (bus << 16) | (dev << 11) |
		    (func << 8) | (off & ~3));
	outl(0xCFC, val);
}

static uint32_t mmio_read32(uint32_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static void mmio_write32(uint32_t base, uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(base + off) = val;
}

static void check_ok(const char *what)
{
	print("GFXATI-OK ");
	print(what);
	print("\n");
}

static void check_fail(const char *what)
{
	print("GFXATI-FAIL ");
	print(what);
	print("\n");
	for (;;)
		__asm__ volatile("cli; hlt");
}

void main(void)
{
	uint8_t dev;
	uint32_t id = 0;
	uint32_t rom_base;
	uint32_t i;

	serial_init();
	print("GFXATI-PROBE begin\n");

	for (dev = 0; dev < 32; dev++) {
		id = pci_read(0, dev, 0, 0);
		if ((id & 0xFFFF) == 0x1002) {
			uint32_t did = id >> 16;
			if (did == 0x7100 || did == 0x7210 || did == 0x7240 ||
			    did == 0x7243 || did == 0x7248)
				break;
		}
	}
	if (dev == 32)
		check_fail("no gfxati card on bus 0");
	{
		uint32_t w, r;
		uint32_t class;
		uint32_t bar0;

		print("GFXATI-DEV ");
		print_hex32(id >> 16);
		print(" at bus 0 dev ");
		print_hex32(dev);
		print("\n");

		class = pci_read(0, dev, 0, 0x08);
		if ((class >> 16) != 0x0300)
			check_fail("not a VGA-class device");
		check_ok("class 0x030000");

		pci_write(0, dev, 0, 0x04, 0x0007);
		w = pci_read(0, dev, 0, 0x04);
		if (!(w & 0x3))
			check_fail("memory/io enable not accepted");

		pci_write(0, dev, 0, 0x10, 0xFFFFFFFF);
		w = pci_read(0, dev, 0, 0x10);
		if ((w & 0x3FF) != 0)
			check_fail("MMIO BAR0 not 1KB-aligned");
		pci_write(0, dev, 0, 0x10, 0x20000000);

		bar0 = pci_read(0, dev, 0, 0x10);
		if ((bar0 & 1) || (bar0 & 0x40000000))
			check_fail("MMIO BAR0 not a 32-bit memory BAR");

		mmio_write32(0x20000000, 0xA0, 0x1C0);
		r = mmio_read32(0x20000000, 0xA0);
		if (r != 0x1C0)
			check_fail("MM_INDEX does not respond");
		mmio_write32(0x20000000, 0xA4, 0x1234);
		r = mmio_read32(0x20000000, 0xA4);
		if (r != 0x1234)
			check_fail("SEPROM_CNTL1 does not respond");
		mmio_write32(0x20000000, 0xA0, 0x1C4);
		mmio_write32(0x20000000, 0xA4, 0x5678);
		r = mmio_read32(0x20000000, 0xA4);
		if (r != 0x5678)
			check_fail("SEPROM_CNTL2 does not respond");
		check_ok("seprom cntl1/2 respond");

		mmio_write32(0x20000000, 0x3E0, 0xE7);
		mmio_write32(0x20000000, 0x3E4, 0xF90100);
		mmio_write32(0x20000000, 0x3E8, 0x42);
		r = mmio_read32(0x20000000, 0x3E8);
		if (r != 0x42)
			check_fail("I2C engine does not respond");
		check_ok("i2c engine responds");

		pci_write(0, dev, 0, 0x30, 0xFFFFFFFF);
		w = pci_read(0, dev, 0, 0x30);
		pci_write(0, dev, 0, 0x30, 0x30000000u | 1);

		rom_base = pci_read(0, dev, 0, 0x30);
		if (!(rom_base & 1))
			check_fail("ROM window enable not accepted");
		rom_base &= 0xFFFFF800;

		pci_write(0, dev, 0, 0x30, 0xFFFFFFFF);
		w = pci_read(0, dev, 0, 0x30);
		pci_write(0, dev, 0, 0x30, rom_base | 1);
		{
			uint32_t rom_size = (~w & 0xFFFFF800) + 0x800;

			r = *(volatile uint8_t *)(rom_base + 0);
			if (r != 0xFF)
				check_fail("ROM window does not serve erased flash");
			r = *(volatile uint8_t *)(rom_base + rom_size - 1);
			if (r != 0xFF)
				check_fail("ROM window does not serve the top");
			check_ok("rom window serves the stub flash");

			*(volatile uint8_t *)(rom_base + 0x5555) = 0xAA;
			*(volatile uint8_t *)(rom_base + 0x2AAA) = 0x55;
			*(volatile uint8_t *)(rom_base + 0x5555) = 0x90;
			if (*(volatile uint8_t *)(rom_base + 0) == 0xFF)
				check_fail("JEDEC id manufacturer byte empty");
			if (*(volatile uint8_t *)(rom_base + 1) == 0xFF)
				check_fail("JEDEC id device byte empty");
			check_ok("jedec id through the rom window");

			*(volatile uint8_t *)(rom_base + 0x5555) = 0xF0;
			for (i = 0; i < 0x100; i++) {
				if (*(volatile uint8_t *)(rom_base + i) != 0xFF)
					check_fail("rom window corrupted after id exit");
			}
			check_ok("rom window intact after id exit");
		}
	}

	print("GFXATI-PROBE done\n");
	for (;;)
		__asm__ volatile("cli; hlt");
}
