/* SPDX-License-Identifier: ISC
 * Original libounce-nx __tls_get_addr of libounce.so (Switch 2).
 *
 * Code of the general-dynamic TLS model in its traditional form
 * (-ftls-model=global-dynamic -mtls-dialect=trad) calls __tls_get_addr with
 * two words from its GOT: the tls_id of the module that has the variable,
 * and the variable's offset in that module's block. The block of a module
 * that was loaded with the program, or that dlopen placed in the spare room,
 * is in the thread's TLS area. The block of any other module is allocated
 * for the thread when the thread first uses it, so its size has no limit.
 *
 * Each thread has a table of the blocks it has used, in one of libnx's
 * dynamic TLS slots. The table and the allocated blocks are freed when the
 * thread exits. An allocated block of a library that was unloaded is freed
 * the next time its thread looks for a block that is not in its table.
 */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "result.h"
#include "kernel/thread.h"
#include "runtime/diag.h"
#include "../../internal.h"
#include "rtld.h"

typedef struct {
    u64 id;             // The module's tls_id; 0 for a free entry.
    u8* block;
    bool is_allocated;  // The block is the thread's own allocation, not in its TLS area.
} RtldTlsEntry;

typedef struct {
    RtldTlsEntry* entries;  // Grows when the thread uses more modules than it holds.
    u32 capacity;
} RtldThreadTls;

// The dynamic TLS slot of the tables, taken when the first thread needs one.
static _Atomic s32 g_rtldTlsSlot = -1;

// Frees a thread's table and blocks, when the thread exits.
static void _rtldTlsFreeThread(void* value) {
    RtldThreadTls* table = value;
    for (u32 i = 0; i < table->capacity; i++) {
        if (table->entries[i].is_allocated)
            free(table->entries[i].block);
    }
    free(table->entries);
    free(table);
}

static const DynlinkModule* _rtldTlsFindModule(u64 id) {
    for (const DynlinkModule* m = g_rtldProgram.first; m; m = m->next) {
        if (m->tls_id == id)
            return m;
    }
    return NULL;
}

// A new block with the initial values of the module's thread-local variables.
static u8* _rtldTlsAllocateBlock(const DynlinkModule* m) {
    const size_t align = m->tls_align > 16 ? m->tls_align : 16;
    const size_t size = (m->tls_size + align - 1) & ~(align - 1);
    u8* block = aligned_alloc(align, size ? size : align);
    if (!block)
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));

    memset(block, 0, size);
    memcpy(block, m->tls_template, m->tls_template_size);
    return block;
}

// Finds or makes the calling thread's block of a module, and records it in
// the thread's table.
static u8* _rtldTlsAddBlock(u64 id) {
    rmutexLock(&g_rtldLock);

    s32 slot = atomic_load(&g_rtldTlsSlot);
    if (slot < 0) {
        slot = threadTlsAlloc(_rtldTlsFreeThread);
        if (slot < 0)
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
        atomic_store(&g_rtldTlsSlot, slot);
    }
    RtldThreadTls* table = threadTlsGet(slot);
    if (!table) {
        table = calloc(1, sizeof(*table));
        if (!table)
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
        threadTlsSet(slot, table);
    }

    // The entries of libraries that were unloaded make room for new ones.
    RtldTlsEntry* entry = NULL;
    for (u32 i = 0; i < table->capacity; i++) {
        RtldTlsEntry* candidate = &table->entries[i];
        if (candidate->id && !_rtldTlsFindModule(candidate->id)) {
            if (candidate->is_allocated)
                free(candidate->block);
            *candidate = (RtldTlsEntry){0};
        }
        if (!candidate->id && !entry)
            entry = candidate;
    }

    if (!entry) {
        const u32 capacity = table->capacity ? table->capacity * 2 : 8;
        RtldTlsEntry* grown = realloc(table->entries, capacity * sizeof(RtldTlsEntry));
        if (!grown)
            diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_OutOfMemory));
        memset(grown + table->capacity, 0, (capacity - table->capacity) * sizeof(RtldTlsEntry));
        entry = grown + table->capacity;
        table->entries = grown;
        table->capacity = capacity;
    }

    // A call with the id of no loaded module is from code of a library that
    // was unloaded.
    const DynlinkModule* m = _rtldTlsFindModule(id);
    if (!m)
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_BadInput));

    entry->id = id;
    entry->is_allocated = !m->is_tls_static;
    if (m->is_tls_static)
        entry->block = (u8*)getThreadVars()->tls_tp + g_rtldProgram.tls_start_offset + m->tls_offset;
    else
        entry->block = _rtldTlsAllocateBlock(m);
    u8* block = entry->block;

    rmutexUnlock(&g_rtldLock);
    return block;
}

void* __tls_get_addr(const size_t* index) {
    const u64 id = index[0];
    const s32 slot = atomic_load(&g_rtldTlsSlot);
    const RtldThreadTls* table = slot >= 0 ? threadTlsGet(slot) : NULL;
    if (table) {
        for (u32 i = 0; i < table->capacity; i++) {
            if (table->entries[i].id == id)
                return table->entries[i].block + index[1];
        }
    }
    return _rtldTlsAddBlock(id) + index[1];
}
