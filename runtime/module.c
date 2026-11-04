/* Lifted guest modules (module.h): war3.exe, Game.dll, Storm.dll, Mss32.dll,
 * ijl15.dll, msvcr80.dll and the Miles plugins, each lifted by gen.sh at a
 * fixed guest base.  This file is the Windows loader for them:
 *
 *   1. the file in the game folder must be the one the code was lifted from
 *      (SHA-256 pinned at gen time); anything else is refused and logged;
 *   2. headers and sections are copied, the rest of SizeOfImage is zero;
 *   3. the file's own HIGHLOW base relocations are applied for the new base,
 *      which reproduces the analysis copy the lifter saw (prepare_module.py);
 *      war3.exe has none and stays at 0x00400000;
 *   4. imports are bound: an import of another lifted module gets that
 *      module's export (by name or ordinal; Storm is imported by ordinal),
 *      loading it first; any other import gets IMP_MAGIC + its index in the
 *      combined import table, so a lifted `call [iat]` lands in win32_call;
 *   5. DllMain(PROCESS_ATTACH) runs for every module a load mapped, the
 *      dependencies first (post order), on the loading thread with the guest
 *      lock held (Windows: under the loader lock);
 *   6. FreeLibrary of the last reference runs DllMain(PROCESS_DETACH); the
 *      pages stay committed, so a thread still inside the module reads stale
 *      data instead of faulting; a later load copies a fresh image.
 *
 * Module handles are base addresses.  Based on nfsu2-web's module.c, which
 * did this for one DLL (server.dll). */
#include "cpu.h"
#include "module.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#ifdef __wasm__
void nfsu2_sha256(const void *data, size_t n, uint8_t out[32]);   /* runtime/web/sha256.c */
#define SHA256_LEN 32
#define SHA256(b, n, d) nfsu2_sha256((b), (n), (d))
#else
#include <CommonCrypto/CommonDigest.h>
#define SHA256_LEN CC_SHA256_DIGEST_LENGTH
#define SHA256(b, n, d) CC_SHA256((b), (CC_LONG)(n), (d))
#endif

uint32_t call_guest(CPU *c, uint32_t fn, int nargs, const uint32_t *args);
void mem_commit(uint32_t, uint32_t);
int fileio_resolve_game(const char *win, char *out, size_t outsz);
const char *fileio_game_dir(void);

#define IMP_MAGIC 0xF0000000u
#define DLL_PROCESS_DETACH 0u
#define DLL_PROCESS_ATTACH 1u
#define MODMAX 16

typedef struct {
    int mapped, refs, refused, attached;
    uint64_t loads, unloads;
    uint32_t imp_first;             /* index of its first import in the combined table */
    uint32_t unresolved;            /* imports of lifted modules with no such export */
    char path[1024];                /* host file it was mapped from */
} ModState;
static ModState g_st[MODMAX];
static int g_exe = -1;              /* g_modtab index of war3.exe */

/* ---- the combined import table (win32.c) ---- */
static const ImpEnt **g_impv; static uint8_t *g_impmod; static uint32_t g_imptot;
static void imp_index(void){
    if (g_impv) return;
    if (g_modtab_count > MODMAX){ fprintf(stderr, "[module] FATAL: %d modules, MODMAX %d\n", g_modtab_count, MODMAX); exit(1); }
    uint32_t n = 0;
    for (int i = 0; i < g_modtab_count; i++){ g_st[i].imp_first = n; n += *g_modtab[i].imp_count; }
    g_impv = calloc(n ? n : 1, sizeof *g_impv); g_impmod = calloc(n ? n : 1, 1); g_imptot = n;
    for (int i = 0; i < g_modtab_count; i++)
        for (uint32_t j = 0; j < *g_modtab[i].imp_count; j++){
            g_impv[g_st[i].imp_first + j] = &g_modtab[i].imp[j];
            g_impmod[g_st[i].imp_first + j] = (uint8_t)i;
        }
}
uint32_t module_imp_total(void){ imp_index(); return g_imptot; }
const ImpEnt *module_imp(uint32_t idx){ imp_index(); return idx < g_imptot ? g_impv[idx] : 0; }
int module_of_import(uint32_t idx){ imp_index(); return idx < g_imptot ? g_impmod[idx] : -1; }

/* "storm", "STORM.DLL", "C:\\x\\Storm.dll" -> "storm.dll" (Windows adds ".dll"
 * when the name has no extension; a trailing dot means none). */
static void norm_name(const char *in, char *out, size_t n){
    const char *b = in;
    for (const char *p = in; *p; p++) if (*p == '\\' || *p == '/') b = p + 1;
    size_t k = 0;
    for (; b[k] && k + 5 < n; k++) out[k] = (char)tolower((unsigned char)b[k]);
    out[k] = 0;
    if (k && out[k-1] == '.') out[k-1] = 0;
    else if (!strchr(out, '.') && k + 5 < n) strcat(out, ".dll");
}
static int by_name(const char *name){
    char nm[260]; norm_name(name, nm, sizeof nm);
    for (int i = 0; i < g_modtab_count; i++) if (!strcmp(nm, g_modtab[i].info->name)) return i;
    return -1;
}
static int by_handle(uint32_t h){
    for (int i = 0; i < g_modtab_count; i++) if (h == g_modtab[i].info->base) return i;
    return -1;
}

static void hexsha(const uint8_t *buf, size_t n, char out[65]){
    uint8_t d[SHA256_LEN];
    SHA256(buf, n, d);
    for (int i = 0; i < SHA256_LEN; i++) snprintf(out + 2*i, 3, "%02x", d[i]);
}
static uint32_t rd32(const uint8_t *p){ return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p){ return (uint16_t)(p[0] | p[1] << 8); }

/* Map and relocate module i from host file `host`.  0 + *err on failure. */
static int map_image(int i, const char *host, uint32_t *err){
    const ModInfo *mi = g_modtab[i].info;
    ModState *st = &g_st[i];
    FILE *f = fopen(host, "rb");
    if (!f){ fprintf(stderr, "[module] %s: cannot read %s\n", mi->name, host); *err = 126u; return 0; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = sz > 0 ? malloc((size_t)sz) : 0;
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz){ fclose(f); free(buf); *err = 126u; return 0; }
    fclose(f);
    char sha[65]; hexsha(buf, (size_t)sz, sha);
    if (strcmp(sha, mi->sha256)){
        if (!st->refused++)
            fprintf(stderr, "[module] %s in the game folder (%s, %ld bytes, sha256 %s) is not the file this build"
                            " was lifted from (sha256 %s, %u bytes): it is NOT loaded. The port needs Colin McRae Rally 2005 CZ"
                            " 1.26a (docs/PLAN.md).\n", mi->name, host, sz, sha, mi->sha256, mi->file_size);
        free(buf); *err = 193u; return 0;                        /* ERROR_BAD_EXE_FORMAT */
    }
    /* The hash pins the file, so these only guard against a broken gen. */
    uint32_t lfa = rd32(buf + 0x3c);
    const uint8_t *nt = buf + lfa, *opt = nt + 24;
    uint16_t nsec = rd16(nt + 6), optsz = rd16(nt + 20);
    uint32_t pref = rd32(opt + 28), imgsz = rd32(opt + 56), hdrsz = rd32(opt + 60), ep = rd32(opt + 16);
    if (rd32(nt) != 0x4550u || pref != mi->pref || imgsz != mi->size || mi->base + ep != mi->entry){
        fprintf(stderr, "[module] %s: PE layout does not match gen/ (base 0x%x size 0x%x entry 0x%x)\n",
                mi->name, pref, imgsz, ep);
        free(buf); *err = 193u; return 0;
    }
    uint32_t base = mi->base, delta = base - pref;
    mem_commit(base, imgsz);
    memset(g_mem + base, 0, imgsz);
    memcpy(g_mem + base, buf, hdrsz < (uint32_t)sz ? hdrsz : (uint32_t)sz);
    const uint8_t *sh = opt + optsz;
    for (int k = 0; k < nsec; k++, sh += 40){
        uint32_t vs = rd32(sh + 8), va = rd32(sh + 12), rs = rd32(sh + 16), ro = rd32(sh + 20);
        uint32_t n = rs < vs ? rs : vs;
        if (va + n > imgsz || ro + n > (uint32_t)sz){ free(buf); *err = 193u; return 0; }
        memcpy(g_mem + base + va, buf + ro, n);
    }
    free(buf);
    /* base relocations (DataDirectory[5]) */
    uint32_t nfix = 0;
    if (delta){
        uint32_t rva = MR32(base + lfa + 24 + 96 + 5*8), rsz = MR32(base + lfa + 24 + 96 + 5*8 + 4);
        for (uint32_t off = 0; off + 8 <= rsz; ){
            uint32_t page = MR32(base + rva + off), bsz = MR32(base + rva + off + 4);
            if (bsz < 8) break;
            for (uint32_t k = 8; k + 2 <= bsz; k += 2){
                uint16_t e = MR16(base + rva + off + k);
                uint32_t t = e >> 12, at = base + page + (e & 0xfffu);
                if (t == 0) continue;                            /* ABSOLUTE: padding */
                if (t != 3 || at + 4 > base + imgsz){
                    fprintf(stderr, "[module] %s: unsupported relocation type %u at 0x%08x\n", mi->name, t, at);
                    *err = 193u; return 0; }
                MR32(at) += delta; nfix++;
            }
            off += bsz;
        }
        MR32(base + lfa + 24 + 28) = base;                       /* the loader records the actual base */
    }
    snprintf(st->path, sizeof st->path, "%s", host);
    fprintf(stderr, "[module] %-12s mapped at 0x%08x-0x%08x (sha256 ok, %u fixups)\n", mi->name, base, base + imgsz, nfix);
    return 1;
}

/* export of mapped module i: by name, or by ordinal when nm <= 0xFFFF (nm is
 * a guest address of the name; host_name, if set, is used instead) */
static uint32_t export_of(int i, uint32_t nm, const char *host_name, uint32_t *err){
    *err = 127u;                                                 /* ERROR_PROC_NOT_FOUND */
    uint32_t base = g_modtab[i].info->base, lfa = MR32(base + 0x3c);
    uint32_t ed = MR32(base + lfa + 24 + 96), esz = MR32(base + lfa + 24 + 96 + 4);
    if (!ed) return 0;
    uint32_t E = base + ed;
    uint32_t obase = MR32(E + 0x10), nfun = MR32(E + 0x14), nnam = MR32(E + 0x18);
    uint32_t funs = base + MR32(E + 0x1c), nams = base + MR32(E + 0x20), ords = base + MR32(E + 0x24);
    uint32_t fi = 0xFFFFFFFFu;
    const char *want = host_name ? host_name : (nm > 0xFFFFu && mem_is_committed(nm) ? (const char *)(g_mem + nm) : 0);
    if (!want){ if (nm >= obase && nm - obase < nfun) fi = nm - obase; }
    else {
        /* the export name table is sorted: binary search */
        uint32_t lo = 0, hi = nnam;
        while (lo < hi){
            uint32_t md = (lo + hi) / 2;
            int r = strcmp((const char *)(g_mem + base + MR32(nams + 4*md)), want);
            if (!r){ fi = MR16(ords + 2*md); break; }
            if (r < 0) lo = md + 1; else hi = md;
        }
    }
    if (fi >= nfun) return 0;
    uint32_t frva = MR32(funs + 4*fi);
    if (!frva) return 0;
    if (frva >= ed && frva < ed + esz){
        fprintf(stderr, "[module] %s: export %s is forwarded to %s: not supported\n", g_modtab[i].info->name,
                want ? want : "(ordinal)", (const char *)(g_mem + base + frva));
        return 0;
    }
    *err = 0;
    return base + frva;
}

static int load_tree(int i, int *order, int *norder, uint32_t *err);

/* bind module i's imports; loads (maps, binds) the lifted modules it needs */
static int bind_imports(int i, int *order, int *norder, uint32_t *err){
    const ModEntry *me = &g_modtab[i];
    uint32_t base = me->info->base, imgsz = me->info->size, first = g_st[i].imp_first;
    uint32_t hle = 0, lifted = 0;
    g_st[i].unresolved = 0;
    for (uint32_t j = 0; j < *me->imp_count; j++){
        const ImpEnt *e = &me->imp[j];
        if (e->iat - base >= imgsz){ fprintf(stderr, "[module] %s: IAT slot 0x%08x outside the image\n", me->info->name, e->iat); *err = 193u; return 0; }
        int k = by_name(e->dll);
        if (k < 0){ MR32(e->iat) = IMP_MAGIC + first + j; hle++; continue; }
        if (!g_st[k].mapped && !load_tree(k, order, norder, err)){
            fprintf(stderr, "[module] %s needs %s, which did not load\n", me->info->name, e->dll);
            return 0;
        }
        uint32_t ee = 0, a = e->name[0] == '#' ? export_of(k, (uint32_t)strtoul(e->name + 1, 0, 10), 0, &ee)
                                                : export_of(k, 0, e->name, &ee);
        if (!a){
            /* left to the HLE: the call is then traced as not implemented */
            if (g_st[i].unresolved++ < 8)
                fprintf(stderr, "[module] %s: import %s!%s not exported by the lifted %s\n",
                        me->info->name, e->dll, e->name, g_modtab[k].info->name);
            MR32(e->iat) = IMP_MAGIC + first + j; hle++; continue;
        }
        MR32(e->iat) = a; lifted++;
    }
    fprintf(stderr, "[module] %-12s %u imports: %u bound to lifted modules, %u to the Win32 HLE%s\n",
            me->info->name, *me->imp_count, lifted, hle, g_st[i].unresolved ? " (some unresolved, see above)" : "");
    return 1;
}

/* the game folder, then redist\miles (Miles' providers Mp3dec.asi and
 * Mssfast.m3d, which Mss32 finds there by FindFirstFile) */
static int resolve_module(const char *name, char *host, size_t hsz){
    char w[300];
    if (fileio_resolve_game(name, host, hsz)) return 1;
    snprintf(w, sizeof w, "redist\\miles\\%s", name);
    return fileio_resolve_game(w, host, hsz);
}

/* map + bind module i and the lifted modules it imports; order[] gets the
 * newly mapped ones in post order (dependencies first) for their DllMain */
static int load_tree(int i, int *order, int *norder, uint32_t *err){
    if (g_st[i].mapped) return 1;
    const ModInfo *mi = g_modtab[i].info;
    char host[1024];
    if (i == g_exe && g_st[i].path[0]) snprintf(host, sizeof host, "%s", g_st[i].path);
    else if (!resolve_module(mi->name, host, sizeof host)){
        fprintf(stderr, "[module] %s: not found in the game folder\n", mi->name);
        *err = 126u; return 0;                                   /* ERROR_MOD_NOT_FOUND */
    }
    if (!map_image(i, host, err)) return 0;
    g_st[i].mapped = 1;                    /* before the imports: a cycle must not map it twice */
    if (!bind_imports(i, order, norder, err)){ g_st[i].mapped = 0; return 0; }
    g_st[i].loads++; g_st[i].refs = 1;
    order[(*norder)++] = i;
    return 1;
}

/* DllMain(hinst, PROCESS_ATTACH, lpReserved): Windows passes a non-NULL
 * lpReserved for the DLLs loaded with the process (static imports) and NULL
 * for LoadLibrary, and DLLs tell the two apart: Storm's DllMain prebuilds a
 * crypt table of another seed (0x10000100) when it is loaded dynamically,
 * which then stands in for the MPQ table and no file of an archive is found. */
static int attach(CPU *c, int i, int is_static){
    const ModInfo *mi = g_modtab[i].info;
    if (i == g_exe || g_st[i].attached) return 1;
    const uint32_t args[3] = { mi->base, DLL_PROCESS_ATTACH, is_static ? 1u : 0u };
    fprintf(stderr, "[module] %s: DllMain(PROCESS_ATTACH) ...\n", mi->name);
    uint32_t ok = call_guest(c, mi->entry, 3, args);
    fprintf(stderr, "[module] %s: DllMain(PROCESS_ATTACH) -> %u\n", mi->name, ok);
    if (!ok){
        const uint32_t a2[3] = { mi->base, DLL_PROCESS_DETACH, 0 };
        call_guest(c, mi->entry, 3, a2);
        g_st[i].mapped = 0; g_st[i].refs = 0; g_st[i].unloads++;
        return 0;
    }
    g_st[i].attached = 1;
    return 1;
}

/* ---- start-up ---- */
static int g_static_order[MODMAX], g_static_n;
int module_load_exe(const char *path){
    imp_index();
    for (int i = 0; i < g_modtab_count; i++)
        if (!strcmp(g_modtab[i].id, "exe")) g_exe = i;
    if (g_exe < 0){ fprintf(stderr, "[module] FATAL: gen/modtab.c has no CMR5.EXE (run ./gen.sh)\n"); return -1; }
    snprintf(g_st[g_exe].path, sizeof g_st[g_exe].path, "%s", path);
    uint32_t err = 0;
    g_static_n = 0;
    if (!load_tree(g_exe, g_static_order, &g_static_n, &err)){
        fprintf(stderr, "[module] FATAL: CMR5.EXE or a DLL it imports did not load (error %u)\n", err);
        return -1;
    }
    return 0;
}
int module_init_static(CPU *c){
    for (int k = 0; k < g_static_n; k++)
        if (!attach(c, g_static_order[k], 1)){
            fprintf(stderr, "[module] FATAL: %s failed to initialise\n", g_modtab[g_static_order[k]].info->name);
            return -1;
        }
    return 0;
}
uint32_t module_exe_entry(void){ return g_exe >= 0 ? g_modtab[g_exe].info->entry : 0; }

/* ---- LoadLibrary / FreeLibrary / GetProcAddress ---- */
uint32_t module_load(CPU *c, const char *name, uint32_t *err){
    int i = by_name(name);
    *err = 126u;
    if (i < 0) return 0;
    if (g_st[i].mapped){ g_st[i].refs++; *err = 0; return g_modtab[i].info->base; }
    int order[MODMAX], n = 0;
    if (!load_tree(i, order, &n, err)) return 0;
    for (int k = 0; k < n; k++)
        if (!attach(c, order[k], 0)){ *err = 1114u; return 0; }  /* ERROR_DLL_INIT_FAILED */
    *err = 0;
    return g_modtab[i].info->base;
}

int module_free(CPU *c, uint32_t h){
    int i = by_handle(h);
    if (i < 0) return 0;
    ModState *st = &g_st[i];
    if (i == g_exe || !st->mapped || st->refs <= 0) return 1;
    if (--st->refs > 0) return 1;
    const uint32_t args[3] = { h, DLL_PROCESS_DETACH, 0 };
    uint32_t r = call_guest(c, g_modtab[i].info->entry, 3, args);
    st->mapped = 0; st->attached = 0; st->unloads++;
    fprintf(stderr, "[module] %s: DllMain(PROCESS_DETACH) -> %u, unloaded (pages kept)\n", g_modtab[i].info->name, r);
    return 1;
}

int module_handle(uint32_t h){ return by_handle(h) >= 0; }
int module_known_name(const char *name){ return by_name(name) >= 0; }
uint32_t module_by_name(const char *name){
    int i = by_name(name);
    return i >= 0 && g_st[i].mapped ? g_modtab[i].info->base : 0u;
}
uint32_t module_proc(uint32_t h, uint32_t nm, uint32_t *err){
    int i = by_handle(h);
    if (i < 0 || !g_st[i].mapped){ *err = 126u; return 0; }
    return export_of(i, nm, 0, err);
}
int module_file_name(uint32_t h, char *out, int n){
    int i = h ? by_handle(h) : g_exe;
    if (i < 0 || !g_st[i].mapped) return 0;
    snprintf(out, (size_t)n, "%s\\%s", fileio_game_dir(), i == g_exe ? "CMR5.EXE" : g_modtab[i].info->name);
    return 1;
}
int module_image_at(uint32_t a, uint32_t *base, uint32_t *size){
    for (int i = 0; i < g_modtab_count; i++){
        const ModInfo *mi = g_modtab[i].info;
        if (g_st[i].mapped && a - mi->base < mi->size){
            if (base) *base = mi->base;
            if (size) *size = mi->size;
            return 1;
        }
    }
    return 0;
}

/* ---- backtraces ---- */
uint32_t module_nearest_fn(uint32_t a, const char **mod){
    for (int i = 0; i < g_modtab_count; i++){
        const ModEntry *m = &g_modtab[i];
        if (a - m->info->text >= m->info->text_size) continue;
        uint32_t lo = 0, hi = *m->fn_count, best = 0;
        while (lo < hi){ uint32_t md = (lo + hi) / 2;
            if (m->fn_addr[md] <= a){ best = m->fn_addr[md]; lo = md + 1; } else hi = md; }
        if (mod) *mod = m->id;
        return best;
    }
    return 0;
}
uint32_t module_fn_from_host(const void *pc){
    uintptr_t p = (uintptr_t)pc, best = 0; uint32_t bestg = 0;
    for (int i = 0; i < g_modtab_count; i++){
        const ModEntry *m = &g_modtab[i];
        for (uint32_t k = 0; k < *m->fn_count; k++){
            uintptr_t f = (uintptr_t)m->fn_ptr[k];
            if (f <= p && f > best){ best = f; bestg = m->fn_addr[k]; }
        }
    }
    return bestg;
}

void module_report(void){
    for (int i = 0; i < g_modtab_count; i++){
        const ModEntry *m = &g_modtab[i]; const ModState *st = &g_st[i];
        fprintf(stderr, "  module %-12s : %6u functions, %4u imports, %llu loads, %llu unloads, %s%s\n",
                m->info->name, *m->fn_count, *m->imp_count, (unsigned long long)st->loads,
                (unsigned long long)st->unloads, st->mapped ? "loaded" : "not loaded",
                st->refused ? ", REFUSED (sha256 mismatch)" : "");
    }
}
