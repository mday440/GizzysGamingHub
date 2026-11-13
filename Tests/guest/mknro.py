"""Build an NRO (homebrew) fixture: text linked at +0x80 behind the NRO header, plus an ASET."""
import struct, subprocess, sys
elf, out = sys.argv[1], sys.argv[2]
def section(name):
    subprocess.run(["llvm-objcopy", "-O", "binary", "--only-section=" + name, elf, "/tmp/sec.bin"], check=True)
    return open("/tmp/sec.bin", "rb").read()
def addr(name):
    o = subprocess.run(["llvm-readelf", "-S", "-W", elf], capture_output=True, text=True).stdout
    for line in o.splitlines():
        p = line.replace("[", " ").replace("]", " ").split()
        if name in p: i = p.index(name); return int(p[i+2], 16), int(p[i+4], 16)
base = 0x8000000
ta, _ = addr(".text"); ra, _ = addr(".rodata"); da, _ = addr(".data"); ba = addr(".bss")
text, ro, data = section(".text"), section(".rodata"), section(".data")
bss = ba[0] + ba[1] - (da + len(data))
img = bytearray(da - base + len(data))
img[ta-base:ta-base+len(text)] = text; img[ra-base:ra-base+len(ro)] = ro; img[da-base:] = data
struct.pack_into("<I", img, 0, 0x14000020)          # b +0x80 (over the header)
img[0x10:0x14] = b"NRO0"
struct.pack_into("<III", img, 0x14, 0, len(img), 0)
struct.pack_into("<IIIIII", img, 0x20, 0, ra-base, ra-base, da-ra, da-base, len(data))
struct.pack_into("<I", img, 0x38, bss)
img[0x40:0x54] = bytes(range(0x30, 0x44))
nacp = bytearray(0x4000); nacp[0:13] = b"Hello Brewery"; nacp[0x200:0x20B] = b"Mike Homebr"; nacp[0x3060:0x3063] = b"0.9"
icon = b"\x89PNG\r\n\x1a\nFAKEPNG"
aset = bytearray(0x38); aset[0:4] = b"ASET"
struct.pack_into("<QQQQ", aset, 8, 0x38, len(icon), 0x38 + len(icon), len(nacp))
open(out, "wb").write(bytes(img) + bytes(aset) + icon + bytes(nacp))
