/*
 * MikesGameRoom core - public C API.
 *
 * Game containers (NUP, extracted folders, NSP, XCI, NCA, NSO, NRO) are opened
 * into an mgr_game, which exposes metadata plus the ExeFS program modules.
 * An mgr_emu runs those modules on the built-in AArch64 interpreter with a
 * high-level (HLE) kernel stub.
 *
 * Encrypted formats (NSP/XCI/NCA) need the user's own console keys
 * (prod.keys / title.keys). No keys ship with this software.
 */
#ifndef MGR_H
#define MGR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MGR_FMT_UNKNOWN = 0,
    MGR_FMT_NUP,
    MGR_FMT_FOLDER,
    MGR_FMT_NSP,
    MGR_FMT_XCI,
    MGR_FMT_NCA,
    MGR_FMT_NSO,
    MGR_FMT_NRO,
    MGR_FMT_NSZ,   /* recognised, not supported yet */
    MGR_FMT_XCZ,   /* recognised, not supported yet */
} mgr_format;

typedef enum {
    MGR_PLATFORM_UNKNOWN = 0,
    MGR_PLATFORM_SWITCH1,
    MGR_PLATFORM_SWITCH2,
    MGR_PLATFORM_HOMEBREW,
} mgr_platform;

typedef struct {
    mgr_format   format;
    mgr_platform platform;
    uint64_t     title_id;
    uint32_t     version;          /* NCM version of the newest content (0 if unknown) */
    char         name[256];
    char         publisher[128];
    char         display_version[32];
    char         format_name[16];
    uint8_t      build_id[32];
    int          has_build_id;
    int          has_icon;
    int          package_count;    /* base + updates + dlc found in the container */
    int          update_applied;   /* 1 if ExeFS/RomFS come from an update overlay */
    uint64_t     romfs_file_count;
    int          exefs_count;
    char         exefs_names[16][32];
    int          bootable;         /* has an ExeFS with main.npdm + main (or an NRO/NSO) */
    char         note[256];        /* human-readable caveat, may be empty */
} mgr_game_info;

typedef struct mgr_game mgr_game;

/* ---- keys ---- */
/* Load a prod.keys / title.keys style file ("name = hex"). Returns number of keys read, -1 on error. */
int  mgr_keys_load(const char *path);
int  mgr_keys_count(void);
int  mgr_keys_have_header_key(void);

/* ---- games ---- */
mgr_format mgr_detect(const char *path);
mgr_game  *mgr_open(const char *path, char *err, size_t errlen);
void       mgr_close(mgr_game *g);
const mgr_game_info *mgr_info(const mgr_game *g);
/* Icon bytes (JPEG/PNG), owned by the game. Returns 0 if none. */
int        mgr_icon(const mgr_game *g, const uint8_t **data, size_t *len);
/* Read a whole ExeFS file (e.g. "main", "main.npdm"). Caller frees *out with free(). */
int        mgr_exefs_read(mgr_game *g, const char *name, uint8_t **out, size_t *len);
/* Read a RomFS file by '/'-separated path (no leading slash needed). Caller frees. */
int        mgr_romfs_read(mgr_game *g, const char *path, uint8_t **out, size_t *len);

/* ---- emulation ---- */
typedef enum {
    MGR_EMU_READY = 0,
    MGR_EMU_RUNNING,
    MGR_EMU_EXITED,        /* guest called svcExitProcess */
    MGR_EMU_BREAK,         /* guest called svcBreak / abort */
    MGR_EMU_UNIMPLEMENTED, /* hit an instruction the interpreter does not support yet */
    MGR_EMU_FAULT,         /* memory fault */
    MGR_EMU_BLOCKED,       /* needs an OS service that is not emulated yet */
} mgr_emu_status;

typedef struct {
    mgr_emu_status status;
    uint64_t pc;
    uint64_t instructions;
    uint64_t svc_calls;
    uint64_t x[31];
    uint64_t sp;
    char     message[256];
} mgr_emu_state;

typedef struct mgr_emu mgr_emu;

mgr_emu *mgr_emu_create(mgr_game *g, char *err, size_t errlen);
void     mgr_emu_destroy(mgr_emu *e);
/* Run up to max_instructions; returns current status. Safe to call repeatedly. */
mgr_emu_status mgr_emu_run(mgr_emu *e, uint64_t max_instructions);
void     mgr_emu_get_state(const mgr_emu *e, mgr_emu_state *out);
/* Drain the emulator log (module map, SVC trace, guest debug output).
   Copies up to buflen-1 bytes, returns bytes copied. */
size_t   mgr_emu_take_log(mgr_emu *e, char *buf, size_t buflen);

/* Copy guest memory (for debuggers / tools). Returns 0 on success, -1 if unmapped. */
int      mgr_emu_read_mem(mgr_emu *e, uint64_t addr, void *buf, size_t n);
/* Log the current thread's backtrace (and every other thread's pc) to the emulator log. */
void     mgr_emu_dump_threads(mgr_emu *e);

const char *mgr_version(void);

#ifdef __cplusplus
}
#endif
#endif
