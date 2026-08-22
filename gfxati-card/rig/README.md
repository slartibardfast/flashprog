
## The FreeDOS rig (plan/0003#gfxati-flash-rig)

`rig/fat12.py` (FAT12/FAT16 floppy and disk-image reader/writer, partition
offset aware) and `rig/rig.py` (prep + run) boot FreeDOS v1.4 LiteUSB with
the stub card, run a real era atiflash from the autoexec, and verify the
flash workflow. Build deps: qemu with the gfxati device (see above), the
FreeDOS v1.4 LiteUSB image (download.freedos.org/1.4/FD14-LiteUSB.zip), a
go32v2-era atiflash build, CWSDPMI, and the vbios image.

Status (2026-08-15): the harness works end to end - FreeDOS boots from the
prepped image, the autoexec runs, result files extract - but atiflash v3.49
produces no output at all in this environment: no stdout (empty redirects),
no files (even with -d), no VGA text, no graphics mode. The exe is a valid
go32v2 binary (go32stub v2.02T, embedded CWSDPMI r5); the batch markers
prove it runs and returns. The failure is silent somewhere in the
DPMI/console layer of the 2006-era binary under 2026 FreeDOS on TCG, so
the rig attestation is blocked. Options being weighed: DOSBox-X/DOSEMU as
the DOS host, a different DPMI configuration, or a 4.x atiflash build.
The card device's own attestation (the probe kernel) is green regardless.

### Root cause of the guest crash (2026-08-15)

The "Invalid Opcode at ..." crash seen when running atiflash or CWSDPMI was
FreeCom 0.86's XMS swap colliding with resident DPMI hosts: the shell swaps
its transient to XMS, the TSR loads into the freed low memory, and the
shell's swap-in executes the TSR's code (a VCPI stub) as its own - the
stub's `pop bp; mov ax,0xde0b; int 67; ret` returns through a stack value
into data -> #UD. The XMS machinery itself round-trips correctly under TCG
(verified with a hand-written XMS alloc/move/readback test COM:
XMS-ROUNDTRIP-OK), so this is a guest-side interaction, not a QEMU bug.
rig.py's prep now adds FreeCom's `/N` swap-disable switch to the SHELL=
line in FDCONFIG.SYS, which eliminates the TSR crash (verified: CWSDPMI
loads and runs clean with the fix).

### MS-DOS v6.22 host (the operator's procedure, 2026-08-16)

The operator's fixed run matrix boots MS-DOS v6.22 cleanly to the A:\>
prompt (R0 passes: -machine pc,graphics=off -m 16 -fda only -boot a,
CONFIG.SYS = HIMEM.SYS /TESTMEM:OFF + DOS=HIGH + FILES/BUFFERS, A20=1,
no hda, no card). Earlier A20=0 boot sticks were artifacts of the
-m 128 + hda + card combination; the operator's parameters are clean.
The boot floppy (Dos6.22.img, archive.org item dos-6.22) is the host;
the payload lives on a separate FAT16 disk. This replaces FreeDOS as
the atiflash host when the FreeDOS kernel's memory-manager path faults.
