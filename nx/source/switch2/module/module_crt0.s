// SPDX-License-Identifier: ISC
// Original libounce-nx start of a dynamically linked module's image (Switch 2).
//
// A module is a shared library or a dynamically linked program. It is an NRO,
// so it begins as every NRO does: one instruction, the offset of MOD0, and a
// name. A module does not start itself: the loader enters the runtime
// library, libounce.so, which reads each module's MOD0, relocates the module
// and runs its constructors (switch2/runtime/dynlink.h), and then calls the
// program's main.
//
// This object is linked first in every module, in place of the startup code
// of a statically linked program (source/runtime/switch_crt0.s).
.section .crt0, "ax", %progbits
.global __ounce_module_start
.hidden __ounce_module_start
.align 2

__ounce_module_start:
    b 1f
    .word __nx_mod0 - __ounce_module_start
    .ascii "OUNCEMOD"

// The NRO header takes the image's bytes 0x10 to 0x80.
.org __ounce_module_start+0x80; 1:
    // A loader that does not load shared libraries enters a program here, as
    // it enters a statically linked one. The module is not relocated and
    // cannot run: return DYNLINK_RESULT_NOT_LINKED (dynlink.h) to the loader.
    mov  w0, #0x915A
    movk w0, #0x1, lsl #16
    ret

// The thread pointer, as source/runtime/readtp.s. Each module has its own
// copy, so that reading it is a direct call and not one through the PLT.
.global __aarch64_read_tp
.hidden __aarch64_read_tp
.type   __aarch64_read_tp, %function
__aarch64_read_tp:
    mrs x0, tpidrro_el0
    ldr x0, [x0, #0x1F8]
    ret

.global __nx_mod0
.hidden __nx_mod0
__nx_mod0:
    .ascii "MOD0"
    .word  _DYNAMIC             - __nx_mod0
    .word  __bss_start__        - __nx_mod0
    .word  __bss_end__          - __nx_mod0
    .word  __eh_frame_hdr_start - __nx_mod0
    .word  __eh_frame_hdr_end   - __nx_mod0
    .word  0

    // The same blocks as a program's MOD0 (source/runtime/switch_crt0.s).
    .ascii "LNY0"
    .word  __got_start__        - __nx_mod0
    .word  __got_end__          - __nx_mod0

    .ascii "LNY1"
    .word  __relro_start        - __nx_mod0
    .word  __data_start         - __nx_mod0

    .ascii "LNY2"
    .word  0x1
    .word  0x0

    .ascii "OUN0"
    .word  __ounce_platform
    .word  0x1

    // Thread-local storage: the initial bytes, a block of the TLS size, and
    // the word that holds the alignment.
    .ascii "OTL0"
    .word  __tdata_lma          - __nx_mod0
    .word  __tdata_lma_end      - __nx_mod0

    .ascii "OTL1"
    .word  __tls_start          - __nx_mod0
    .word  __tls_end            - __nx_mod0

    .ascii "OTL2"
    .word  __tls_align          - __nx_mod0
    .word  0x0

    // The module's __dso_handle. When the module is unloaded, the runtime
    // library runs what the module registered with it (__cxa_atexit).
    .ascii "ODH0"
    .word  __dso_handle         - __nx_mod0
    .word  0x0

// The platform word of the "OUN0" block: 2 is Switch 2 (NroPlatform).
.global __ounce_platform
.hidden __ounce_platform
.set __ounce_platform, 2

// The module's identity for __cxa_atexit, which the compiler passes with each
// destructor of a static C++ object. A statically linked program has it from
// crtbegin.o.
.section .data.rel.ro.__dso_handle, "aw", %progbits
.global __dso_handle
.hidden __dso_handle
.align 3
__dso_handle:
    .quad __dso_handle
