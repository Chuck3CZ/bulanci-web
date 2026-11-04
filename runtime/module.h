/* Lifted guest modules: war3.exe and the DLLs gen.sh lifted next to it
 * (Game.dll, Storm.dll, Mss32.dll, ijl15.dll, msvcr80.dll, the Miles
 * plugins).  tools/prepare_module.py (tools/ptrscan.py for the exe) writes a
 * ModInfo for each into gen/<module>/modinfo.c and gen.sh lists them in
 * gen/modtab.c; runtime/module.c maps the original files, relocates them to
 * the bases the code was lifted for, binds their imports (to each other's
 * exports, or to the Win32 HLE) and runs their DllMain. */
#ifndef MODULE_H
#define MODULE_H
#include <stdint.h>

typedef struct {
    const char *name;         /* lower-case file name */
    const char *sha256;       /* of the original file the code was lifted from */
    uint32_t base;            /* guest base the code was lifted for */
    uint32_t pref;            /* the file's preferred ImageBase */
    uint32_t size;            /* SizeOfImage */
    uint32_t entry;           /* entry point at base (DllMain or the exe's start) */
    uint32_t text, text_size; /* .text at base */
    uint32_t file_size;
} ModInfo;

/* one import of a module: its IAT slot, DLL, name ("#n" = ordinal n) and the
 * stdcall argument count tools/argcount.py inferred (the HLE pops that many) */
typedef struct { uint32_t iat; const char *dll; const char *name; int nargs; } ImpEnt;

struct CPU;
typedef void (*mod_fn)(struct CPU *);
typedef struct {
    const char *id;                         /* gen.sh's module name: war3, game, storm, ... */
    const ModInfo *info;
    const ImpEnt *imp; const uint32_t *imp_count;
    const uint32_t *fn_addr; const mod_fn *fn_ptr; const uint32_t *fn_count;
} ModEntry;
extern const ModEntry g_modtab[];           /* gen/modtab.c */
extern const int g_modtab_count;

/* Start-up (main_wc3.c): map the exe and, depth first, every lifted DLL it
 * imports, as the Windows loader does; then run the DLLs' DllMain
 * (PROCESS_ATTACH) in dependency order.  0 = ok. */
int      module_load_exe(const char *path);
int      module_init_static(struct CPU *c);
uint32_t module_exe_entry(void);

/* Module handles are the base addresses (as on Windows). */
uint32_t module_load(struct CPU *c, const char *name, uint32_t *err);  /* LoadLibraryA */
int      module_free(struct CPU *c, uint32_t h);                       /* FreeLibrary; 0 = not a module */
int      module_handle(uint32_t h);                    /* h is a lifted module (mapped or not) */
uint32_t module_by_name(const char *name);             /* GetModuleHandleA: base if mapped, else 0 */
int      module_known_name(const char *name);          /* name is a lifted module, mapped or not */
uint32_t module_proc(uint32_t h, uint32_t name_or_ord, uint32_t *err);  /* GetProcAddress */
int      module_file_name(uint32_t h, char *out, int n);                /* GetModuleFileNameA */
int      module_image_at(uint32_t a, uint32_t *base, uint32_t *size);   /* a inside a mapped image */

/* The Win32 HLE's import table (win32.c): every module's imports in one index
 * space, g_modtab order; an IAT slot of a non-lifted DLL holds IMP_MAGIC +
 * its index. */
uint32_t       module_imp_total(void);
const ImpEnt  *module_imp(uint32_t idx);
int            module_of_import(uint32_t idx);         /* g_modtab index of the importing module */

/* dispatch.c / backtraces */
uint32_t module_nearest_fn(uint32_t addr, const char **mod);
uint32_t module_fn_from_host(const void *pc);
void     module_report(void);
#endif
