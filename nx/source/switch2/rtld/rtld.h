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

// A module and what the runtime linker keeps for it.
typedef struct {
    DynlinkModule module;   // First: a DynlinkModule of the program is one of these.
    void* memory;           // The heap pages under a library that dlopen loaded. NULL for a module that the loader mapped.
    u32 references;         // The handles and loaded libraries that need a library that dlopen loaded.
    u32 needed_acquired;    // How many of its DT_NEEDED libraries it holds a reference to.
} RtldModule;

// A free entry has no base address.
extern RtldModule g_rtldModules[DYNLINK_MAX_MODULES];
extern DynlinkProgram g_rtldProgram;

// Held to read or change the modules after startup. An exception that passes
// through a constructor takes it again on the same thread.
extern RMutex g_rtldLock;

// The main thread's TLS area, and the end of the last block in every area.
extern void* g_rtldMainTls;
extern size_t g_rtldTlsUsed;

Result _rtldProtectModule(const DynlinkModule* m);
