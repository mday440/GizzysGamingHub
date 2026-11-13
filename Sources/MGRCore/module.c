/* NSO / NRO program modules and NPDM program metadata. */
#include "mgr_internal.h"

#define PAGE 0x1000u
static uint32_t page_up(uint32_t v) { return (v + PAGE - 1) & ~(PAGE - 1); }

void module_free(mgr_module *m) { free(m->image); memset(m, 0, sizeof *m); }

int nso_load(const uint8_t *b, size_t len, mgr_module *m, char *err, size_t errlen) {
    memset(m, 0, sizeof *m);
    if (len < 0x100 || memcmp(b, "NSO0", 4) != 0) { mgr_seterr(err, errlen, "not an NSO"); return -1; }
    uint32_t flags = rd32(b + 0xC);
    uint32_t foff[3], moff[3], msz[3], csz[3];
    for (int i = 0; i < 3; i++) {
        foff[i] = rd32(b + 0x10 + i * 0x10);
        moff[i] = rd32(b + 0x14 + i * 0x10);
        msz[i]  = rd32(b + 0x18 + i * 0x10);
        csz[i]  = rd32(b + 0x60 + i * 4);
    }
    uint32_t bss = rd32(b + 0x3C);
    memcpy(m->module_id, b + 0x40, 32);
    uint32_t end = page_up(moff[2] + msz[2] + bss);
    if (end > (1u << 30)) { mgr_seterr(err, errlen, "NSO too large"); return -1; }
    m->image = calloc(1, end);
    if (!m->image) { mgr_seterr(err, errlen, "out of memory"); return -1; }
    m->image_size = end;
    for (int i = 0; i < 3; i++) {
        int comp = (flags >> i) & 1;
        uint32_t in_sz = comp ? csz[i] : msz[i];
        if ((uint64_t)foff[i] + in_sz > len || (uint64_t)moff[i] + msz[i] > end) {
            mgr_seterr(err, errlen, "NSO segment %d out of bounds", i); module_free(m); return -1;
        }
        if (comp && in_sz >= 4 && memcmp(b + foff[i], "ZBIC", 4) == 0) {
            /* Switch 2 SDK modules: ZBIC (zstd variant) instead of LZ4 */
            if (zstd_decompress_frame(b + foff[i], in_sz, m->image + moff[i], msz[i])) {
                mgr_seterr(err, errlen, "NSO segment %d failed to decompress (ZBIC)", i); module_free(m); return -1;
            }
        } else if (comp) {
            if (lz4_decompress_block(b + foff[i], in_sz, m->image + moff[i], msz[i])) {
                mgr_seterr(err, errlen, "NSO segment %d failed to decompress", i); module_free(m); return -1;
            }
        } else {
            memcpy(m->image + moff[i], b + foff[i], msz[i]);
        }
    }
    m->text_off = moff[0]; m->text_size = msz[0];
    m->ro_off = moff[1];   m->ro_size = msz[1];
    m->data_off = moff[2]; m->data_size = msz[2];
    m->bss_size = bss;
    return 0;
}

int nro_load(const uint8_t *b, size_t len, mgr_module *m, char *err, size_t errlen) {
    memset(m, 0, sizeof *m);
    if (len < 0x80 || memcmp(b + 0x10, "NRO0", 4) != 0) { mgr_seterr(err, errlen, "not an NRO"); return -1; }
    uint32_t size = rd32(b + 0x18);
    uint32_t off[3], sz[3];
    for (int i = 0; i < 3; i++) { off[i] = rd32(b + 0x20 + i * 8); sz[i] = rd32(b + 0x24 + i * 8); }
    uint32_t bss = rd32(b + 0x38);
    if (size > len) { mgr_seterr(err, errlen, "NRO truncated"); return -1; }
    uint32_t end = page_up(off[2] + sz[2] + bss);
    m->image = calloc(1, end);
    if (!m->image) { mgr_seterr(err, errlen, "out of memory"); return -1; }
    m->image_size = end;
    for (int i = 0; i < 3; i++) {
        if ((uint64_t)off[i] + sz[i] > size) { mgr_seterr(err, errlen, "NRO segment out of bounds"); module_free(m); return -1; }
        memcpy(m->image + off[i], b + off[i], sz[i]);
    }
    memcpy(m->module_id, b + 0x40, 32);
    m->text_off = off[0]; m->text_size = sz[0];
    m->ro_off = off[1];   m->ro_size = sz[1];
    m->data_off = off[2]; m->data_size = sz[2];
    m->bss_size = bss;
    return 0;
}

int npdm_parse(const uint8_t *b, size_t len, mgr_npdm *o) {
    memset(o, 0, sizeof *o);
    if (len < 0x80 || memcmp(b, "META", 4) != 0) return -1;
    o->is_64bit = b[0xC] & 1;
    o->main_priority = b[0xE];
    o->main_core = b[0xF];
    o->main_stack_size = rd32(b + 0x1C);
    o->system_resource_size = rd32(b + 0x14);
    memcpy(o->name, b + 0x20, 16);
    o->name[16] = 0;
    uint32_t aci_off = rd32(b + 0x70), aci_sz = rd32(b + 0x74);
    o->acid_size = rd32(b + 0x7C);
    if ((uint64_t)aci_off + 0x18 <= len && aci_sz >= 0x18 && memcmp(b + aci_off, "ACI0", 4) == 0)
        o->program_id = rd64(b + aci_off + 0x10);
    o->ok = 1;
    return 0;
}
