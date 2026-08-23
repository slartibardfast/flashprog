#!/usr/bin/env python3
"""The Linux conformance rig (plan/0004#linux-rig): boot a Linux guest
with the stub card and the flashprog gfxati programmer, then attest the
flash workflow end to end: identification (the AT25F 15H product-ID
probe through the window's ID latch), erase + program + verify (flashprog
VERIFIED), and a full-chip read back that matches the written image byte
for byte.

The MS-DOS rig (rig-msdos.py) attested the CARD against the original
atiflash; this rig attests the PROGRAMMER against the same interface.

Prep + run:
  python3 rig-linux.py --prep <flashprog> <vbios.bin> <dir>
  python3 rig-linux.py --run <dir> [qemu-system-x86_64 path]

Guest pieces (pinned in <dir> by --prep, from the already-fetched
refs/rig/linux-guest/):
  vmlinuz-lts + initramfs-alpine - the Alpine 3.20.3 netboot kernel
    (6.6.49-0-lts) and initramfs, which contribute busybox and the
    pseudo-filesystem userspace
  overlay/init            - replaces Alpine's init: mounts, runs the
                            workflow, compares, poweroffs
  overlay/rig/            - the flashprog build + its glibc library
                            closure (run via the copied ld.so, so the
                            musl base doesn't matter) + the vbios
  the kernel unpacks the concatenated initramfs archives in order, so
  the overlay's /init and /rig win.

Device: -device gfxati-card,chip=3,strap=9 - the AT25F1024 serial chip
(the R580's own flash; flashprog identifies it as "AT25F1024(A)", the
DB's name for the 1F 60 part that also covers the relabeled AT25F512,
hence the explicit -c). The vbios is the archived 9700 Pro ROM (45056
bytes) padded to the chip size with 0xFF.

The write path uses flashprog's own erasers (the 52H sector erases now
real in the card model), the page-program stream through the window,
and verification through the array read path.

Known guest wrinkle, handled in the programmer: the kernel's VGA
"shadowed ROM" fixup leaves the ROM BAR enable off; the card's
forced-enable hack restores the bit behind the PCI core's back, so the
config reads enabled while the mapping was never refreshed. The
programmer re-writes address+enable through the config path (as
atiflash did), which makes the PCI core map the window.
"""
import os
import subprocess
import sys
import time

CHIP = 3      # AT25F1024 (128KB SPI)
STRAP = 9     # 0xE4 high nibble -> type-table "AT25F1024/C"

INIT = '''#!/bin/sh
export PATH=/bin:/sbin:/usr/bin:/usr/sbin
BB=/bin/busybox
$BB mount -t proc proc /proc 2>/dev/null
$BB mount -t sysfs sysfs /sys 2>/dev/null
$BB mount -t devtmpfs dev /dev 2>/dev/null
. /rig/scenario
echo "RIG-BOOT linux-guest scenario=$SCENARIO chip=$QCHIP strap=$QSTRAP"
PROG="gfxati"
[ -n "$PROGSUFFIX" ] && PROG="$PROG:$PROGSUFFIX"
FP="/rig/ld-linux-x86-64.so.2 --library-path /rig /rig/flashprog -p $PROG $CHIPARGS"
if [ "$NEGATIVE" = "1" ]; then
	echo "=== negative: wrong chip forced ==="
	$FP
	echo "RIG-ID-EXIT=$?"
	echo "RIG-DONE"
	$BB sleep 1
	$BB poweroff -f
	exit 0
fi
if [ "$SCENARIO" = "ssid-guard" ]; then
	echo "=== guard 1: write the blank-SSID image with strap bytes ==="
	$FP -c 'AT25F1024(A)' -w /rig/ssid-a.bin
	echo "RIG-STAGE1-EXIT=$?"
	echo "=== guard 2: a differing SSID onto the blank card is refused ==="
	$FP -c 'AT25F1024(A)' -w /rig/ssid-b.bin
	echo "RIG-STAGE2-EXIT=$?"
	echo "=== guard 3: --force overrides (the card sits erased after guard 2; B lands as-is) ==="
	$FP -c 'AT25F1024(A)' --force -w /rig/ssid-b.bin
	echo "RIG-STAGE3-EXIT=$?"
	$FP -c 'AT25F1024(A)' -r /rig/readback.bin
	echo "RIG-STAGE4-EXIT=$?"
	if $BB cmp -s /rig/ssid-b.bin /rig/readback.bin; then
		echo "RIG-FORCE-AS-IS"
	else
		echo "RIG-FORCE-MISMATCH"
	fi
	echo "=== guard 5: a card that carries an SSID crossflashes freely ==="
	$FP -c 'AT25F1024(A)' -w /rig/ssid-a2.bin
	echo "RIG-STAGE5-EXIT=$?"
	$FP -c 'AT25F1024(A)' -w /rig/ssid-c.bin
	echo "RIG-STAGE6-EXIT=$?"
	echo "RIG-DONE"
	$BB sleep 1
	$BB poweroff -f
	exit 0
fi
echo "=== identify ==="
$FP
echo "RIG-ID-EXIT=$?"
echo "=== write ==="
$FP -w "$IMAGE"
echo "RIG-WRITE-EXIT=$?"
echo "=== read back ==="
$FP -r /rig/readback.bin
echo "RIG-READ-EXIT=$?"
if $BB cmp -s "$IMAGE" /rig/readback.bin; then
	echo "RIG-READBACK-MATCH"
else
	echo "RIG-READBACK-MISMATCH"
fi
echo "RIG-DONE"
$BB sleep 1
$BB poweroff -f
'''


def prep(flashprog, vbios, outdir):
	os.makedirs(outdir + '/overlay/rig', exist_ok=True)
	open(outdir + '/overlay/init', 'w').write(INIT)
	os.chmod(outdir + '/overlay/init', 0o755)
	shutil_copy(flashprog, outdir + '/overlay/rig/flashprog')
	os.chmod(outdir + '/overlay/rig/flashprog', 0o755)
	shutil_copy(vbios, outdir + '/overlay/rig/rom.bin')
	with open(outdir + '/overlay/rig/rom-pad.bin', 'wb') as f:
		f.write(open(vbios, 'rb').read())
		f.write(b'\xff' * (128 * 1024 - os.path.getsize(vbios)))
	with open(outdir + '/overlay/rig/scenario', 'w') as f:
		f.write("SCENARIO=baseline\nQCHIP=%d\nQSTRAP=%d\n"
			"CHIPARGS='-c AT25F1024(A)'\nIMAGE=/rig/rom-pad.bin\n"
			"NEGATIVE=0\n" % (CHIP, STRAP))
	libs = subprocess.run(['ldd', flashprog], capture_output=True,
			      text=True).stdout
	import re
	seen = set()
	for m in re.finditer(r'(/[^ ]+\.so[^ ]*)', libs):
		lib = m.group(1)
		if lib not in seen:
			seen.add(lib)
			dst = os.path.join(outdir, 'overlay', 'rig',
					   os.path.basename(lib))
			shutil_copy(lib, dst)
			os.chmod(dst, 0o755)
	print('prep: %s ready (overlay + initrd.combined)' % outdir)
	build_initrd(outdir)


def build_initrd(outdir):
	overlay = subprocess.run(
		['find', 'init', 'rig', '-type', 'd'], cwd=outdir + '/overlay',
		capture_output=True, text=True).stdout.split()
	files = subprocess.run(
		['find', 'init', 'rig', '-type', 'f'], cwd=outdir + '/overlay',
		capture_output=True, text=True).stdout.split()
	entries = ['init'] + overlay + files
	cpio = subprocess.run(['cpio', '-o', '-H', 'newc'], cwd=outdir + '/overlay',
			      input=('\n'.join(entries) + '\n').encode(),
			      capture_output=True)
	import gzip
	with open(outdir + '/overlay.cpio.gz', 'wb') as f:
		f.write(gzip.compress(cpio.stdout, 9))
	with open(outdir + '/initrd.combined', 'wb') as out:
		for part in ['initramfs-alpine', 'overlay.cpio.gz']:
			with open(os.path.join(outdir, part), 'rb') as f:
				out.write(f.read())
	print('prep: initrd.combined built')


# (name, qemu chip index, strap, -c arg or None, image, negative)
# The matrix covers every identification scheme the DB shares with
# the model: the AT25F 15H product ID, plain JEDEC RDID, and the
# multi-erase families; one scenario probes with NO -c (uniqueness),
# one runs a LYING strap (the probe must ignore the hint), and one
# negative forces the wrong chip. EXCLUDED, recorded: AT45DB011D (the
# window transport carries no AT45 buffer-opcode path - a #chip-set
# gap, not a regression) and MX25L*/W25P10/S25FL001D (model-only,
# the DB lacks entries - the optional lineage additions).
# The COMPLETE disposition table for every serial chip in the model
# (gfxati-flash.c, indices 0-32). "complete w.r.t. the test chips":
# every chip is either RUN (full five-check workflow) or EXCLUDED
# with a recorded reason. The card sizes its ROM window to the chip
# (the rig traces and the af349 chip matrix, which ran full
# workflows on the 512KB parts through this same window, prove it),
# so the programmer derives the window from the ROM BAR sizing probe
# and no chip is out of range.
#
# disposition: full | exclude
CHIPS = [
	# idx model name      size    -c name (DB)      disposition  reason
	(0,  "AT25F512",     "64K",  "AT25F512",       "full",   "AT25F 15H scheme, 32K sectors"),
	(1,  "AT25F512A",    "64K",  "AT25F512A",      "full",   "AT25F scheme, 128B pages"),
	(2,  "AT25F512B",    "64K",  "AT25F512B",      "full",   "JEDEC RDID 1F 65 00"),
	(3,  "AT25F1024",    "128K", "AT25F1024(A)",   "full",   "the R580's own chip; baseline uses the real 9700 Pro ROM"),
	(4,  "AT25F2048",    "256K", "AT25F2048",      "full",   "AT25F scheme, 64K sectors"),
	(5,  "AT25F4096",    "512K", "AT25F4096",      "full",   "AT25F scheme; the af349 chip matrix ran this part"),
	(6,  "AT25S010N",    "128K", "AT25FS010",      "full",   "the DB's AT25FS010 entry is this part (the archived sheet is its printing; JEDEC 1F 66 01)"),
	(7,  "M25P05",       "64K",  "M25P05",         "full",   "RES-only 1999 ST part (signature 05)"),
	(8,  "M25P10",       "128K", "M25P10",         "full",   "RES-only 1999 ST part (signature 10)"),
	(9,  "M25P20",       "256K", "M25P20",         "full",   "JEDEC RDID 20 20 12"),
	(10, "M25P40",       "512K", "M25P40",         "full",   "JEDEC RDID 20 20 13; ran in the af349 chip matrix"),
	(11, "MX25L512",     "64K",  "MX25L512",       "full",   "DB entry added (catalog chip, archived sheet grounding)"),
	(12, "MX25L5121E",   "64K",  "MX25L5121E",     "full",   "JEDEC RDID C2 22 10"),
	(13, "MX25L1005",    "128K", "MX25L1005",      "full",   "DB entry added"),
	(14, "MX25L1024lE",  "128K", "MX25L1024lE",    "full",   "DB entry added (shares C2 20 11 with MX25L1005; needs -c)"),
	(15, "MX25L2005",    "256K", "MX25L2005",      "full",   "DB entry added"),
	(16, "SST25VF512",   "64K",  "SST25VF512",     "full",   "DB entry added (REMS BF 48)"),
	(17, "SST25VF010",   "128K", "SST25VF010",     "full",   "SST BF 25 49, 4K/32K/64K erases"),
	(18, "SST25VF020",   "256K", "SST25VF020",     "full",   "REMS device 43 (differs from its JEDEC id)"),
	(19, "SST25VF040",   "512K", "SST25VF040",     "full",   "REMS device 44; ran in the af349 chip matrix"),
	(20, "SST25VF040B",  "512K", "SST25VF040B",    "full",   "AAI+EWSR quirks; ran in the af349 chip matrix"),
	(21, "W25P10",       "128K", "W25P10",         "full",   "DB entry added (REMS/RES 10h; predates RDID)"),
	(22, "W25P20",       "256K", "W25P20",         "full",   "DB entry added (REMS/RES 11h)"),
	(23, "W25P40",       "512K", "W25P40",         "full",   "DB entry added (REMS/RES 12h)"),
	(24, "W25Q40",       "512K", "W25Q40.V",       "full",   "JEDEC RDID EF 40 13; ran in the af349 chip matrix"),
	(25, "W25X10",       "128K", "W25X10",         "full",   "JEDEC RDID EF 30 11, RES 10"),
	(26, "W25X20",       "256K", "W25X20",         "full",   "JEDEC RDID EF 30 12, RES 11"),
	(27, "W25X40",       "512K", "W25X40",         "full",   "JEDEC RDID EF 30 13, RES 12; ran in the af349 chip matrix"),
	(28, "W25X80",       "1M",   "W25X80",         "full",   "the largest catalog part (1MB window)"),
	(29, "S25FL001D",    "128K", "S25FL001D",      "full",   "DB entry added (RES 10h; no JEDEC RDID)"),
	(30, "S25FL002D",    "256K", "S25FL002D",      "full",   "DB entry added (RES 11h; no JEDEC RDID)"),
	(31, "S25FL004A",    "512K", "S25FL004A",      "full",   "JEDEC RDID 01 02 12 (Spansion)"),
	(32, "AT45DB011D",   "128K", None,             "exclude","the window carries no AT45 buffer-opcode path (transport gap, #chip-set)"),
	(33, "AT29C256",     "32K",  None,             "exclude","the model row carries no id bytes; unidentifiable until grounded"),
	(34, "AT29C512",     "64K",  "AT29C512",       "full",   "JEDEC parallel, 1F 5D; the page-load proof"),
	(35, "AT29C010A",    "128K", "AT29C010A",      "full",   "JEDEC parallel, 1F D5"),
	(36, "AT29C020",     "256K", "AT29C020",       "full",   "JEDEC parallel, 1F DA"),
	(37, "AT29C040A",    "512K", "AT29C040A",      "full",   "JEDEC parallel, 1F A4"),
	(38, "AT49F512",     "64K",  None,             "exclude","the model row carries no id bytes"),
	(39, "AT49F001N",    "128K", None,             "exclude","DB entry added (1F 04); the write_jedec_1 byte-program dispatch is not yet modeled end-to-end"),
	(40, "AT49F001T",    "128K", None,             "exclude","DB entry added (1F 05); write_jedec_1 dispatch not yet modeled"),
	(41, "AT49LV010",    "128K", None,             "exclude","DB entry added (1F 17); write_jedec_1 dispatch not yet modeled"),
	(42, "MX29F001B",    "128K", None,             "exclude","JEDEC parallel, C2 19; the non-uniform boot-block topology and sector dispatch are not yet modeled"),
	(43, "MX29F001T",    "128K", None,             "exclude","JEDEC parallel, C2 18; boot-block topology not yet modeled"),
	(44, "MX29F512",     "64K",  None,             "exclude","the model row carries the ST sibling id (20 24); the write dispatch is not yet modeled"),
	(45, "Pm39LV512R",   "64K",  None,             "exclude","the model row carries no id bytes"),
	(46, "Pm39LV010R",   "128K", None,             "exclude","the model row carries no id bytes"),
	(47, "SST39SF512",   "64K",  None,             "exclude","JEDEC parallel, BF B4; write_jedec_1 + 30H sector dispatch not yet modeled end-to-end"),
	(48, "SST39SF010",   "128K", None,             "exclude","DB entry added (BF B5); write dispatch not yet modeled"),
	(49, "SST39VF512",   "64K",  None,             "exclude","JEDEC parallel, BF D4; write dispatch not yet modeled"),
	(50, "SST39VF010",   "128K", None,             "exclude","JEDEC parallel, BF D5; write dispatch not yet modeled"),
	(51, "SST45LF010",   "128K", None,             "exclude","DB entry added (BF 42); write dispatch not yet modeled"),
]

# strap values per family (the hint flashprog logs; the probe decides)
FAMILY_STRAP = {"AT25F": 4, "M25P": 5, "MX25L": 5, "SST25": 6, "W25": 7,
		"S25FL": 5, "AT45": 4, "AT25S": 4, "AT29": 8, "AT49": 8,
		"MX29": 8, "Pm39": 8, "SST39": 8, "SST45": 8}

# the flash size in bytes per chip index, for image synthesis
KB = 1024
SIZES = {}
for _idx, _name, _sz, _cname, _disp, _reason in CHIPS:
	SIZES[_idx] = int(_sz.rstrip('KM')) * (KB if _sz.endswith('K')
					       else KB * KB)

KERNEL_URL = ("https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/"
	      "x86_64/netboot-3.20.3/vmlinuz-lts")
INITRAMFS_URL = ("https://dl-cdn.alpinelinux.org/alpine/v3.20/releases/"
		 "x86_64/netboot-3.20.3/initramfs-lts")
KERNEL_SHA256 = "62c37ee5eb7cc244290b990b1c811df60212d87e97c84086bfb31081bbaa573b"
INITRAMFS_SHA256 = "f5303bdd26eef67b714928886fb482405f401c73779fb9926a8a73de9eb324de"


def verify_pinned(outdir):
	"""Durability: the guest pieces are pinned by sha256; a mismatch
	fails closed (re-fetch from the pinned Alpine 3.20.3 netboot URLs)."""
	import hashlib
	for fname, want in [('vmlinuz-lts', KERNEL_SHA256),
			    ('initramfs-alpine', INITRAMFS_SHA256)]:
		path = os.path.join(outdir, fname)
		h = hashlib.sha256(open(path, 'rb').read()).hexdigest()
		if h != want:
			raise SystemExit('FAIL: %s sha256 %s != pinned %s; '
					 're-fetch %s{vmlinuz-lts,initramfs-lts}'
					 % (fname, h, want,
					    'https://dl-cdn.alpinelinux.org/alpine/'
					    'v3.20/releases/x86_64/netboot-3.20.3/'))
	print('prep: pinned guest pieces verified (kernel + initramfs)')


def family_strap(name):
	for fam, strap in FAMILY_STRAP.items():
		if name.startswith(fam):
			return strap
	return 9


def scenarios_from_chips():
	"""Every runnable scenario, derived from the complete table."""
	scen = []
	for idx, name, _size, cname, disp, _reason in CHIPS:
		if disp == "full":
			image = "rom-pad.bin" if idx == 3 else "img-%d.bin" % idx
			scen.append((name, idx, family_strap(name),
				     "-c " + cname, image, False))
	scen.append(("M25P10-lying-strap", 8, 9, "-c M25P10",
		     "img-8.bin", False))
	scen.append(("ssid-guard", 3, 9, "", "ssid-a.bin", 3))
	scen.append(("direct-route-AT25F1024", 3, 9,
		     "-c AT25F1024(A)", "rom-pad.bin", 4))
	scen.append(("indirect-route-AT25F1024", 3, 9,
		     "-c AT25F1024(A)", "rom-pad.bin", 5))
	scen.append(("negative-wrong-chip", 3, 9, "-c MX25L512",
		     "rom-pad.bin", 1))
	return scen


def synth_image(path, size, seed):
	"""A deterministic per-chip image: a 16-bit LCG byte stream."""
	state = seed & 0xFFFF
	data = bytearray(size)
	for i in range(size):
		state = (state * 251 + 17) & 0xFFFF
		data[i] = (state >> 8) & 0xFF
	open(path, 'wb').write(bytes(data))


def prep_matrix(flashprog, vbios, outdir):
	os.makedirs(outdir + '/overlay/rig', exist_ok=True)
	verify_pinned(outdir)
	open(outdir + '/overlay/init', 'w').write(INIT)
	os.chmod(outdir + '/overlay/init', 0o755)
	shutil_copy(flashprog, outdir + '/overlay/rig/flashprog')
	os.chmod(outdir + '/overlay/rig/flashprog', 0o755)
	shutil_copy(vbios, outdir + '/overlay/rig/rom.bin')
	# the padded baseline image
	with open(outdir + '/overlay/rig/rom-pad.bin', 'wb') as f:
		f.write(open(vbios, 'rb').read())
		f.write(b'\xff' * (128 * 1024 - os.path.getsize(vbios)))
	# the SSID-guard images: a blank-SSID image A (with distinct
	# strap bytes at 0x7A/0x7B), a stamped image B, a crossflash
	# image C, and B/C with the strap bytes re-inserted (what the
	# readback must equal after the guard preserves them)
	base = outdir + '/overlay/rig/'
	a = bytearray(open(base + 'img-8.bin', 'rb').read()[:128 * 1024])
	a[0x1A] = 0x00; a[0x1B] = 0x00
	a[0x7A] = 0xC3; a[0x7B] = 0x5A
	open(base + 'ssid-a.bin', 'wb').write(bytes(a))
	b = bytearray(a)
	b[0x1A] = 0x44; b[0x1B] = 0x4E	# 0x4E44 - the R580's SSID
	b[0x7A] = 0x00; b[0x7B] = 0x00	# B would wipe the strap bytes
	open(base + 'ssid-b.bin', 'wb').write(bytes(b))
	bp = bytearray(b)
	bp[0x7A] = 0xC3; bp[0x7B] = 0x5A
	open(base + 'ssid-b-preserved.bin', 'wb').write(bytes(bp))
	c = bytearray(b)
	c[0x1A] = 0x78; c[0x1B] = 0x56	# 0x5678 - a crossflash
	c[0x7A] = 0x00; c[0x7B] = 0x00
	open(base + 'ssid-c.bin', 'wb').write(bytes(c))
	cp = bytearray(c)
	cp[0x7A] = 0xC3; cp[0x7B] = 0x5A
	open(base + 'ssid-c-preserved.bin', 'wb').write(bytes(cp))
	a2 = bytearray(a)
	a2[0x1A] = 0x11; a2[0x1B] = 0x11	# a real SSID + the strap bytes
	open(base + 'ssid-a2.bin', 'wb').write(bytes(a2))
	for idx, size in SIZES.items():
		if idx != 3:
			synth_image(outdir + '/overlay/rig/img-%d.bin' % idx,
				    size, idx + 1)
	# every scenario's config, prepared now, swapped in at run time
	for name, chip, strap, chipargs, image, neg in scenarios_from_chips():
		with open(outdir + '/scenario-%s' % name, 'w') as f:
			f.write("SCENARIO=%s\nQCHIP=%d\nQSTRAP=%d\n"
				"CHIPARGS='%s'\nIMAGE=/rig/%s\nNEGATIVE=%d\n"
				"PROGSUFFIX=%s\n"
				% (name, chip, strap, chipargs, image, neg or 0,
				   "regs=direct" if neg == 4 else
				   ("regs=indirect" if neg == 5 else "")))
	print('prep: matrix ready in %s' % outdir)
	build_initrd(outdir)


def run_scenario(outdir, qemu, name, chip, strap, chipargs, image, neg,
		 timeout_s=200):
	# swap the scenario file into the overlay and rebuild the initrd
	shutil_copy(os.path.join(outdir, 'scenario-' + name),
		    os.path.join(outdir, 'overlay', 'rig', 'scenario'))
	build_initrd(outdir)
	slog = os.path.join(outdir, 'serial-%s.log' % name)
	q = subprocess.Popen(
		[qemu, '-machine', 'pc,graphics=off', '-m', '512',
		 '-kernel', os.path.join(outdir, 'vmlinuz-lts'),
		 '-initrd', os.path.join(outdir, 'initrd.combined'),
		 '-append', 'console=ttyS0,115200',
		 '-device', 'gfxati-card,chip=%d,strap=%d' % (chip, strap),
		 '-nic', 'none', '-display', 'none',
		 '-serial', 'file:' + slog, '-no-reboot'],
		stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
	try:
		deadline = time.time() + timeout_s
		text = ''
		while time.time() < deadline:
			time.sleep(5)
			if q.poll() is not None:
				try:
					text = open(slog, errors='replace').read()
				except OSError:
					pass
				break
			try:
				text = open(slog, errors='replace').read()
			except OSError:
				continue
			if 'RIG-DONE' in text:
				break
		if 'RIG-DONE' not in text:
			print('FAIL %s: no RIG-DONE' % name)
			return False
		if 'RIG-BOOT' in text:
			print(text[text.find('RIG-BOOT'):text.find('RIG-DONE')][:2000])
		if neg == 3:
			checks = [
				('stage1 blank image written',
				 'RIG-STAGE1-EXIT=0' in text),
				('stage2 refused without force',
				 ('RIG-STAGE2-EXIT=1' in text or
				  'RIG-STAGE2-EXIT=2' in text) and
				 'refusing to stamp an identity' in text),
				('stage3 forced through',
				 'RIG-STAGE3-EXIT=0' in text),
				('stage4 readback matches B as-is',
				 'RIG-STAGE4-EXIT=0' in text and
				 'RIG-FORCE-AS-IS' in text),
				('stage5 the crossflash base written',
				 'RIG-STAGE5-EXIT=0' in text),
				('stage6 crossflash without force',
				 'RIG-STAGE6-EXIT=0' in text),
			]
			ok = True
			for what, passed in checks:
				if not passed:
					print('FAIL %s: %s' % (name, what))
					ok = False
			if ok:
				print('ok: %s (guard: refuse/force/preserve/'
				      'crossflash)' % name)
			return ok
		if neg == 4 or neg == 5:
			route = ('direct' if neg == 4 else 'indirect')
			checks = [
				('%s route selected' % route,
				 ('gfxati: %s register route' % route) in text),
				('identified', 'RIG-ID-EXIT=0' in text),
				('erase+write', 'Erase/write done.' in text),
				('verified', 'VERIFIED.' in text),
				('read back', 'RIG-READ-EXIT=0' in text),
				('byte-identical', 'RIG-READBACK-MATCH' in text),
			]
			ok = True
			for what, passed in checks:
				if not passed:
					print('FAIL %s: %s' % (name, what))
					ok = False
			if ok:
				print('ok: %s (%s route, all checks)' % (name, route))
			return ok
		if neg == 1:
			ok = 'RIG-ID-EXIT=1' in text
			print('%s %s (negative: refused the wrong chip)'
			      % ('ok:' if ok else 'FAIL:', name))
			return ok
		if neg == 2:
			ok = ('RIG-ID-EXIT=1' in text and
			      'too big for this programmer' in text)
			print('%s %s (out of window range: refused cleanly)'
			      % ('ok:' if ok else 'FAIL:', name))
			return ok
		checks = [
			('identified', 'RIG-ID-EXIT=0' in text),
			('erase+write', 'Erase/write done.' in text),
			('verified', 'VERIFIED.' in text),
			('read back', 'RIG-READ-EXIT=0' in text),
			('byte-identical', 'RIG-READBACK-MATCH' in text),
		]
		ok = True
		for what, passed in checks:
			if not passed:
				print('FAIL %s: %s' % (name, what))
				ok = False
		if ok:
			print('ok: %s (all five checks)' % name)
		return ok
	finally:
		q.terminate()


def run_matrix(outdir, qemu='qemu-system-x86_64'):
	ok = True
	print('=== chip disposition table (all %d serial chips in the model) ==='
	      % len(CHIPS))
	for idx, name, size, cname, disp, reason in CHIPS:
		print('  [%2d] %-12s %-5s -> %-7s %s'
		      % (idx, name, size, disp, reason))
	print('=== scenarios ===')
	for name, chip, strap, chipargs, image, neg in scenarios_from_chips():
		ok = run_scenario(outdir, qemu, name, chip, strap, chipargs,
				  image, neg) and ok
	if not ok:
		return 1
	print('MATRIX-ATTESTED (%d chips: %d full, %d excluded)'
	      % (len(CHIPS),
		 sum(1 for c in CHIPS if c[4] == 'full'),
		 sum(1 for c in CHIPS if c[4] == 'exclude')))
	return 0


def run(outdir, qemu='qemu-system-x86_64', timeout_s=240):
	slog = os.path.join(outdir, 'serial.log')

	q = subprocess.Popen(
		[qemu, '-machine', 'pc,graphics=off', '-m', '512',
		 '-kernel', os.path.join(outdir, 'vmlinuz-lts'),
		 '-initrd', os.path.join(outdir, 'initrd.combined'),
		 '-append', 'console=ttyS0,115200',
		 '-device', 'gfxati-card,chip=%d,strap=%d' % (CHIP, STRAP),
		 '-nic', 'none', '-display', 'none',
		 '-serial', 'file:' + slog, '-no-reboot'],
		stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
	try:
		deadline = time.time() + timeout_s
		text = ''
		while time.time() < deadline:
			time.sleep(5)
			if q.poll() is not None:
				try:
					text = open(slog, errors='replace').read()
				except OSError:
					pass
				break
			try:
				text = open(slog, errors='replace').read()
			except OSError:
				continue
			if 'RIG-DONE' in text:
				break
		if 'RIG-DONE' not in text:
			print('FAIL: no RIG-DONE within the deadline')
			return 1
		print(text[text.find('RIG-BOOT'):])
		checks = [
			('chip identified',
			 'Using Atmel flash chip "AT25F1024(A)"' in text),
			('strap read as a hint', 'strap 0x9' in text),
			('id latch answers the 15H probe',
			 'AT25F id probe 1f 60' in text),
			('status window answers RDSR',
			 'RDSR after WREN 0x02' in text),
			('erase and write', 'Erase/write done.' in text),
			('flashprog verification', 'VERIFIED.' in text),
			('read back', 'RIG-READ-EXIT=0' in text),
			('read-back byte-identical',
			 'RIG-READBACK-MATCH' in text),
		]
		ok = True
		for name, passed in checks:
			print('%s %s' % ('ok:' if passed else 'FAIL:', name))
			ok = ok and passed
		if not ok:
			return 1
		print('RIG-ATTESTED')
		return 0
	finally:
		q.terminate()


def shutil_copy(src, dst):
	with open(src, 'rb') as f:
		with open(dst, 'wb') as g:
			g.write(f.read())


def main():
	if sys.argv[1] == '--prep':
		os.makedirs(sys.argv[4], exist_ok=True)
		prep(sys.argv[2], sys.argv[3], sys.argv[4])
	elif sys.argv[1] == '--prep-matrix':
		os.makedirs(sys.argv[4], exist_ok=True)
		prep_matrix(sys.argv[2], sys.argv[3], sys.argv[4])
	elif sys.argv[1] == '--run-matrix':
		args = sys.argv[3:]
		qemu = args[0] if args else 'qemu-system-x86_64'
		sys.exit(run_matrix(sys.argv[2], qemu))
	elif sys.argv[1] == '--run':
		args = sys.argv[3:]
		qemu = args[0] if args else 'qemu-system-x86_64'
		sys.exit(run(sys.argv[2], qemu))
	else:
		sys.exit(__doc__)


if __name__ == '__main__':
	main()
