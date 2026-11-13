"""Build encrypted NCA / NSP / XCI fixtures with throwaway TEST keys (not console keys).

Program NCA: section 0 = ExeFS PFS0 (AES-CTR), section 1 = RomFS IVFC (AES-CTR)
Control NCA: section 0 = RomFS with control.nacp + icon_AmericanEnglish.dat
"""
import os, struct, sys
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

out = sys.argv[1]
exefs_dir = sys.argv[2]
rnd = __import__("random").Random(1234)
def rb(n): return bytes(rnd.getrandbits(8) for _ in range(n))
HEADER_KEY = rb(32)
KAK = rb(16)
CTR_KEY = rb(16)
TID = 0x0100ABCD12340000

def aes_ecb_enc(key, data): e = Cipher(algorithms.AES(key), modes.ECB()).encryptor(); return e.update(data) + e.finalize()
def aes_ctr(key, iv, data): e = Cipher(algorithms.AES(key), modes.CTR(iv)).encryptor(); return e.update(data) + e.finalize()
def xts_enc(key, data, sector0):
    o = b""
    for i in range(len(data) // 0x200):
        e = Cipher(algorithms.AES(key), modes.XTS((sector0 + i).to_bytes(16, "big"))).encryptor()
        o += e.update(data[i*0x200:(i+1)*0x200]) + e.finalize()
    return o
def pad(b, a): return b + b"\0" * ((-len(b)) % a)

def pfs0(files):
    names = b""; offs = []; data = b""
    for n, d in files:
        offs.append((len(data), len(d), len(names))); names += n.encode() + b"\0"; data += d
    names = pad(names, 0x20)
    h = b"PFS0" + struct.pack("<III", len(files), len(names), 0)
    for (o, s, so) in offs: h += struct.pack("<QQII", o, s, so, 0)
    return h + names + data

def hfs0(files):
    names = b""; offs = []; data = b""
    for n, d in files:
        offs.append((len(data), len(d), len(names))); names += n.encode() + b"\0"; data += pad(d, 0x200)
    names = pad(names, 0x20)
    h = b"HFS0" + struct.pack("<III", len(files), len(names), 0)
    for (o, s, so) in offs: h += struct.pack("<QQIIQ", o, s, so, 0x200, 0) + b"\0" * 0x20
    return h + names + data

def romfs(tree):
    """tree: dict name -> bytes | dict"""
    dirs, files, data = bytearray(), bytearray(), bytearray()
    NONE = 0xFFFFFFFF
    def ent_len(name, base): return (base + len(name) + 3) & ~3
    # assign offsets breadth-first
    dlist = [("", tree, NONE)]; doffs = {id(tree): 0}; cur = ent_len(b"", 0x18)
    flist = []
    i = 0
    while i < len(dlist):
        name, node, parent = dlist[i]; i += 1
        for k, v in node.items():
            if isinstance(v, dict):
                doffs[id(v)] = cur; cur += ent_len(k.encode(), 0x18); dlist.append((k, v, doffs[id(node)]))
    foffs = {}; fcur = 0
    for name, node, parent in dlist:
        for k, v in node.items():
            if not isinstance(v, dict): foffs[(id(node), k)] = fcur; fcur += ent_len(k.encode(), 0x20)
    dirs = bytearray(cur); files = bytearray(fcur)
    for name, node, parent in dlist:
        sub = [(k, v) for k, v in node.items() if isinstance(v, dict)]
        fl = [(k, v) for k, v in node.items() if not isinstance(v, dict)]
        off = doffs[id(node)]
        child_d = doffs[id(sub[0][1])] if sub else NONE
        child_f = foffs[(id(node), fl[0][0])] if fl else NONE
        struct.pack_into("<IIIIII", dirs, off, parent if parent != NONE else 0, NONE, child_d, child_f, NONE, len(name))
        dirs[off+0x18:off+0x18+len(name)] = name.encode()
        for j, (k, v) in enumerate(sub):
            sib = doffs[id(sub[j+1][1])] if j + 1 < len(sub) else NONE
            struct.pack_into("<I", dirs, doffs[id(v)] + 4, sib)
        for j, (k, v) in enumerate(fl):
            fo = foffs[(id(node), k)]
            sib = foffs[(id(node), fl[j+1][0])] if j + 1 < len(fl) else NONE
            doff = len(data); data += pad(v, 16)
            struct.pack_into("<IIQQII", files, fo, off, sib, doff, len(v), NONE, len(k))
            files[fo+0x20:fo+0x20+len(k)] = k.encode()
    hdr_end = 0x50
    dh = struct.pack("<I", NONE); fh = struct.pack("<I", NONE)
    o_dh = hdr_end; o_dm = pad_to(o_dh + len(dh)); o_fh = pad_to(o_dm + len(dirs)); o_fm = pad_to(o_fh + len(fh)); o_data = pad_to(o_fm + len(files), 0x200)
    img = bytearray(o_data + len(data))
    struct.pack_into("<QQQQQQQQQQ", img, 0, 0x50, o_dh, len(dh), o_dm, len(dirs), o_fh, len(fh), o_fm, len(files), o_data)
    img[o_dh:o_dh+4] = dh; img[o_dm:o_dm+len(dirs)] = dirs; img[o_fh:o_fh+4] = fh; img[o_fm:o_fm+len(files)] = files
    img[o_data:] = data
    return bytes(img)
def pad_to(v, a=4): return (v + a - 1) & ~(a - 1)

def make_nca(content_type, sections):
    """sections: list of (fs_type, payload) ; payload already laid out (pfs0 / romfs)."""
    hdr = bytearray(0xC00)
    body = bytearray()
    cur = 0xC00
    for idx, (fs_type, payload) in enumerate(sections):
        fs = bytearray(0x200)
        fs[0:2] = struct.pack("<H", 2); fs[2] = fs_type; fs[3] = 2 if fs_type == 1 else 3; fs[4] = 3
        sec = bytearray(0x200) + bytearray(payload)  # 0x200 bytes of "hash table" first
        sec = pad(bytes(sec), 0x200)
        if fs_type == 1:
            struct.pack_into("<QQ", fs, 8 + 0x38, 0x200, len(payload))
        else:
            fs[8:12] = b"IVFC"; struct.pack_into("<I", fs, 8 + 12, 7)
            struct.pack_into("<QQ", fs, 8 + 0x10 + 5 * 0x18, 0x200, len(payload))
        gen_ctr = struct.pack("<Q", 0x1122334400000000 + idx)
        fs[0x140:0x148] = gen_ctr
        iv = bytes(reversed(gen_ctr)) + (cur >> 4).to_bytes(8, "big")
        body += aes_ctr(CTR_KEY, iv, sec)
        struct.pack_into("<II", hdr, 0x240 + idx * 0x10, cur // 0x200, (cur + len(sec)) // 0x200)
        hdr[0x400 + idx * 0x200: 0x600 + idx * 0x200] = fs
        cur += len(sec)
    hdr[0x200:0x204] = b"NCA3"; hdr[0x205] = content_type
    struct.pack_into("<Q", hdr, 0x208, cur); struct.pack_into("<Q", hdr, 0x210, TID)
    keyarea = bytearray(0x40); keyarea[0x20:0x30] = CTR_KEY
    hdr[0x300:0x340] = aes_ecb_enc(KAK, bytes(keyarea))
    return xts_enc(HEADER_KEY, bytes(hdr), 0) + bytes(body)

exefs = [(n, open(os.path.join(exefs_dir, n), "rb").read()) for n in ("main", "main.npdm")]
prog = make_nca(0, [(1, pfs0(exefs)), (0, romfs({"data": {"readme.txt": b"romfs inside an NCA\n"}}))])
nacp = bytearray(0x4000)
nacp[0:13] = b"NCA Test Game"; nacp[0x200:0x20B] = b"Mike's Labs"; nacp[0x3060:0x3065] = b"1.2.3"
icon = b"\xff\xd8\xff\xe0FAKEJPEG"
ctrl = make_nca(2, [(0, romfs({"control.nacp": bytes(nacp), "icon_AmericanEnglish.dat": icon}))])
os.makedirs(out, exist_ok=True)
open(out + "/program.nca", "wb").write(prog)
open(out + "/test.nsp", "wb").write(pfs0([("aaaa.nca", prog), ("bbbb.nca", ctrl)]))
secure = hfs0([("aaaa.nca", prog), ("bbbb.nca", ctrl)])
root = hfs0([("update", hfs0([])), ("normal", hfs0([])), ("secure", secure)])
xci = bytearray(0x200); xci[0x100:0x104] = b"HEAD"; struct.pack_into("<Q", xci, 0x130, 0x200)
open(out + "/test.xci", "wb").write(bytes(xci) + root)
with open(out + "/test.keys", "w") as f:
    f.write("# throwaway TEST keys for MikesGameRoom unit tests - not console keys\n")
    f.write("header_key = %s\n" % HEADER_KEY.hex())
    f.write("key_area_key_application_00 = %s\n" % KAK.hex())
print("fixtures written")
