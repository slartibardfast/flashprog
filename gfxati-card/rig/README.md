
## The FreeDOS rig (plan/0003#gfxati-flash-rig)

`rig/fat12.py` (FAT12/FAT16 floppy and disk-image reader/writer, partition
offset aware) and `rig/rig.py` (prep + run) boot FreeDOS 1.4 LiteUSB with
the stub card, run a real era atiflash from the autoexec, and verify the
flash workflow. Build deps: qemu with the gfxati device (see above), the
FreeDOS 1.4 LiteUSB image (download.freedos.org/1.4/FD14-LiteUSB.zip), a
go32v2-era atiflash build, CWSDPMI, and the vbios image.

Status (2026-08-15): the harness works end to end - FreeDOS boots from the
prepped image, the autoexec runs, result files extract - but atiflash 3.49
produces no output at all in this environment: no stdout (empty redirects),
no files (even with -d), no VGA text, no graphics mode. The exe is a valid
go32v2 binary (go32stub v2.02T, embedded CWSDPMI r5); the batch markers
prove it runs and returns. The failure is silent somewhere in the
DPMI/console layer of the 2006-era binary under 2026 FreeDOS on TCG, so
the rig attestation is blocked. Options being weighed: DOSBox-X/DOSEMU as
the DOS host, a different DPMI configuration, or a 4.x atiflash build.
The card device's own attestation (the probe kernel) is green regardless.
