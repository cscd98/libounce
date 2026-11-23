// SPDX-License-Identifier: ISC
// Original libounce-nx start of libounce.so's image (Switch 2).
//
// libounce.so is the runtime library of a dynamically linked program. The
// loader maps the program and the shared libraries it needs, and enters this
// image in place of the program, with an NRO's arguments. The code below
// relocates libounce.so, links every module (rtld.c), initializes the
// library as a statically linked program's startup code does
// (source/runtime/switch_crt0.s), and calls the program's main.
.section .crt0, "ax", %progbits
.global __ounce_rtld_start
.hidden __ounce_rtld_start
.align 2

__ounce_rtld_start:
    b 1f
    .word __nx_mod0 - __ounce_rtld_start
    .ascii "OUNCERTL"

.org __ounce_rtld_start+0x80; 1:
    // Arguments on entry from the loader (homebrew ABI):
    //   x0=ptr to env context    | x1=UINT64_MAX
    // Arguments on user-mode exception entry, which the loader passes on:
    //   x0=excpt type (non-zero) | x1=ptr to excpt context
    cmp  x0, #0
    ccmn x1, #1, #4, ne // 4 = Z
    beq  .Lrtld_main_entry
    b    __libnx_exception_entry

.Lrtld_main_entry:
    // Preserve registers across function calls
    mov x25, x0  // entrypoint argument 0
    mov x26, x1  // entrypoint argument 1
    mov x27, x30 // loader return address
    mov x28, sp  // initial stack pointer

    // Relocate libounce.so itself. Until this returns, no code may read a
    // global variable through the GOT.
    adr  x0, __ounce_rtld_start
    adrp x1, _DYNAMIC
    add  x1, x1, #:lo12:_DYNAMIC
    bl   _rtldRelocateSelf

    // Save initial stack pointer
    adrp x9, __stack_top
    str  x28, [x9, #:lo12:__stack_top]

    // Link the program and its libraries. Until this returns, no code may
    // use a symbol that a program can replace, such as __libnx_init.
    mov  x0, x25
    adr  x1, __ounce_rtld_start
    bl   _rtldLinkProgram
    cbnz w0, .Lrtld_link_failed

    // Perform system initialization
    mov  x0, x25
    mov  x1, x26
    mov  x2, x27
    bl   __libnx_init

    // Call the program's main, and exit with its result
    bl   _rtldRunProgram

.Lrtld_link_failed:
    // Return the result to the loader
    mov  sp, x28
    br   x27

.global __nx_exit
.type   __nx_exit, %function
__nx_exit:
    // Restore stack pointer
    adrp x8, __stack_top
    ldr  x8, [x8, #:lo12:__stack_top]
    mov  sp, x8

    // Jump back to loader
    br   x1

// newlib's __libc_init_array and __libc_fini_array call these. A statically
// linked program has them from crti.o; no module has code in .init or .fini.
.global _init
.hidden _init
.type   _init, %function
_init:
    ret

.global _fini
.hidden _fini
.type   _fini, %function
_fini:
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
    .word  __ounce_module_record - __nx_mod0

    // The same blocks as a module's MOD0 (switch2/module/module_crt0.s).
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
    .word  __ounce_platform // NroPlatform, defined by nro_ounce.c
    .word  0x1

    .ascii "OTL0"
    .word  __tdata_lma          - __nx_mod0
    .word  __tdata_lma_end      - __nx_mod0

    .ascii "OTL1"
    .word  __tls_start          - __nx_mod0
    .word  __tls_end            - __nx_mod0

    .ascii "OTL2"
    .word  __tls_align          - __nx_mod0
    .word  0x0

    // The size of the space for libounce.so's own module record, which
    // MOD0's last word locates.
    .ascii "OMR0"
    .word  0x200
    .word  0x0

    // The runtime library's record: the entry that the loader enters, and
    // the version of the loader's DynlinkLoadInfo that it reads.
    .ascii "ORT0"
    .word  __ounce_rtld_start   - __nx_mod0
    .word  0x1

.section .bss.__stack_top, "aw", %nobits
.global __stack_top
.hidden __stack_top
.align 3

__stack_top:
    .space 8

// The space for libounce.so's own module record (DynlinkModule), as every
// module has it (switch2/module/module_crt0.s).
.section .bss.__ounce_module_record, "aw", %nobits
.global __ounce_module_record
.hidden __ounce_module_record
.align 4
__ounce_module_record:
    .space 0x200
