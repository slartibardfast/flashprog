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
echo "RIG-BOOT linux-guest"
FP="/rig/ld-linux-x86-64.so.2 --library-path /rig /rig/flashprog -c AT25F1024(A)"
echo "=== identify ==="
$FP -p gfxati
echo "RIG-ID-EXIT=$?"
echo "=== write ==="
$BB cp /rig/rom.bin /rig/rom-pad.bin
$BB dd if=/dev/zero bs=86016 count=1 2>/dev/null | $BB tr '\\000' '\\377' >> /rig/rom-pad.bin
$FP -p gfxati -w /rig/rom-pad.bin
echo "RIG-WRITE-EXIT=$?"
echo "=== read back ==="
$FP -p gfxati -r /rig/readback.bin
echo "RIG-READ-EXIT=$?"
if $BB cmp -s /rig/rom-pad.bin /rig/readback.bin; then
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
	elif sys.argv[1] == '--run':
		args = sys.argv[3:]
		qemu = args[0] if args else 'qemu-system-x86_64'
		sys.exit(run(sys.argv[2], qemu))
	else:
		sys.exit(__doc__)


if __name__ == '__main__':
	main()
