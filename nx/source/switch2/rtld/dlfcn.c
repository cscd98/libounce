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
 * A library is local unless RTLD_GLOBAL asks otherwise: its references are
 * looked up in the global scope and then in the library and what it needs,
 * and no other library sees its symbols. Two plugins that define the same
 * names therefore each use their own.
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
#define DL_PATH_SIZE            0x301

void __cxa_finalize(void* dso_handle);

static __thread char g_dlError[192];
static __thread bool g_dlHasError;
static bool g_dlRoInitialized;

static void _dlSetError(const char* text, const char* name) {
    snprintf(g_dlError, sizeof(g_dlError), "%s%s%s", text, name ? ": " : "", name ? name : "");
    g_dlHasError = true;
}

static bool _dlIsHandle(const void* handle) {
    for (const DynlinkModule* m = g_rtldProgram.first; m; m = m->next) {
        if (handle == m)
            return true;
    }
    return false;
}

// Opens a library's file: a path as it is, a file name in each of the
// directories where the loader looked for the program's libraries.
static FILE* _dlOpenFile(const char* name) {
    if (strchr(name, '/'))
        return fopen(name, "rb");

    for (u32 i = 0; i < g_rtldLibraryDirectoryCount; i++) {
        char path[DL_PATH_SIZE];
        const int length = snprintf(path, sizeof(path), "%s/%s", g_rtldLibraryDirectories[i], name);
        FILE* file = length > 0 && (size_t)length < sizeof(path) ? fopen(path, "rb") : NULL;
        if (file)
            return file;
    }
    return NULL;
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
static DynlinkModule* _dlLoad(const char* name, const char* soname) {
    if (!g_dlRoInitialized) {
        if (R_FAILED(ldrRoInitialize())) {
            _dlSetError("the loader gave no session to load shared libraries with", NULL);
            return NULL;
        }
        g_dlRoInitialized = true;
    }

    RtldLibrary* library = calloc(1, sizeof(*library));
    NroHeader header;
    u8* memory = library ? _dlReadFile(name, &header) : NULL;
    if (!memory) {
        if (!library)
            _dlSetError("no memory to load the shared library", name);
        free(library);
        return NULL;
    }

    u64 base = 0;
    Result rc = ldrRoLoadNro(&base, (u64)(uintptr_t)memory, header.size, (u64)(uintptr_t)memory + header.size,
                             header.bss_size);
    if (R_FAILED(rc)) {
        free(memory);
        free(library);
        _dlSetError("the shared library could not be mapped", name);
        return NULL;
    }

    // The module's record is in its own image.
    DynlinkModule* m = NULL;
    rc = dynlinkModuleOpenInImage((void*)(uintptr_t)base, (size_t)header.size + header.bss_size, &m);
    const bool is_named = R_SUCCEEDED(rc) && m->soname && !strcmp(m->soname, soname);
    if (!is_named) {
        ldrRoUnloadNro(base);
        free(memory);
        free(library);
        _dlSetError("the file is not a shared library with this DT_SONAME that was built for this libounce", name);
        return NULL;
    }

    library->memory = memory;
    library->references = 1;
    m->owner_data = library;
    dynlinkProgramAddModule(&g_rtldProgram, m, false);
    return m;
}

// Runs a library's destructors and unloads it.
static void _dlUnload(DynlinkModule* m) {
    RtldLibrary* library = m->owner_data;

    // What the library registered with __cxa_atexit, such as the destructors
    // of its static C++ objects, then its destructor functions.
    if (m->is_initialized) {
        if (m->dso_handle)
            __cxa_finalize(m->dso_handle);
        dynlinkProgramRunModuleDestructors(&g_rtldProgram, m);
    }

    // The room of its thread-local variables is free again when it is the
    // last in the area.
    if (m->is_tls_static && m->tls_size && m->tls_offset + m->tls_size == g_rtldTlsUsed)
        g_rtldTlsUsed = m->tls_offset;

    // The module's record goes with its image.
    dynlinkProgramRemoveModule(&g_rtldProgram, m);
    ldrRoUnloadNro(m->base);
    free(library->memory);
    free(library);
}

// Gives up one reference to a library that dlopen loaded. A library with no
// reference left joins the chain of libraries to unload.
static void _dlDropReference(DynlinkModule* m, DynlinkModule** chain) {
    RtldLibrary* library = m->owner_data;
    if (!library || --library->references)
        return;
    library->release_next = *chain;
    *chain = m;
}

// Gives up one reference to a library. Without references, the library is
// unloaded after it gives up its references to the libraries it needs, which
// can leave those without references too.
static void _dlRelease(DynlinkModule* m) {
    DynlinkModule* chain = NULL;
    _dlDropReference(m, &chain);
    while (chain) {
        DynlinkModule* current = chain;
        RtldLibrary* library = current->owner_data;
        chain = library->release_next;

        for (u32 i = 0; i < library->needed_acquired; i++) {
            DynlinkModule* needed = dynlinkProgramFindModule(&g_rtldProgram, dynlinkModuleGetNeeded(current, i));
            if (needed)
                _dlDropReference(needed, &chain);
        }
        _dlUnload(current);
    }
}

// Takes a reference to a library, loading it and the libraries it needs when
// it is not loaded. On failure the caller releases the library it asked for,
// which gives up what was taken up to the failure.
static DynlinkModule* _dlAcquire(const char* name, bool* out_failed) {
    const char* file_name = strrchr(name, '/');
    const char* soname = file_name ? file_name + 1 : name;

    DynlinkModule* m = dynlinkProgramFindModule(&g_rtldProgram, soname);
    if (m) {
        RtldLibrary* library = m->owner_data;
        if (library)
            library->references++;
        return m;
    }

    m = _dlLoad(name, soname);
    if (!m) {
        *out_failed = true;
        return NULL;
    }
    RtldLibrary* library = m->owner_data;
    for (u32 i = 0; i < m->needed_count && !*out_failed; i++) {
        if (_dlAcquire(dynlinkModuleGetNeeded(m, i), out_failed))
            library->needed_acquired++;
    }
    return m;
}

// Gives one thread's area the initial values of the thread-local variables
// of the new modules that have their block there. The new modules are the
// last of the program's list, from the argument on.
static void _dlInitializeTls(void* area, void* argument) {
    for (const DynlinkModule* m = argument; m; m = m->next) {
        if (!m->is_tls_static)
            continue;
        memset((u8*)area + m->tls_offset, 0, m->tls_size);
        memcpy((u8*)area + m->tls_offset, m->tls_template, m->tls_template_size);
    }
}

// Links the modules that _dlAcquire loaded: places their thread-local
// variables, relocates them, and runs their constructors.
static bool _dlLink(void) {
    // Modules join the end of the list, so the new ones are the first that
    // is not relocated and all after it.
    DynlinkModule* first_new = g_rtldProgram.first;
    while (first_new && first_new->is_relocated)
        first_new = first_new->next;

    for (DynlinkModule* m = first_new; m; m = m->next) {
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
    for (const DynlinkModule* m = first_new; m; m = m->next) {
        if (R_FAILED(_rtldProtectModule(m))) {
            _dlSetError("read-only data could not be protected in", m->soname);
            return false;
        }
    }

    _dlInitializeTls(g_rtldMainTls, first_new);
    _threadForEachTlsArea(_dlInitializeTls, first_new);
    dynlinkProgramRunConstructors(&g_rtldProgram);
    return true;
}

void* dlopen(const char* name, int flags) {
    rmutexLock(&g_rtldLock);

    DynlinkModule* m;
    if (!name) {
        m = g_rtldProgram.first;
    } else {
        bool failed = false;
        m = _dlAcquire(name, &failed);
        // A library that joins the global scope takes the libraries it needs
        // with it, also when they were loaded as local before.
        if (m && !failed && (flags & RTLD_GLOBAL))
            dynlinkProgramMakeGlobal(&g_rtldProgram, m);
        if (m && (failed || !_dlLink())) {
            _dlRelease(m);
            m = NULL;
        }
    }

    rmutexUnlock(&g_rtldLock);
    return m;
}

void* dlsym(void* handle, const char* name) {
    rmutexLock(&g_rtldLock);

    const DynlinkModule* owner = NULL;
    const Elf64_Sym* symbol = NULL;
    // The program's handle is the global scope too.
    if (handle == RTLD_DEFAULT || handle == g_rtldProgram.first)
        symbol = dynlinkProgramFindSymbol(&g_rtldProgram, name, &owner);
    else if (_dlIsHandle(handle))
        symbol = dynlinkProgramFindSymbolFrom(&g_rtldProgram, handle, name, &owner);

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
    DynlinkModule* m = g_rtldProgram.first;
    while (m) {
        DynlinkModule* next = m->next;
        if (m->owner_data) {
            dynlinkProgramRemoveModule(&g_rtldProgram, m);
            ldrRoUnloadNro(m->base);
        }
        m = next;
    }
    if (g_dlRoInitialized) {
        ldrRoExit();
        g_dlRoInitialized = false;
    }
    rmutexUnlock(&g_rtldLock);
}
