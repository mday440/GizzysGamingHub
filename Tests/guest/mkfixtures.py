"""Build NSO/NRO/NPDM test fixtures from the linked guest ELF."""
import struct, subprocess, sys, os, lz4.block
elf, out = sys.argv[1], sys.argv[2]
def section(name):
    r = subprocess.run(["llvm-objcopy", "-O", "binary", "--only-section=" + name, elf, "/tmp/sec.bin"], check=True)
    return open("/tmp/sec.bin", "rb").read()
def addr(name):
    o = subprocess.run(["llvm-readelf", "-S", "-W", elf], capture_output=True, text=True).stdout
    for line in o.splitlines():
        parts = line.replace("[", " ").replace("]", " ").split()
        if name in parts:
            i = parts.index(name); return int(parts[i+2], 16), int(parts[i+4], 16)
    return None
text, ro, data = section(".text"), section(".rodata"), section(".data")
ta, _ = addr(".text"); ra, _ = addr(".rodata")
ba = addr(".bss")
dd = addr(".data")
da = dd[0] if dd else (ba[0] if ba else ra + 0x1000)
bss = (ba[0] + ba[1] - (da + len(data))) if ba else 0
base = 0x8000000
segs = [(text, ta - base), (ro, ra - base), (data, da - base)]
def nso(compress):
    hdr = bytearray(0x100); hdr[0:4] = b"NSO0"
    body = b""; off = 0x100; flags = 0
    for i, (seg, moff) in enumerate(segs):
        payload = lz4.block.compress(seg, store_size=False) if compress else seg
        if compress: flags |= 1 << i
        struct.pack_into("<IIII", hdr, 0x10 + i*0x10, off, moff, len(seg), bss if i == 2 else 0)
        struct.pack_into("<I", hdr, 0x60 + i*4, len(payload))
        body += payload; off += len(payload)
    struct.pack_into("<I", hdr, 0xC, flags)
    hdr[0x40:0x54] = bytes(range(0xA0, 0xB4))
    return bytes(hdr) + body
def npdm(pid):
    d = bytearray(0x400); d[0:4] = b"META"; d[0xC] = 1; d[0xE] = 44; d[0xF] = 0
    struct.pack_into("<I", d, 0x1C, 0x100000); d[0x20:0x20+9] = b"mgr-hello"
    struct.pack_into("<IIII", d, 0x70, 0x200, 0x100, 0x80, 0x100)
    d[0x200:0x204] = b"ACI0"; struct.pack_into("<Q", d, 0x210, pid)
    return bytes(d)
def nro():
    img = bytearray(); 
    end = max(m + len(s) for s, m in segs)
    img = bytearray(end)
    for s, m in segs: img[m:m+len(s)] = s
    # NRO header lives inside text at 0x0..0x80: our text starts with _start code, so
    # prepend nothing - instead place header in a copy where bytes 0x10.. are header.
    return img
os.makedirs(out + "/dump/exefs", exist_ok=True)
open(out + "/dump/exefs/main", "wb").write(nso(True))
open(out + "/dump/exefs/main.npdm", "wb").write(npdm(0x0400F00DCAFE0000))
open(out + "/hello_raw.nso", "wb").write(nso(False))
os.makedirs(out + "/dump/romfs/data", exist_ok=True)
open(out + "/dump/romfs/data/readme.txt", "w").write("romfs works\n")
print("segments", [(hex(m), len(s)) for s, m in segs], "bss", bss)
