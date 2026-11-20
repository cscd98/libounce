/**
 * @file dynlink.h
 * @brief Dynamic linking (Switch 2): the modules of a program and their relocation.
 * @author kurumuz
 * @copyright 2026 libounce contributors
 * @license ISC
 * @note A module is an NRO image with a MOD0 header and an ELF dynamic section: the
 *       program, or a shared library that it names with DT_NEEDED. A \ref DynlinkProgram
 *       holds the modules in lookup order: the program first, then its libraries
 *       breadth-first. These functions parse and relocate images that are already in
 *       memory. Loading the files and mapping them is the loader's work: it maps the
 *       program and every library it needs, and enters the runtime library (libounce.so)
 *       with a \ref DynlinkLoadInfo. The runtime library links the modules and calls main.
 * @note Thread-local variables of load-time modules use the initial-exec model
 *       (-ftls-model=initial-exec). TLS descriptors are refused: the compiler generates
 *       wrong code for them with -mtp=soft.
 */
#pragma once
#if !defined(__SWITCH2__)
#error "Dynamic linking is available only in a native Switch 2 build"
#endif

#include <elf.h>
#include "../../switch/types.h"
#include "../../switch/result.h"

/// Most modules of one program, with the program itself.
#define DYNLINK_MAX_MODULES 32

/// Most libraries that one module names with DT_NEEDED.
#define DYNLINK_MAX_NEEDED 16

/// Key of the loader's configuration entry for a dynamically linked program ("OMOD").
/// Value[0] is the address of a \ref DynlinkLoadInfo.
#define DYNLINK_CONFIG_ENTRY_KEY 0x4F4D4F44

/// What a dynamically linked program returns to a loader that entered it as a statically
/// linked one, without its runtime library.
#define DYNLINK_RESULT_NOT_LINKED MAKERESULT(Module_HomebrewAbi, 200)

/// An image that the loader mapped.
typedef struct DynlinkLoadedModule {
    u64 base;                                   ///< Address of the image's first byte.
    u64 size;                                   ///< Size of the image with its BSS.
} DynlinkLoadedModule;

/// What the loader gives the runtime library.
typedef struct DynlinkLoadInfo {
    const DynlinkLoadedModule* modules;         ///< The program first, then its libraries breadth-first.
    u32 module_count;                           ///< Their number.
    u32 message_size;                           ///< Size of the message buffer.
    char* message;                              ///< When linking fails, the runtime library writes the reason here.
} DynlinkLoadInfo;

/// One module: a parsed NRO image.
typedef struct DynlinkModule {
    uintptr_t base;                             ///< Address of the image's first byte.
    size_t size;                                ///< Size of the image with its BSS.
    const Elf64_Dyn* dynamic;                   ///< The dynamic section.
    const char* soname;                         ///< DT_SONAME, NULL for a program.
    const Elf64_Sym* symtab;                    ///< Dynamic symbol table.
    const char* strtab;                         ///< Dynamic string table.
    size_t strtab_size;                         ///< Its size.
    u32 symbol_count;                           ///< Number of dynamic symbols, from the SysV hash table.
    const u32* sysv_hash;                       ///< DT_HASH, or NULL.
    const u32* gnu_hash;                        ///< DT_GNU_HASH, or NULL.
    const Elf64_Rela* rela;                     ///< DT_RELA entries.
    size_t rela_count;                          ///< Their number.
    const Elf64_Rela* jmprel;                   ///< DT_JMPREL entries.
    size_t jmprel_count;                        ///< Their number.
    const Elf64_Relr* relr;                     ///< DT_RELR entries.
    size_t relr_count;                          ///< Their number.
    void (**init_array)(void);                  ///< Constructors.
    size_t init_count;                          ///< Their number.
    void (**fini_array)(void);                  ///< Destructors.
    size_t fini_count;                          ///< Their number.
    const char* needed[DYNLINK_MAX_NEEDED];     ///< DT_NEEDED names.
    u32 needed_count;                           ///< Their number.
    const void* tls_template;                   ///< Initial bytes of the module's thread-local data.
    size_t tls_template_size;                   ///< Their size.
    size_t tls_size;                            ///< Size of the module's thread-local block.
    size_t tls_align;                           ///< Its alignment.
    size_t tls_offset;                          ///< Offset of the block in a thread's TLS area, set by \ref dynlinkProgramLayoutTls.
    const void* eh_frame_hdr;                   ///< Index of the unwind entries (.eh_frame_hdr), or NULL.
    size_t eh_frame_hdr_size;                   ///< Its size.
    void* relro;                                ///< Data that is read-only after relocation, or NULL.
    size_t relro_size;                          ///< Its size.
    uintptr_t runtime_entry;                    ///< Entry of the runtime library; zero for every other module.
    void* dso_handle;                           ///< The module's __dso_handle, which its C++ code gives __cxa_atexit, or NULL.
    bool is_self_relocated;                     ///< The module already applied its own base relocations (RELATIVE and RELR).
    bool is_relocated;                          ///< \ref dynlinkProgramRelocate finished this module.
    bool is_initialized;                        ///< Its constructors have run.
} DynlinkModule;

/// The modules of one program, in lookup order.
typedef struct DynlinkProgram {
    DynlinkModule* modules[DYNLINK_MAX_MODULES]; ///< The program first, then libraries breadth-first.
    u32 module_count;                            ///< Their number.
    size_t tls_start_offset;                     ///< Distance from the thread pointer to the TLS area.
    size_t tls_size;                             ///< Size of the TLS area of all modules.
    size_t tls_align;                            ///< Its alignment.
    DynlinkModule* init_order[DYNLINK_MAX_MODULES]; ///< Private: the modules in the order their constructors ran.
    u32 init_order_count;                        ///< Private.
} DynlinkProgram;

/**
 * @brief Parses a module's MOD0 header and dynamic section.
 * @param[out] m Output module. It refers to the image; it does not copy from it.
 * @param[in] image The NRO image from its first byte: mapped, or a file image in memory.
 * @param[in] size Size of the image.
 * @return LibnxError_BadInput for an image without a MOD0 header or a usable dynamic section.
 */
Result dynlinkModuleOpen(DynlinkModule* m, const void* image, size_t size);

/**
 * @brief Finds a symbol that a module defines and exports.
 * @param[in] m Module.
 * @param[in] name Symbol name.
 * @return The symbol, or NULL.
 */
const Elf64_Sym* dynlinkModuleFindSymbol(const DynlinkModule* m, const char* name);

/// Starts an empty program.
void dynlinkProgramCreate(DynlinkProgram* p);

/**
 * @brief Adds a module at the end of the program's lookup order.
 * @param[in] p Program.
 * @param[in] m Module. The program keeps the pointer.
 * @return LibnxError_OutOfMemory when the program has \ref DYNLINK_MAX_MODULES modules.
 */
Result dynlinkProgramAddModule(DynlinkProgram* p, DynlinkModule* m);

/**
 * @brief Removes a module from the program.
 * @param[in] p Program.
 * @param[in] m Module, after \ref dynlinkProgramRunModuleDestructors. No other module may still use its symbols.
 */
void dynlinkProgramRemoveModule(DynlinkProgram* p, DynlinkModule* m);

/// Finds the program's module with a DT_SONAME, or NULL.
DynlinkModule* dynlinkProgramFindModule(const DynlinkProgram* p, const char* soname);

/**
 * @brief Finds the first definition of a symbol in the program's lookup order.
 * @param[in] p Program.
 * @param[in] name Symbol name.
 * @param[out] out_module The module that defines it. May be NULL.
 * @return The symbol, or NULL.
 */
const Elf64_Sym* dynlinkProgramFindSymbol(const DynlinkProgram* p, const char* name, const DynlinkModule** out_module);

/**
 * @brief Finds a symbol in a module, then in the libraries it needs, breadth-first.
 * @param[in] p Program.
 * @param[in] m Module of the program.
 * @param[in] name Symbol name.
 * @param[out] out_module The module that defines it. May be NULL.
 * @return The symbol, or NULL.
 */
const Elf64_Sym* dynlinkProgramFindSymbolFrom(const DynlinkProgram* p, const DynlinkModule* m, const char* name,
                                              const DynlinkModule** out_module);

/**
 * @brief Gives every module its place in a thread's TLS area.
 * @param[in] p Program, with all its modules added.
 * @note The program's block is first: its code uses offsets fixed when it was linked.
 */
void dynlinkProgramLayoutTls(DynlinkProgram* p);

/**
 * @brief Applies the relocations of every module that is not relocated yet.
 * @param[in] p Program, after \ref dynlinkProgramLayoutTls.
 * @param[out] out_symbol Name of the symbol that had no definition, on LibnxError_NotFound. May be NULL.
 * @return LibnxError_NotFound for an undefined symbol that is not weak, LibnxError_BadReloc for a
 *         relocation type that is not supported.
 */
Result dynlinkProgramRelocate(DynlinkProgram* p, const char** out_symbol);

/**
 * @brief Finds the unwind entry (FDE) of the function that contains an address.
 * @param[in] p Program.
 * @param[in] address Address in the code of one of the modules.
 * @param[out] out_function Address of the function's first instruction.
 * @return The entry in the module's .eh_frame section, or NULL when the address is in no module
 *         or in code without unwind information.
 * @note A module has the index that this reads when it is linked with --eh-frame-hdr.
 */
const void* dynlinkProgramFindUnwindEntry(const DynlinkProgram* p, uintptr_t address, uintptr_t* out_function);

/**
 * @brief Runs the constructors of every module, each after the libraries it needs.
 * @param[in] p Program, relocated.
 */
void dynlinkProgramRunConstructors(DynlinkProgram* p);

/// Runs the destructors of every module, in the reverse order of their constructors.
void dynlinkProgramRunDestructors(DynlinkProgram* p);

/// Runs the destructors of one module whose constructors ran.
void dynlinkProgramRunModuleDestructors(DynlinkProgram* p, DynlinkModule* m);
