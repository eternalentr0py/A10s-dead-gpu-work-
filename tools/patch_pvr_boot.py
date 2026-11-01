#!/usr/bin/env python3
"""Patch the PowerVR (Rogue) kernel driver inside an Android boot image so a dead GPU firmware does not
mark the driver "bad".  Targets the A10s kernel (A107FXXS8CWD3, arm64, gzip Image, boot header v2).

  mode poll   : PVRSRVPollForValueKM() returns PVRSRV_OK immediately (all firmware waits "succeed")
  mode fwstart: only the 'FW started' wait inside RGXPostPowerState() is skipped

Usage: patch_pvr_boot.py <in boot.img> <out boot.img> --mode poll|fwstart
Needs: pip install capstone zopfli
The patched kernel is recompressed with zopfli (smaller than the original gzip -9) and zero-padded to the
original kernel_size, so no other part of the boot image moves. AVB footer is left untouched (the image will
no longer verify; the bootloader must be unlocked).
"""
import argparse, hashlib, struct, sys, zlib

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('inp'); ap.add_argument('out'); ap.add_argument('--mode', choices=['poll', 'fwstart'], required=True)
    ap.add_argument('--iterations', type=int, default=3)
    a = ap.parse_args()
    d = bytearray(open(a.inp, 'rb').read())
    assert d[:8] == b'ANDROID!', 'not an Android boot image'
    ksz, kaddr, rsz, raddr, ssz, saddr, tags, psz, ver, osv = struct.unpack('<10I', d[8:48])
    assert ver == 2 and rsz == 0 and ssz == 0, 'unexpected layout'
    koff = psz
    kern = bytes(d[koff:koff + ksz])
    dec = zlib.decompressobj(16 + zlib.MAX_WBITS)
    img = bytearray(dec.decompress(kern))
    print('kernel Image:', len(img), 'bytes; arm64 magic', bytes(img[0x38:0x3c]))

    # ---- locate PVRSRVPollForValueKM by its signature: 5 s timeout constant 0x4c4b40 loaded via movw/movk w28 ----
    def movz_w(rd, imm): return 0x52800000 | (imm << 5) | rd
    def movk_w16(rd, imm): return 0x72A00000 | (imm << 5) | rd
    needle = struct.pack('<I', movz_w(28, 0x4b40))      # mov  w28, #0x4b40
    needle_k = struct.pack('<I', movk_w16(28, 0x4c))    # movk w28, #0x4c, lsl #16   (=> 5,000,000 us timeout)
    cands = []
    pos = 0
    while True:
        p = img.find(needle, pos)
        if p < 0: break
        if p % 4 == 0:
            for q in range(p + 4, p + 40, 4):
                if img[q:q + 4] == needle_k: cands.append(p); break
        pos = p + 1
    assert len(cands) == 1, f'expected 1 poll-function signature, found {len(cands)}'
    p = cands[0]
    # walk back to the prologue: sub sp, sp, #0x70
    SUB_SP_70 = 0xD10003FF | (0x70 << 10)
    fs = None
    for o in range(p, p - 0x80, -4):
        if struct.unpack('<I', img[o:o + 4])[0] == SUB_SP_70: fs = o; break
    assert fs is not None, 'prologue not found'
    print(f'PVRSRVPollForValueKM at image offset {fs:#x}')
    MOV_W0_WZR = 0x2A1F03E0; RET = 0xD65F03C0
    if a.mode == 'poll':
        patches = {fs: MOV_W0_WZR, fs + 4: RET}
        orig_expect = {fs: SUB_SP_70}
    else:
        # call site in RGXPostPowerState: mov w2,#1; mov w3,#-1; mov w4,#3; ...; bl poll  -> replace the bl with mov w0,wzr
        calls = []
        for o in range(0, len(img) - 4, 4):
            w = struct.unpack('<I', img[o:o + 4])[0]
            if (w >> 26) == 0x25:
                off = w & 0x3ffffff
                if off & 0x2000000: off -= 0x4000000
                if o + off * 4 == fs:
                    prev = [struct.unpack('<I', img[o - 4 * k:o - 4 * k + 4])[0] for k in range(1, 8)]
                    if movz_w(4, 3) in prev and (0x12800000 | (0 << 5) | 3) in prev:   # mov w4,#3 and mov w3,#-1 nearby
                        calls.append(o)
        assert len(calls) == 1, f'expected 1 FW-started call site, found {len(calls)}'
        patches = {calls[0]: MOV_W0_WZR}
        orig_expect = {}
        print(f'FW-started call site at {calls[0]:#x}')
    for off, new in patches.items():
        old = struct.unpack('<I', img[off:off + 4])[0]
        if off in orig_expect: assert old == orig_expect[off]
        print(f'  {off:#x}: {old:#010x} -> {new:#010x}')
        img[off:off + 4] = struct.pack('<I', new)

    # ---- recompress, verify, pad ----
    import zopfli.gzip as zg
    gz = zg.compress(bytes(img), numiterations=a.iterations)
    assert len(gz) <= ksz, f'recompressed kernel too big ({len(gz)} > {ksz})'
    chk = zlib.decompressobj(16 + zlib.MAX_WBITS).decompress(gz)
    assert chk == bytes(img), 'round-trip mismatch'
    newk = gz + b'\0' * (ksz - len(gz))
    d[koff:koff + ksz] = newk
    # header id = sha1(kernel,ksz, ramdisk,rsz, second,ssz, recovery_dtbo,size, dtb,size)  (AOSP mkbootimg)
    dtb_size = struct.unpack('<I', d[1648:1652])[0]
    dtb_off = koff + (ksz + psz - 1) // psz * psz
    dtb = bytes(d[dtb_off:dtb_off + dtb_size])
    h = hashlib.sha1()
    for blob, size in ((newk, ksz), (b'', 0), (b'', 0), (b'', 0), (dtb, dtb_size)):
        h.update(blob); h.update(struct.pack('<I', size))
    d[576:576 + 20] = h.digest()
    open(a.out, 'wb').write(d)
    print(f'wrote {a.out}: {len(d)} bytes; kernel gz {len(gz)} of {ksz}; sha1 id updated')

if __name__ == '__main__':
    main()
