#!/usr/bin/env python3
"""Minimal FAT12 floppy image reader/writer (no external tools).

Reads the BPB for geometry, walks the FAT12 cluster chains, and can
list/read/write files in a FAT12 image (1.2MB and 1.44MB formats).
Used by the gfxati FreeDOS rig to inject atiflash, CWSDPMI and the
vbios image into the boot floppy and to extract the result files
after the run.
"""
import struct
import sys


def _bpb(img):
    bps = struct.unpack_from('<H', img, 11)[0]
    spc = img[13]
    rsvd = struct.unpack_from('<H', img, 14)[0]
    nfat = img[16]
    root_ents = struct.unpack_from('<H', img, 17)[0]
    tsectors = struct.unpack_from('<H', img, 19)[0]
    spf = struct.unpack_from('<H', img, 22)[0]
    return bps, spc, rsvd, nfat, root_ents, tsectors, spf


class Fat12Image:
    def __init__(self, data, offset=0):
        self.data = bytearray(data)
        self.off = offset
        (self.bps, self.spc, self.rsvd, self.nfat,
         self.root_ents, self.tsectors, self.spf) = _bpb(self.data[self.off:])
        self.fat_off = self.off + self.rsvd * self.bps
        self.root_off = self.fat_off + self.nfat * self.spf * self.bps
        self.root_sectors = (self.root_ents * 32 + self.bps - 1) // self.bps
        self.data_off = self.root_off + self.root_sectors * self.bps
        self.data_clusters = (self.tsectors * self.bps - self.data_off) // (
            self.bps * self.spc)
        self.fat_bits = 12 if self.data_clusters < 4085 else 16
        self.fat = self._read_fat()

    def _read_fat(self):
        fat = []
        raw = bytes(self.data[self.fat_off:self.fat_off + self.spf * self.bps])
        if self.fat_bits == 12:
            i = 0
            while True:
                b0 = raw[i]
                b1 = raw[i + 1]
                b2 = raw[i + 2]
                fat.append(b0 | ((b1 & 0x0F) << 8))
                fat.append(((b1 & 0xF0) >> 4) | (b2 << 4))
                i += 3
                if len(fat) >= self.data_clusters + 2:
                    break
        else:
            for i in range(0, self.data_clusters + 2):
                fat.append(int.from_bytes(raw[i * 2:i * 2 + 2], 'little'))
        return fat

    def _cluster_off(self, n):
        return self.data_off + (n - 2) * self.bps * self.spc

    def _end(self):
        return 0xFFF if self.fat_bits == 12 else 0xFFFF

    def read_chain(self, start):
        out = b''
        n = start
        seen = 0
        while 2 <= n < self._end() - 7 and seen < self.data_clusters:
            off = self._cluster_off(n)
            out += bytes(self.data[off:off + self.bps * self.spc])
            n = self.fat[n]
            seen += 1
        return out

    def free_cluster(self, start=2):
        for n in range(start, len(self.fat)):
            if self.fat[n] == 0:
                return n
        return None

    def cluster_at(self, n):
        off = self.fat_off + n * 2 if self.fat_bits == 16 else self.fat_off + n * 3 // 2
        return int.from_bytes(self.data[off:off + 2], 'little') if self.fat_bits == 16 else None

    def write_file(self, name, content):
        entries = []
        root = self.data[self.root_off:self.root_off + self.root_ents * 32]
        want = name.strip().lower().replace('.', '').replace(' ', '')
        for i in range(self.root_ents):
            e = root[i * 32:(i + 1) * 32]
            if not e or e[0] in (0x00, 0xE5):
                continue
            lfn = e[11] == 0x0F
            if lfn or e[11] == 0x08 or e[11] == 0x10:
                continue
            short = e[0:11].decode('ascii', 'replace').strip()
            if short.lower().replace('.', '').replace(' ', '') == want:
                return self._overwrite(i, name, content)
        return self._new_file(name, content)

    def _name_to_short(self, name):
        base, _, ext = name.partition('.')
        return (base[:8].upper().ljust(8) + ext[:3].upper().ljust(3)).encode()

    def _overwrite(self, idx, name, content):
        # Reuse existing clusters first
        ent = bytearray(self.data[self.root_off + idx * 32:
                                  self.root_off + (idx + 1) * 32])
        old_start = struct.unpack_from('<H', ent, 26)[0]
        old_clusters = []
        if old_start >= 2:
            n = old_start
            seen = 0
            while 2 <= n < 0xFF8 and seen < self.data_clusters:
                nxt = self.fat[n]
                self.fat[n] = 0
                n = nxt
                seen += 1
                if n >= 2 and n < 0xFF8:
                    old_clusters.append(n)
        
        needed = (len(content) + self.bps * self.spc - 1) // (self.bps * self.spc)
        new_clusters = old_clusters[:needed]
        if len(new_clusters) < needed:
            extra = self._alloc(needed - len(new_clusters))
            new_clusters.extend(extra)
        
        for i, c in enumerate(new_clusters):
            nextc = new_clusters[i + 1] if i + 1 < len(new_clusters) else self._end()
            self.fat[c] = nextc
            off = self._cluster_off(c)
            chunk = content[i * self.bps * self.spc:(i + 1) * self.bps * self.spc]
            self.data[off:off + len(chunk)] = chunk
        self._write_fat()
        ent = bytearray(self.data[self.root_off + idx * 32:
                                  self.root_off + (idx + 1) * 32])
        ent[26:28] = struct.pack('<H', new_clusters[0] if new_clusters else 0)
        ent[28:32] = struct.pack('<I', len(content))
        ent[0x0D] = 0x02  # time
        ent[0x0B] = 0x20  # archive
        self.data[self.root_off + idx * 32:
                  self.root_off + (idx + 1) * 32] = ent
        return True

    def _alloc(self, size):
        clusters = []
        taken = set()
        remaining = size
        while remaining > 0:
            free = self.free_cluster()
            while free is not None and free in taken:
                free = self.free_cluster(free + 1)
            if free is None:
                raise RuntimeError('no free clusters')
            taken.add(free)
            clusters.append(free)
            remaining -= self.bps * self.spc
        return clusters

    def _new_file(self, name, content):
        idx = None
        root = self.data[self.root_off:self.root_off + self.root_ents * 32]
        for i in range(self.root_ents):
            e = root[i * 32:(i + 1) * 32]
            if not e or e[0] in (0x00, 0xE5):
                idx = i
                break
        if idx is None:
            raise RuntimeError('root directory full')
        ent = bytearray(32)
        ent[0:11] = self._name_to_short(name)
        ent[0x0B] = 0x20
        ent[0x0C] = 0x00
        ent[0x0D] = 0x02
        ent[0x16] = 0x00
        ent[0x1A] = 0x00
        ent[0x1B] = 0x00
        clusters = self._alloc(len(content))
        for i, c in enumerate(clusters):
            nextc = clusters[i + 1] if i + 1 < len(clusters) else self._end()
            self.fat[c] = nextc
            off = self._cluster_off(c)
            chunk = content[i * self.bps * self.spc:(i + 1) * self.bps * self.spc]
            self.data[off:off + len(chunk)] = chunk
        ent[26:28] = struct.pack('<H', clusters[0] if clusters else 0)
        ent[28:32] = struct.pack('<I', len(content))
        self.data[self.root_off + idx * 32:
                  self.root_off + (idx + 1) * 32] = ent
        self._write_fat()
        return True

    def _write_fat(self):
        raw = bytearray()
        if self.fat_bits == 12:
            for i in range(0, len(self.fat), 2):
                e0 = self.fat[i]
                e1 = self.fat[i + 1] if i + 1 < len(self.fat) else 0xFFF
                raw += bytes([e0 & 0xFF, ((e0 >> 8) & 0x0F) | ((e1 & 0x0F) << 4),
                              (e1 >> 4) & 0xFF])
        else:
            for e in self.fat:
                raw += e.to_bytes(2, 'little')
        raw = raw[:self.spf * self.bps]
        raw += b'\x00' * (self.spf * self.bps - len(raw))
        for n in range(self.nfat):
            off = self.fat_off + n * self.spf * self.bps
            self.data[off:off + self.spf * self.bps] = raw

    def list_files(self):
        root = self.data[self.root_off:self.root_off + self.root_ents * 32]
        out = []
        for i in range(self.root_ents):
            e = root[i * 32:(i + 1) * 32]
            if not e or e[0] in (0x00, 0xE5):
                continue
            if e[11] == 0x0F:
                continue
            name = e[0:11].decode('ascii', 'replace').strip()
            size = struct.unpack_from('<I', e, 28)[0]
            start = struct.unpack_from('<H', e, 26)[0]
            out.append((name, size, start))
        return out

    def read_file(self, name):
        def flat(s):
            return s.strip().lower().replace('.', '').replace(' ', '')
        want = flat(name)
        for short, size, start in self.list_files():
            if flat(short) == want:
                return self.read_chain(start)[:size]
        return None

    def save(self, path):
        with open(path, 'wb') as f:
            f.write(bytes(self.data))


def main():
    img = Fat12Image(open(sys.argv[2], 'rb').read())
    if sys.argv[1] == 'list':
        for name, size, start in img.list_files():
            print('%-12s %8d  clus %d' % (name, size, start))
    elif sys.argv[1] == 'get':
        data = img.read_file(sys.argv[3])
        if data is None:
            sys.exit('not found')
        open(sys.argv[4], 'wb').write(data)
    elif sys.argv[1] == 'put':
        data = open(sys.argv[3], 'rb').read()
        img.write_file(sys.argv[4], data)
        img.save(sys.argv[2])
    else:
        sys.exit('usage: fat12.py list|get|put <image> [src] [dst]')


if __name__ == '__main__':
    main()


def make_fat16_disk(size_mb=32, part_lba=63):
    """Build a fresh FAT16 disk image from scratch (MBR + one partition).
    The partition is FAT16 with a DOS-style boot sector; the data area is
    empty (files are added with write_file). Used to give MS-DOS v6.22 a
    clean payload disk - the FreeDOS Lite partition triggers a boot stick
    in the MS-DOS IO.SYS drive setup."""
    import math
    total_sectors = size_mb * 1024 * 1024 // 512
    part_sectors = total_sectors - part_lba
    bps, spc, rsvd, nfat, root_ents = 512, 4, 1, 2, 512
    # FAT16 size: solve spf so the FAT covers all clusters
    root_sectors = root_ents * 32 // bps
    for spf in range(64, 1024):
        fat_bytes = spf * bps
        clusters = (part_sectors - rsvd - nfat * spf - root_sectors) // spc
        if clusters + 2 <= fat_bytes // 2:
            break
    img = bytearray(total_sectors * bps)
    # MBR with one partition (type 0x04 FAT16)
    mbr = bytearray(512)
    mbr[446 + 4] = 0x04
    mbr[446 + 8:446 + 12] = part_lba.to_bytes(4, 'little')
    mbr[446 + 12:446 + 16] = part_sectors.to_bytes(4, 'little')
    mbr[510:512] = b'\x55\xaa'
    img[0:512] = mbr
    # partition boot sector: BPB + a minimal stub
    bs = bytearray(512)
    bs[0:3] = b'\xeb\x3c\x90'
    bs[3:11] = b'MSDOS6.22'
    bs[11:13] = bps.to_bytes(2, 'little')
    bs[13] = spc
    bs[14:16] = rsvd.to_bytes(2, 'little')
    bs[16] = nfat
    bs[17:19] = root_ents.to_bytes(2, 'little')
    bs[19:21] = part_sectors.to_bytes(2, 'little')
    bs[21] = 0xF8
    bs[22:24] = spf.to_bytes(2, 'little')
    bs[24:26] = (63).to_bytes(2, 'little')
    bs[26:28] = (16).to_bytes(2, 'little')
    bs[28:32] = part_lba.to_bytes(4, 'little')
    bs[510:512] = b'\x55\xaa'
    img[part_lba * bps:part_lba * bps + 512] = bs
    # FATs: media + EOF for the first two entries
    fat = bytearray(spf * bps)
    fat[0] = 0xF8
    fat[1:2] = b'\xff'
    fat[2:4] = b'\xff\xff'
    for n in range(nfat):
        off = (part_lba + rsvd + n * spf) * bps
        img[off:off + len(fat)] = fat
    return img


def make_fat12_floppy():
    """Build a fresh 1.44MB FAT12 floppy image (2880 sectors, 18 spt,
    2 heads, spc 1, root 224, spf 9). Used as the payload floppy (B:) so
    the MS-DOS host never touches the FAT16/partition code path that
    sticks the IO.SYS boot."""
    img = bytearray(2880 * 512)
    bs = bytearray(512)
    bs[0:3] = b'\xeb\x3c\x90'
    bs[3:11] = b'MSDOS6.22'
    bs[11:13] = (512).to_bytes(2, 'little')
    bs[13] = 1
    bs[14:16] = (1).to_bytes(2, 'little')
    bs[16] = 2
    bs[17:19] = (224).to_bytes(2, 'little')
    bs[19:21] = (2880).to_bytes(2, 'little')
    bs[21] = 0xF0
    bs[22:24] = (9).to_bytes(2, 'little')
    bs[24:26] = (18).to_bytes(2, 'little')
    bs[26:28] = (2).to_bytes(2, 'little')
    bs[510:512] = b'\x55\xaa'
    img[0:512] = bs
    fat = bytearray(9 * 512)
    fat[0] = 0xF0
    fat[1:2] = b'\xff'
    fat[2:4] = b'\xff\xff'
    img[512:512 + 9 * 512] = fat
    img[512 + 9 * 512:512 + 18 * 512] = fat
    return img


def delete_file(self, name):
    """Remove a root file: free its chain and mark the entry 0xE5."""
    want = name.strip().lower().replace('.', '').replace(' ', '')
    root = self.data[self.root_off:self.root_off + self.root_ents * 32]
    for i in range(self.root_ents):
        e = root[i * 32:(i + 1) * 32]
        if not e or e[0] in (0x00, 0xE5):
            continue
        if e[11] in (0x0F, 0x08, 0x10):
            continue
        short = e[0:11].decode('ascii', 'replace').strip()
        if short.lower().replace('.', '').replace(' ', '') == want:
            import struct
            start = struct.unpack_from('<H', e, 26)[0]
            n = start
            seen = 0
            while 2 <= n < self._end() - 7 and seen < self.data_clusters:
                nxt = self.fat[n]
                self.fat[n] = 0
                n = nxt
                seen += 1
            self._write_fat()
            self.data[self.root_off + i * 32] = 0xE5
            return True
    return False


Fat12Image.delete_file = delete_file
