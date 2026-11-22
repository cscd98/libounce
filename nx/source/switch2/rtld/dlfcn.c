/* SPDX-License-Identifier: ISC
 * Original libounce-nx dlopen, dlsym and dlclose of libounce.so (Switch 2).
 *
 * dlopen reads a library's NRO file into page-aligned heap memory and has
 * RO map it, as the loader does for the libraries a program names. RO takes
 * the heap pages for the mapping, so they stay allocated until the library
 * is unloaded. The library joins the program's modules (rtld.c), and the
 * dynamic linker core relocates it against them.
 *
 * A library that dlopen loaded has a reference for each handle and for each
 * loaded library that needs it; it is unloaded when the last one goes. The
 * modules that the loader mapped stay for the life of the program.
 *
 * A thread's TLS area has a fixed size. A library whose code reads its
 * thread-local variables at fixed offsets (the initial-exec model) gets its
 * block in the spare room at the area's end (RTLD_TLS_SPARE), and fails to
 * load when that is used up. A library of the general-dynamic model calls
 * __tls_get_addr (tls_dynamic.c), which allocates a thread's block when the
 * thread first uses it: such a library has no limit.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "result.h"
#include "nro.h"
#include "services/ro.h"
#include "../../internal.h"
#include "rtld.h"

#define PAGE_SIZE               0x1000
#define DL_LIBRARY_DIRECTORY    "sdmc:/kaihen/lib"
#define DL_PATH_SIZE            0x301

extern int __system_argc;
extern char** __system_argv;
void __cxa_finalize(void* dso_handle);

static __thread char g_dlError[192];
static __thread bool g_dlHasError;
static bool g_dlRoInitialized;

static void _dlSetError(const char* text, const char* name) {
    snprintf(g_dlError, sizeof(g_dlError), "%s%s%s", text, name ? ": " : "", name ? name : "");
    g_dlHasError = true;
}

static bool _dlIsHandle(const void* handle) {
    for (u32 i = 0; i < DYNLINK_MAX_MODULES; i++) {
        if (handle == &g_rtldModules[i] && g_rtldModules[i].module.base)
            return true;
    }
    return false;
}

// Opens a library's file: a path as it is, a file name in the program's
// directory and then in the library directory.
static FILE* _dlOpenFile(const char* name) {
    if (strchr(name, '/'))
        return fopen(name, "rb");

    char path[DL_PATH_SIZE];
    const char* program = __system_argc > 0 ? __system_argv[0] : NULL;
    const char* program_name = program ? strrchr(program, '/') : NULL;
    if (program_name) {
        const int length = snprintf(path, sizeof(path), "%.*s/%s", (int)(program_name - program), program, name);
        FILE* file = length > 0 && (size_t)length < sizeof(path) ? fopen(path, "rb") : NULL;
        if (file)
            return file;
    }

    const int length = snprintf(path, sizeof(path), "%s/%s", DL_LIBRARY_DIRECTORY, name);
    return length > 0 && (size_t)length < sizeof(path) ? fopen(path, "rb") : NULL;
}

// Reads a library's file into new page-aligned memory, with its BSS after it.
static u8* _dlReadFile(const char* name, NroHeader* out_header) {
    FILE* file = _dlOpenFile(name);
    if (!file) {
        _dlSetError("shared library not found", name);
        return NULL;
    }

    u8 start[sizeof(NroStart) + sizeof(NroHeader)];
    NroHeader header;
    u8* memory = NULL;
    bool is_read = fread(start, 1, sizeof(start), file) == sizeof(start);
    if (is_read) {
        memcpy(&header, start + sizeof(NroStart), sizeof(header));
        is_read = header.magic == NROHEADER_MAGIC && header.size >= sizeof(start) &&
                  !((header.size | header.bss_size) & (PAGE_SIZE - 1));
    }
    if (is_read) {
        memory = aligned_alloc(PAGE_SIZE, (size_t)header.size + header.bss_size);
        is_read = memory && !fseek(file, 0, SEEK_SET) && fread(memory, 1, header.size, file) == header.size;
    }
    fclose(file);

    if (!is_read) {
        free(memory);
        _dlSetError("not a shared library, or it could not be read", name);
        return NULL;
    }
    memset(memory + header.size, 0, header.bss_size);
    *out_header = header;
    return memory;
}

// Loads a library that is not loaded yet, and adds it to the program with
// one reference. It is not relocated.
static RtldModule* _dlLoad(const char* name, const char* soname) {
    RtldModule* library = NULL;
    for (u32 i = 0; i < DYNLINK_MAX_MODULES && !library; i++) {
        if (!g_rtldModules[i].module.base)
            library = &g_rtldModules[i];
    }
    if (!library || g_rtldProgram.module_count == DYNLINK_MAX_MODULES) {
        _dlSetError("too many shared libraries are loaded", name);
        return NULL;
    }
    if (!g_dlRoInitialized) {
        if (R_FAILED(ldrRoInitialize())) {
            _dlSetError("the loader gave no session to load shared libraries with", NULL);
            return NULL;
        }
        g_dlRoInitialized = true;
    }

    NroHeader header;
    u8* memory = _dlReadFile(name, &header);
    if (!memory)
        return NULL;

    u64 base = 0;
    Result rc = ldrRoLoadNro(&base, (u64)(uintptr_t)memory, header.size, (u64)(uintptr_t)memory + header.size,
                             header.bss_size);
    if (R_FAILED(rc)) {
        free(memory);
        _dlSetError("the shared library could not be mapped", name);
        return NULL;
    }

    rc = dynlinkModuleOpen(&library->module, (const void*)(uintptr_t)base, (size_t)header.size + header.bss_size);
    const bool is_named = R_SUCCEEDED(rc) && library->module.soname && !strcmp(library->module.soname, soname);
    if (!is_named) {
        ldrRoUnloadNro(base);
        free(memory);
        memset(library, 0, sizeof(*library));
        _dlSetError("the file is not a shared library with this DT_SONAME", name);
        return NULL;
    }

    library->memory = memory;
    library->references = 1;
    dynlinkProgramAddModule(&g_rtldProgram, &library->module);
    return library;
}

// Gives up one reference to a library that dlopen loaded. Without
// references, the library's destructors run, it is unloaded, and it gives up
// its references to the libraries it needs.
static void _dlRelease(RtldModule* library) {
    if (!library->memory || --library->references)
        return;

    // The names of the needed libraries are in the image that is unmapped below.
    RtldModule* needed[DYNLINK_MAX_NEEDED];
    for (u32 i = 0; i < library->needed_acquired; i++)
        needed[i] = (RtldModule*)dynlinkProgramFindModule(&g_rtldProgram, library->module.needed[i]);
    const u32 needed_count = library->needed_acquired;

    // What the library registered with __cxa_atexit, such as the destructors
    // of its static C++ objects, then its destructor functions.
    if (library->module.is_initialized) {
        if (library->module.dso_handle)
            __cxa_finalize(library->module.dso_handle);
        dynlinkProgramRunModuleDestructors(&g_rtldProgram, &library->module);
    }

    // The room of its thread-local variables is free again when it is the
    // last in the area.
    if (library->module.is_tls_static && library->module.tls_size &&
        library->module.tls_offset + library->module.tls_size == g_rtldTlsUsed)
        g_rtldTlsUsed = library->module.tls_offset;

    dynlinkProgramRemoveModule(&g_rtldProgram, &library->module);
    ldrRoUnloadNro(library->module.base);
    free(library->memory);
    memset(library, 0, sizeof(*library));

    for (u32 i = 0; i < needed_count; i++) {
        if (needed[i])
            _dlRelease(needed[i]);
    }
}

// Takes a reference to a library, loading it and the libraries it needs when
// it is not loaded. On failure the caller releases the library it asked for,
// which gives up what was taken up to the failure.
static RtldModule* _dlAcquire(const char* name, bool* out_failed) {
    const char* file_name = strrchr(name, '/');
    const char* soname = file_name ? file_name + 1 : name;

    RtldModule* library = (RtldModule*)dynlinkProgramFindModule(&g_rtldProgram, soname);
    if (library) {
        if (library->memory)
            library->references++;
        return library;
    }

    library = _dlLoad(name, soname);
    if (!library) {
        *out_failed = true;
        return NULL;
    }
    for (u32 i = 0; i < library->module.needed_count && !*out_failed; i++) {
        if (_dlAcquire(library->module.needed[i], out_failed))
            library->needed_acquired++;
    }
    return library;
}

typedef struct {
    DynlinkModule* modules[DYNLINK_MAX_MODULES];
    u32 count;
} DlNewModules;

// Gives one thread's area the initial values of the thread-local variables
// of the new modules that have their block there.
static void _dlInitializeTls(void* area, void* argument) {
    const DlNewModules* added = argument;
    for (u32 i = 0; i < added->count; i++) {
        const DynlinkModule* m = added->modules[i];
        if (!m->is_tls_static)
            continue;
        memset((u8*)area + m->tls_offset, 0, m->tls_size);
        memcpy((u8*)area + m->tls_offset, m->tls_template, m->tls_template_size);
    }
}

// Links the modules that _dlAcquire loaded: places their thread-local
// variables, relocates them, and runs their constructors.
static bool _dlLink(void) {
    DlNewModules added = {0};
    for (u32 i = 0; i < g_rtldProgram.module_count; i++) {
        DynlinkModule* m = g_rtldProgram.modules[i];
        if (m->is_relocated)
            continue;
        added.modules[added.count++] = m;
        // Only initial-exec code needs the block in the TLS area.
        if (!m->tls_size || !m->needs_static_tls)
            continue;

        const size_t align = m->tls_align ? m->tls_align : 1;
        const size_t offset = (g_rtldTlsUsed + align - 1) & ~(align - 1);
        if (align > g_rtldProgram.tls_align || offset + m->tls_size > g_rtldProgram.tls_size) {
            _dlSetError("its thread-local variables do not fit; build it with "
                        "-ftls-model=global-dynamic -mtls-dialect=trad", m->soname);
            return false;
        }
        m->tls_offset = offset;
        m->is_tls_static = true;
        g_rtldTlsUsed = offset + m->tls_size;
    }

    const char* symbol = NULL;
    Result rc = dynlinkProgramRelocate(&g_rtldProgram, &symbol);
    if (R_FAILED(rc)) {
        if (symbol)
            _dlSetError("undefined symbol", symbol);
        else
            _dlSetError("a relocation is not supported; compile shared libraries with -ftls-model=initial-exec", NULL);
        return false;
    }
    for (u32 i = 0; i < added.count; i++) {
        if (R_FAILED(_rtldProtectModule(added.modules[i]))) {
            _dlSetError("read-only data could not be protected in", added.modules[i]->soname);
            return false;
        }
    }

    _dlInitializeTls(g_rtldMainTls, &added);
    _threadForEachTlsArea(_dlInitializeTls, &added);
    dynlinkProgramRunConstructors(&g_rtldProgram);
    return true;
}

void* dlopen(const char* name, int flags) {
    NX_IGNORE_ARG(flags);
    rmutexLock(&g_rtldLock);

    RtldModule* library;
    if (!name) {
        library = (RtldModule*)g_rtldProgram.modules[0];
    } else {
        bool failed = false;
        library = _dlAcquire(name, &failed);
        if (library && (failed || !_dlLink())) {
            _dlRelease(library);
            library = NULL;
        }
    }

    rmutexUnlock(&g_rtldLock);
    return library;
}

void* dlsym(void* handle, const char* name) {
    rmutexLock(&g_rtldLock);

    const DynlinkModule* owner = NULL;
    const Elf64_Sym* symbol = NULL;
    if (handle == RTLD_DEFAULT)
        symbol = dynlinkProgramFindSymbol(&g_rtldProgram, name, &owner);
    else if (_dlIsHandle(handle))
        symbol = dynlinkProgramFindSymbolFrom(&g_rtldProgram, &((RtldModule*)handle)->module, name, &owner);

    void* address = NULL;
    if (!symbol)
        _dlSetError("symbol not found", name);
    else if (ELF64_ST_TYPE(symbol->st_info) == STT_TLS)
        _dlSetError("the symbol is a thread-local variable, which has no one address", name);
    else if (symbol->st_shndx == SHN_ABS)
        address = (void*)(uintptr_t)symbol->st_value;
    else
        address = (void*)(owner->base + symbol->st_value);

    rmutexUnlock(&g_rtldLock);
    return address;
}

int dlclose(void* handle) {
    rmutexLock(&g_rtldLock);
    const bool is_handle = _dlIsHandle(handle);
    if (is_handle)
        _dlRelease(handle);
    else
        _dlSetError("not a handle from dlopen", NULL);
    rmutexUnlock(&g_rtldLock);
    return is_handle ? 0 : -1;
}

char* dlerror(void) {
    if (!g_dlHasError)
        return NULL;
    g_dlHasError = false;
    return g_dlError;
}

// Unmaps the libraries that the program left loaded, when it exits: the
// loader unloads only the images that it mapped. Their destructors have run
// with those of the other modules, and the heap goes with the program.
void _rtldUnmapLibraries(void) {
    rmutexLock(&g_rtldLock);
    for (u32 i = 0; i < DYNLINK_MAX_MODULES; i++) {
        RtldModule* library = &g_rtldModules[i];
        if (!library->memory)
            continue;
        dynlinkProgramRemoveModule(&g_rtldProgram, &library->module);
        ldrRoUnloadNro(library->module.base);
        memset(library, 0, sizeof(*library));
    }
    if (g_dlRoInitialized) {
        ldrRoExit();
        g_dlRoInitialized = false;
    }
    rmutexUnlock(&g_rtldLock);
}
