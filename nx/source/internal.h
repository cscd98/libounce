#pragma once
#include "types.h"
#include "arm/tls.h"
#include "kernel/thread.h"

#define THREADVARS_MAGIC 0x21545624 // !TV$

// This structure is exactly 0x20 bytes
typedef struct {
    // Magic value used to check if the struct is initialized
    u32 magic;

    // Thread handle, for mutexes
    Handle handle;

    // Pointer to the current thread (if exists)
    Thread* thread_ptr;

    // Pointer to this thread's newlib state
    struct _reent* reent;

    // Pointer to this thread's thread-local segment
    void* tls_tp; // !! Offset needs to be TLS+0x1F8 for __aarch64_read_tp !!
} ThreadVars;

static inline ThreadVars* getThreadVars(void) {
    return (ThreadVars*)((u8*)armGetTls() + 0x200 - sizeof(ThreadVars));
}

// Every thread has one area for its thread-local variables, getTlsStartOffset()
// bytes after its thread pointer. In a statically linked program the area is
// the program's TLS segment. In libounce.so it also holds the segments of the
// shared libraries, and the runtime linker lays it out (switch2/rtld/rtld.c).
#ifdef LIBOUNCE_SHARED

size_t getTlsStartOffset(void);
size_t getTlsAreaSize(void);
size_t getTlsAreaAlign(void);
void initializeTlsArea(void* area);

// Calls a function with the TLS area of each running thread that
// threadCreate made (kernel/thread.c). The main thread is not one of them.
void _threadForEachTlsArea(void (*function)(void* area, void* argument), void* argument);

#else

extern const u8 __tdata_lma[];
extern const u8 __tdata_lma_end[];
extern u8 __tls_start[];
extern u8 __tls_end[];
extern size_t __tls_align;

NX_INLINE size_t getTlsStartOffset(void)
{
    // TLS region begins with the Thread Control Block (TCB), which is intended
    // to contain two pointers. The actual tdata/tbss segment follows the TCB,
    // however if it requires special alignment the offset is rounded up.
    size_t tcb_sz = 2*sizeof(void*);
    return __tls_align > tcb_sz ? __tls_align : tcb_sz;
}

NX_INLINE size_t getTlsAreaSize(void)
{
    return __tls_end - __tls_start;
}

NX_INLINE size_t getTlsAreaAlign(void)
{
    return getTlsStartOffset();
}

// Gives an area its initial values: the segment's initialized data, then zeros.
NX_INLINE void initializeTlsArea(void* area)
{
    size_t load_sz = __tdata_lma_end - __tdata_lma;
    __builtin_memcpy(area, __tdata_lma, load_sz);
    __builtin_memset((u8*)area + load_sz, 0, getTlsAreaSize() - load_sz);
}

#endif
