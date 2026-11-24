/* SPDX-License-Identifier: ISC
 * Original libounce-nx runtime linker of libounce.so (Switch 2).
 *
 * The loader maps a dynamically linked program and every shared library it
 * needs, and enters libounce.so (rtld_crt0.s) with a DynlinkLoadInfo in its
 * configuration entries. The entry code calls these functions in order:
 *
 *   _rtldRelocateSelf  applies libounce.so's own base relocations.
 *   _rtldLinkProgram   links all modules with the dynamic linker core
 *                      (switch2/runtime/dynlink.c).
 *   __libnx_init       initializes the library; it calls _rtldSetupMainTls
 *                      and _rtldRunConstructors.
 *   _rtldRunProgram    calls the program's main.
 *
 * While the program runs, dlopen and dlclose (dlfcn.c) add and remove
 * libraries.
 *
 * libounce.so is linked so that its own references bind to itself. The
 * exceptions are the symbols that a program can replace (__libnx_initheap,
 * __appInit, __nx_heap_size and the like) and the ones only a program has
 * (main): the code reaches those through the GOT, which is not filled in
 * until _rtldLinkProgram returns.
 *
 * A C++ exception can pass from one module to another. libgcc's unwinder,
 * which each module that needs it links statically, asks _Unwind_Find_FDE
 * for the unwind entry of every return address. libgcc's own answers from a
 * list that a statically linked program's crtbegin.o registers its entries
 * in. The one here takes its place for all modules and answers from the
 * module that contains the address.
 */
#include <stdlib.h>
#include <string.h>
#include "result.h"
#include "kernel/svc.h"
#include "runtime/env.h"
#include "runtime/diag.h"
#include "switch2/runtime/dynlink.h"
#include "../../internal.h"
#include "rtld.h"

#define RTLD_HIDDEN __attribute__((visibility("hidden")))

// DT_RELR and its size, for a toolchain whose elf.h has no names for them.
#ifndef DT_RELR
#define DT_RELRSZ 35
#define DT_RELR   36
#endif

extern char* fake_heap_start;
extern char* fake_heap_end;
extern int __system_argc;
extern char** __system_argv;
int main(int argc, char** argv);

DynlinkProgram g_rtldProgram;
RMutex g_rtldLock;
const char* const* g_rtldLibraryDirectories;
u32 g_rtldLibraryDirectoryCount;
void* g_rtldMainTls;
size_t g_rtldTlsUsed;

// Applies libounce.so's relocations that need only its own address. The code
// reads no global variable: the GOT is among the words that it fills in.
RTLD_HIDDEN void _rtldRelocateSelf(uintptr_t base, const Elf64_Dyn* dynamic) {
    const Elf64_Rela* rela = NULL;
    const Elf64_Relr* relr = NULL;
    size_t rela_count = 0;
    size_t relr_count = 0;
    for (const Elf64_Dyn* entry = dynamic; entry->d_tag != DT_NULL; entry++) {
        if (entry->d_tag == DT_RELA)
            rela = (const Elf64_Rela*)(base + entry->d_un.d_ptr);
        else if (entry->d_tag == DT_RELASZ)
            rela_count = entry->d_un.d_val / sizeof(Elf64_Rela);
        else if (entry->d_tag == DT_RELR)
            relr = (const Elf64_Relr*)(base + entry->d_un.d_ptr);
        else if (entry->d_tag == DT_RELRSZ)
            relr_count = entry->d_un.d_val / sizeof(Elf64_Relr);
    }

    for (; rela_count--; rela++) {
        if (ELF64_R_TYPE(rela->r_info) == R_AARCH64_RELATIVE)
            *(u64*)(base + rela->r_offset) = base + rela->r_addend;
    }

    u64* where = NULL;
    for (; relr_count--; relr++) {
        if (!(*relr & 1)) {
            where = (u64*)(base + *relr);
            *where++ += base;
            continue;
        }
        for (u64 bitmap = *relr >> 1, bit = 0; bitmap; bitmap >>= 1, bit++) {
            if (bitmap & 1)
                where[bit] += base;
        }
        where += 63;
    }
}

// Writes the reason of a failure for the loader: a text, then a symbol's name.
static void _rtldWriteMessage(const DynlinkLoadInfo* info, const char* text, const char* name) {
    if (!info || !info->message || !info->message_size)
        return;

    size_t length = 0;
    for (; *text && length + 1 < info->message_size; text++)
        info->message[length++] = *text;
    for (; name && *name && length + 1 < info->message_size; name++)
        info->message[length++] = *name;
    info->message[length] = '\0';
}

// Makes a module's relocated constant data read-only, as a statically linked
// program's startup code does for its own (source/runtime/dynamic.c).
Result _rtldProtectModule(const DynlinkModule* m) {
    if (!m->relro || !m->relro_size)
        return 0;

    Result rc = svcSetMemoryPermission(m->relro, m->relro_size, Perm_R);
    if (R_SUCCEEDED(rc))
        svcSetMemoryAttribute(m->relro, m->relro_size, MemAttr_IsPermissionLocked, MemAttr_IsPermissionLocked);
    return rc;
}

// Links the modules that the loader mapped. On failure the result goes back
// to the loader, with the reason in the loader's message buffer.
RTLD_HIDDEN Result _rtldLinkProgram(const ConfigEntry* entries, uintptr_t own_base) {
    const DynlinkLoadInfo* info = NULL;
    for (const ConfigEntry* entry = entries; entry->Key != EntryType_EndOfList; entry++) {
        if (entry->Key == DYNLINK_CONFIG_ENTRY_KEY)
            info = (const DynlinkLoadInfo*)entry->Value[0];
    }
    if (!info || !info->module_count) {
        _rtldWriteMessage(info, "the loader gave no usable module list", NULL);
        return DYNLINK_RESULT_NO_RUNTIME_LIBRARY;
    }

    dynlinkProgramCreate(&g_rtldProgram);
    for (u32 i = 0; i < info->module_count; i++) {
        DynlinkModule* m = NULL;
        const Result rc = dynlinkModuleOpenInImage((void*)info->modules[i].base, info->modules[i].size, &m);
        if (R_FAILED(rc)) {
            _rtldWriteMessage(info, "a module is not a dynamically linked NRO that was built for this libounce", NULL);
            return DYNLINK_RESULT_LIBRARY_NOT_LOADED;
        }
        dynlinkProgramAddModule(&g_rtldProgram, m, true);
        m->is_self_relocated = m->base == own_base;
    }

    g_rtldLibraryDirectories = info->library_directories;
    g_rtldLibraryDirectoryCount = info->library_directory_count;

    dynlinkProgramLayoutTls(&g_rtldProgram);
    g_rtldTlsUsed = g_rtldProgram.tls_size;
    g_rtldProgram.tls_size += RTLD_TLS_SPARE;
    const char* symbol = NULL;
    Result rc = dynlinkProgramRelocate(&g_rtldProgram, &symbol);
    if (R_FAILED(rc)) {
        if (symbol) {
            _rtldWriteMessage(info, "undefined symbol: ", symbol);
            return DYNLINK_RESULT_UNDEFINED_SYMBOL;
        }
        _rtldWriteMessage(info, "a module has a relocation that is not supported; "
                                "compile shared libraries with -ftls-model=initial-exec", NULL);
        return DYNLINK_RESULT_BAD_RELOCATION;
    }

    for (const DynlinkModule* m = g_rtldProgram.first; m && R_SUCCEEDED(rc); m = m->next)
        rc = _rtldProtectModule(m);
    if (R_FAILED(rc))
        _rtldWriteMessage(info, "a module's read-only data could not be protected", NULL);
    return rc;
}

size_t getTlsStartOffset(void) {
    return g_rtldProgram.tls_start_offset;
}

size_t getTlsAreaSize(void) {
    return g_rtldProgram.tls_size;
}

size_t getTlsAreaAlign(void) {
    return g_rtldProgram.tls_align;
}

void initializeTlsArea(void* area) {
    rmutexLock(&g_rtldLock);
    memset(area, 0, g_rtldProgram.tls_size);
    for (const DynlinkModule* m = g_rtldProgram.first; m; m = m->next) {
        // Not a library whose blocks are allocated apart (tls_dynamic.c).
        if (m->is_tls_static && m->tls_template_size)
            memcpy((u8*)area + m->tls_offset, m->tls_template, m->tls_template_size);
    }
    rmutexUnlock(&g_rtldLock);
}

// Gives the main thread its TLS area, from the start of the heap, as
// argvSetup takes the arguments' storage. A statically linked program has the
// area in its image; here its size is known only after linking, and the
// allocator itself has thread-local variables, so it cannot provide it.
void _rtldSetupMainTls(void) {
    const size_t align_mask = g_rtldProgram.tls_align - 1;
    const size_t size = (g_rtldProgram.tls_size + 0xF) & ~(size_t)0xF;
    char* area = (char*)(((uintptr_t)fake_heap_start + align_mask) & ~(uintptr_t)align_mask);
    if (!fake_heap_start || area > fake_heap_end || (size_t)(fake_heap_end - area) < size)
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));

    fake_heap_start = area + size;
    g_rtldMainTls = area;
    initializeTlsArea(area);
    getThreadVars()->tls_tp = area - g_rtldProgram.tls_start_offset;
}

// What libgcc's unwinder keeps with an unwind entry (struct dwarf_eh_bases
// in its unwind-dw2-fde.h): the bases of text-relative and data-relative
// addresses, which AArch64 does not use, and the function's first address.
typedef struct {
    void* text_base;
    void* data_base;
    void* function;
} RtldUnwindBases;

const void* _Unwind_Find_FDE(void* address, RtldUnwindBases* bases) {
    uintptr_t function = 0;
    rmutexLock(&g_rtldLock);
    const void* entry = dynlinkProgramFindUnwindEntry(&g_rtldProgram, (uintptr_t)address, &function);
    rmutexUnlock(&g_rtldLock);
    if (entry) {
        bases->text_base = NULL;
        bases->data_base = NULL;
        bases->function = (void*)function;
    }
    return entry;
}

static void _rtldRunDestructors(void) {
    rmutexLock(&g_rtldLock);
    dynlinkProgramRunDestructors(&g_rtldProgram);
    rmutexUnlock(&g_rtldLock);
}

// Runs every module's constructors, and its destructors when the program exits.
void _rtldRunConstructors(void) {
    dynlinkProgramRunConstructors(&g_rtldProgram);
    atexit(_rtldRunDestructors);
}

RTLD_HIDDEN void NX_NORETURN _rtldRunProgram(void) {
    exit(main(__system_argc, __system_argv));
}
