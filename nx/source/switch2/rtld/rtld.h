/* SPDX-License-Identifier: ISC
 * Original libounce-nx state of libounce.so's runtime linker (Switch 2),
 * which rtld.c sets up at startup and dlfcn.c changes while the program runs.
 */
#pragma once
#include "kernel/mutex.h"
#include "switch2/runtime/dynlink.h"

// The room in each thread's TLS area, after the blocks of the modules that
// the loader mapped, for the thread-local variables of libraries with
// initial-exec code that dlopen loads. A library of the general-dynamic
// model does not use it (tls_dynamic.c).
#define RTLD_TLS_SPARE 0x1000

// What the runtime linker keeps for a library that dlopen loaded, in the
// module's owner_data. A module that the loader mapped has none: it stays for
// the life of the program.
typedef struct {
    void* memory;                   // The heap pages under the library's image.
    u32 references;                 // The handles and loaded libraries that need it.
    u32 needed_acquired;            // How many of its DT_NEEDED libraries it holds a reference to.
    DynlinkModule* release_next;    // The next library with no reference left, while they are unloaded.
} RtldLibrary;

// The program's modules. Each module's record is in the module's own image.
extern DynlinkProgram g_rtldProgram;

// Held to read or change the modules after startup. An exception that passes
// through a constructor takes it again on the same thread.
extern RMutex g_rtldLock;

// The directories where a library is looked for by its name, from the loader.
extern const char* const* g_rtldLibraryDirectories;
extern u32 g_rtldLibraryDirectoryCount;

// The main thread's TLS area, and the end of the last block in every area.
extern void* g_rtldMainTls;
extern size_t g_rtldTlsUsed;

Result _rtldProtectModule(const DynlinkModule* m);
