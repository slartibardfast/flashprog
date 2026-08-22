#!/usr/bin/env python3
"""The MS-DOS v6.22 rig (plan/0003#gfxati-flash-rig): boot MS-DOS with the
stub card and a real era atiflash, then attest the flash workflow end to
end: -i reports the stub card and its chip, -p programs a vbios image,
-s saves the ROM back, and the read-back matches the written image byte
for byte.

Prep + run:
  python3 rig-msdos.py --prep <Dos6.22.img> <atiflash.exe> <vbios.bin> <dir>
  python3 rig-msdos.py --run <dir> [qemu-system-x86_64 path]

The boot floppy (A:) carries the minimal CONFIG.SYS (HIMEM.SYS
/TESTMEM:OFF + FILES/BUFFERS + DOS=HIGH) and the AUTOEXEC batch; the
payload floppy (B:) carries the flasher, the vbios image, and receives
RES.TXT + READBACK.BIN. The rig images are the MS-DOS v6.22 disk image
itself as the FAT12 base (archive.org item dos-6.22, Dos6.22.img,
sha256 1ab300a0a54b8f384cc457424ea0d2f3f46bef11c0172429c6b207b2ec539e6e)
- hand-formatted floppies read as garbage to the guest.

Device: -device gfxati-card,chip=3,strap=9 — the AT25F1024 serial chip
(the af417/3.49 catalog part) with the 0xE4 flash-strap nibble 9, which
atiflash 3.49's type table binds to "AT25F1024/C" (128KB, initial
romsize 0x20000). The attested flasher is the pristine
refs/sources/archive/af349/atiflash.exe; the vbios is the archived
Radeon 9700 Pro ROM (a real ATI image, 55 AA header, 0xB000 bytes).
"""
import os
import subprocess
import sys
import time

import fat12

CHIP = 3      # AT25F1024 (128KB SPI)
STRAP = 9     # 0xE4 high nibble -> type-table "AT25F1024/C"

CONFIG_SYS = (b"DEVICE=A:\\HIMEM.SYS /TESTMEM:OFF\r\n"
	      b"FILES=40\r\nBUFFERS=20\r\nDOS=HIGH\r\n")

AUTOEXEC = (b"@ECHO OFF\r\n"
	    b"B:\\ATIFLASH.EXE -i > B:\\RES.TXT\r\n"
	    b"B:\\ATIFLASH.EXE -p 0 -f B:\\VBIOS.BIN >> B:\\RES.TXT\r\n"
	    b"B:\\ATIFLASH.EXE -s 0 B:\\READBACK.BIN >> B:\\RES.TXT\r\n"
	    b"ECHO RIG-DONE >> B:\\RES.TXT\r\n")


def prep(boot_img, atiflash, vbios, outdir):
    base = open(boot_img, 'rb').read()

    boot = fat12.Fat12Image(base, offset=0)
    boot.write_file('CONFIG.SYS', CONFIG_SYS)
    boot.write_file('AUTOEXEC.BAT', AUTOEXEC)
    boot.save(os.path.join(outdir, 'boot.img'))

    pay = fat12.Fat12Image(base, offset=0)
    for f in ['QBASIC EXE', 'QBASIC HLP', 'SCANDISKEXE', 'SCANDISKINI',
              'MOUSE COM', 'MOUSE SYS', 'MOUSE INI', 'MOUSE @@@',
              'EDIT EXE', 'EDIT HLP', 'EMM386 EXE', 'DRVSPACEBIN',
              'CD1 SYS', 'CD2 SYS', 'CD3 SYS', 'CD4 SYS', 'SETVER EXE',
              'RESTORE EXE', 'UNDELETEEXE', 'UNDELETEINI', 'UNFORMATCOM',
              'SHARE EXE', 'MSCDEX EXE', 'DELTREE EXE', 'FDISK EXE',
              'FORMAT COM', 'ATTRIB EXE']:
        try:
            pay.delete_file(f)
        except Exception:
            pass
    pay.write_file('ATIFLASH.EXE', open(atiflash, 'rb').read())
    pay.write_file('VBIOS.BIN', open(vbios, 'rb').read())
    pay.save(os.path.join(outdir, 'payload.img'))
    print('prep: %s ready (boot.img + payload.img)' % outdir)


def run(outdir, qemu='qemu-system-x86_64', timeout_s=580):
    payload = os.path.join(outdir, 'payload.img')
    runpay = os.path.join(outdir, 'run-payload.img')
    shutil_copy(payload, runpay)
    slog = os.path.join(outdir, 'serial.log')

    q = subprocess.Popen(
        [qemu, '-machine', 'pc,graphics=off', '-m', '16',
         '-fda', os.path.join(outdir, 'boot.img'), '-fdb', runpay,
         '-boot', 'a',
         '-device', 'gfxati-card,chip=%d,strap=%d' % (CHIP, STRAP),
         '-nic', 'none', '-display', 'none',
         '-serial', 'file:' + slog, '-no-reboot'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + timeout_s
        res = None
        img = None
        while time.time() < deadline:
            time.sleep(5)
            try:
                img = fat12.Fat12Image(open(runpay, 'rb').read(), offset=0)
            except Exception:
                continue
            res = img.read_file('RES.TXT')
            if res and b'RIG-DONE' in res:
                break
            if q.poll() is not None:
                print('qemu exited early (code %d)' % q.returncode)
                return 1
        if not res or b'RIG-DONE' not in res:
            print('FAIL: no RIG-DONE within the deadline')
            return 1
        text = res.decode('cp437', 'replace')
        print(text)
        checks = [
            ('flash type identified', 'AT25F1024/C' in text),
            ('bytes programmed', 'bytes programmed' in text and
             '0000/0000h' not in text.split('bytes programmed')[0][-8:]),
            ('bytes verified', 'bytes verified' in text),
            ('ROM saved', '0x0 bytes saved' not in text),
        ]
        ok = True
        for name, passed in checks:
            print('%s %s' % ('ok:' if passed else 'FAIL:', name))
            ok = ok and passed
        rb = img.read_file('READBACKBIN')
        vb = img.read_file('VBIOS BIN')
        if rb is None or vb is None or not len(rb):
            print('FAIL: readback missing or empty')
            return 1
        match = rb[:len(vb)] == vb
        print('readback %d bytes, vbios %d bytes, match=%s' %
              (len(rb), len(vb), 'yes' if match else 'no'))
        if not match:
            bad = next((i for i in range(min(len(rb), len(vb)))
                        if rb[i] != vb[i]), -1)
            print('first mismatch at byte %d: want 0x%02x got 0x%02x' %
                  (bad, vb[bad], rb[bad]))
            return 1
        if not ok:
            return 1
        print('RIG-ATTESTED')
        return 0
    finally:
        q.terminate()


def shutil_copy(src, dst):
    with open(src, 'rb') as f, open(dst, 'wb') as g:
        g.write(f.read())


def main():
    if sys.argv[1] == '--prep':
        os.makedirs(sys.argv[5], exist_ok=True)
        prep(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5])
    elif sys.argv[1] == '--run':
        args = sys.argv[3:]
        qemu = args[0] if args else 'qemu-system-x86_64'
        sys.exit(run(sys.argv[2], qemu))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
