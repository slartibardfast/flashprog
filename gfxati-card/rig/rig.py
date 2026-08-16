#!/usr/bin/env python3
"""The gfxati FreeDOS rig: boot FreeDOS with the stub card, run the real
era atiflash against it, and verify the flash workflow.

Prep + run:
  python3 rig.py --prep <boot.img> <atiflash.exe> <vbios.bin> <cwsdpmi.exe> <out.img>
  python3 rig.py --run <out.img> [qemu-system-x86_64 path] [chip index]

The attestation per plan/0003#gfxati-flash-rig: -i reports the stub card
and its chip, -p writes the vbios image, the read-back matches the written
image byte for byte. The autoexec writes the results to A:\\RES.TXT and the
read-back to A:\\READBACK.BIN on the floppy; the driver polls the floppy
image file and verifies.
"""
import subprocess
import sys
import time
import os

import fat12

CHIP_DEFAULT = 19  # SST39SF010 (128KB, JEDEC id 0xBF 0xB5) - fits the X1900GT vbios

AUTOEXEC = (
    "@echo off\r\n"
    "SET SWAP=OFF\r\n"
    "\\ATIFLASH.EXE -i > \\RES.TXT\r\n"
    "\\ATIFLASH.EXE -p \\VBIOS.BIN >> \\RES.TXT\r\n"
    "\\ATIFLASH.EXE -r \\READBACK.BIN >> \\RES.TXT\r\n"
    "ECHO RIG-DONE >> \\RES.TXT\r\n"
)


LITE_PART_LBA = 63


def prep(boot, atiflash, vbios, cwsdpmi, out):
    img = fat12.Fat12Image(open(boot, 'rb').read(),
                           offset=LITE_PART_LBA * 512)
    cfg = img.read_file('FDCONFIG.SYS')
    if cfg and b'/P=\\FDAUTO.BAT' in cfg and b'/N' not in cfg:
        img.write_file('FDCONFIG.SYS', cfg.replace(b'/P=\\FDAUTO.BAT',
                                                   b'/P=\\FDAUTO.BAT /N'))
    img.write_file('FDAUTO.BAT', AUTOEXEC.encode())
    img.write_file('CWSDPMI.EXE', open(cwsdpmi, 'rb').read())
    img.write_file('ATIFLASH.EXE', open(atiflash, 'rb').read())
    img.write_file('VBIOS.BIN', open(vbios, 'rb').read())
    img.save(out)
    print('prep: %s ready' % out)


def run(out, qemu='qemu-system-x86_64', chip=CHIP_DEFAULT):
    slog = out + '.serial.log'
    q = subprocess.Popen(
        [qemu, '-machine', 'pc,graphics=off', '-m', '128',
         '-device', 'gfxati-card,chip=%d' % chip,
         '-nic', 'none',
         '-hda', out, '-display', 'none',
         '-serial', 'file:' + slog, '-no-reboot'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + 180
        res = None
        img = None
        while time.time() < deadline:
            time.sleep(4)
            try:
                img = fat12.Fat12Image(open(out, 'rb').read(),
                                       offset=LITE_PART_LBA * 512)
            except Exception:
                continue
            res = img.read_file('RES.TXT')
            if res and b'RIG-DONE' in res:
                break
            if q.poll() is not None:
                print('qemu exited early (code %d)' % q.returncode)
                return 1
        if not res or b'RIG-DONE' not in res:
            print('FAIL: no result within the deadline')
            return 1
        text = res.decode('latin1')
        print(text)
        rb = img.read_file('READBACK.BIN')
        vb = img.read_file('VBIOS.BIN')
        if rb is None or vb is None:
            print('FAIL: readback or vbios missing')
            return 1
        ok = rb[:len(vb)] == vb
        print('readback %d bytes, vbios %d bytes, match=%s' %
              (len(rb), len(vb), 'yes' if ok else 'no'))
        if not ok:
            bad = next((i for i in range(len(vb))
                        if rb[i] != vb[i]), -1)
            print('first mismatch at byte %d: want 0x%02x got 0x%02x' %
                  (bad, vb[bad], rb[bad]))
            return 1
        print('RIG-ATTESTED')
        return 0
    finally:
        q.terminate()


def main():
    if sys.argv[1] == '--prep':
        prep(sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6])
    elif sys.argv[1] == '--run':
        args = sys.argv[3:]
        qemu = args[0] if args else 'qemu-system-x86_64'
        chip = int(args[1]) if len(args) > 1 else CHIP_DEFAULT
        sys.exit(run(sys.argv[2], qemu, chip))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
