# gfxati-card: the fake ATI GPU PCI card device

A QEMU PCI device presenting the R520/R580 identity, the R5xx (Rialto)
flash interface registers, and the ROM window backed by the flash model.
The interface contract is the map in plan/0003-gfxati-flash-stub (the
R5xx interface map section); the register offsets are grounded in the
atiflash 3.49 decompile.

## Layout

- `gfxati_card.h` / `gfxati_card.c` — the device core in plain C: PCI
  identity, the MMIO register surface, and the ROM window over the flash
  model. This is the self-testable unit.
- `gfxati_card_test.c` — the self-test: identity, the MM_INDEX / MM_DATA
  pair, SEPROM_CNTL1/2, the I2C engine, and the ROM window (array reads,
  JEDEC id mode, byte program). Build and run:
  `cc -o gfxati_card_test gfxati-flash.c gfxati-card/gfxati_card.c gfxati-card/gfxati_card_test.c && ./gfxati_card_test`
- `gfxati_card_qemu.c` — the qdev glue (QEMU APIs only; compiles inside a
  QEMU tree, not in the plain build).
- `probe/probe.c` — a multiboot probe kernel for the boot attestation: PCI
  scan, MMIO register checks, ROM-window reads and a JEDEC id through the
  window, one `GFXATI-OK/GFXATI-FAIL` line per check on the serial port.
  Build: `gcc -m32 -ffreestanding -nostdlib -static -fno-stack-protector
  -Wl,-e,main -T probe/probe.lds -o probe.elf probe/probe.c`

## Register surface (grounded)

| Offset | Register | Semantics |
|--------|----------|-----------|
| 0xA0 | MM_INDEX | the indirect index (SEPROM_CNTL1 = 0x1C0, SEPROM_CNTL2 = 0x1C4) |
| 0xA4 | MM_DATA | reads/writes the indexed register |
| 0x3E0 | I2C control | direct register (bus-free mask 0xFFFFFFDE \| 6) |
| 0x3E4 | I2C length/flags | `(len-1) \| flags \| 0xF90100`, GO 0x1000 |
| 0x3E8 | I2C data | one byte per access |

SEPROM_CNTL1 bit 0x400 is the flash chip-select and drives the flash
model's CS; bits 0x1100 are the busy flags of the command protocol (clear
in this cut: every command completes instantly).

Open item (recorded in the plan): the MMIO offset of the flash window
address register — the vtable slot that receives the 0x9000000-relative
window values is not in the 3.49 C listing. This cut serves the flash
through the ROM window directly (the parallel path, which the decompile
grounds: CParallel reads and JEDEC sequences go through the window). The
window register is expected to surface as a concrete gap when real
atiflash runs against the device in the rig round, and gets pinned then.

## Boot attestation recipe

The plan's attestation for plan/0003#gfxati-card-device: QEMU boots the
device, the ROM window serves the stub flash, and the controller registers
respond. Build QEMU with this module:

1. Clone qemu (`--depth 1 --branch v9.0.0 https://gitlab.com/qemu-project/qemu`).
2. Copy `gfxati-flash.c`, `gfxati-flash.h`, `gfxati-card/*` into `hw/misc/`,
   add `system_ss.add(when: 'CONFIG_GFXATI', if_true: files(...))` to
   `hw/misc/meson.build`, and a `config_gfxati` Kconfig entry.
3. `./configure --target-list=x86_64-softmmu && make -j$(nproc)`.
4. Run:
   `qemu-system-x86_64 -machine pc -device gfxati-card -kernel probe.elf -nographic`
   and grep the serial output for `GFXATI-PROBE done` with no
   `GFXATI-FAIL`.

The device may be instantiated per identity: `-device gfxati-card,device_id=0x7100`
(any of 0x7100/0x7210/0x7240/0x7243/0x7248) and per chip:
`-device gfxati-card,chip=N` (index into the flash model's chip table).
