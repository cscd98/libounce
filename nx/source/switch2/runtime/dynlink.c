/* SPDX-License-Identifier: ISC
 * Original libounce-nx Switch 2 dynamic linker core.
 *
 * A module is an NRO image. The word at +4 gives its MOD0 header, and MOD0
 * gives its ELF dynamic section. NROs have no program headers, so what an
 * ELF loader reads from them comes from the blocks after MOD0 instead:
 * libnx's "LNY1" for the data that is read-only after relocation, and
 * libounce's "OTL" blocks for thread-local storage (switch_crt0.s,
 * switch2/module/module_crt0.s). The runtime library has an "ORT0" block
 * with its entry (switch2/rtld/rtld_crt0.s).
 *
 * Addresses in the dynamic section and the symbol table are relative to the
 * image's first byte. A program's lookup order is its module order: the
 * first module that defines a name gives every module that name, so a
 * program's definition takes the place of a library's.
 *
 * Thread-local storage follows AArch64's variant 1 as libounce's threads lay
 * it out: the thread pointer, then tls_start_offset bytes (the larger of 16
 * and the program's TLS alignment), then the program's block, then each
 * library's block at its alignment. A TLS_TPREL relocation gets the distance
 * from the thread pointer to its variable.
 */
#include <string.h>
#include "switch2/runtime/dynlink.h"
#include "nro.h"

// MOD0: its magic and six words, then extension blocks of a magic and two
// words each. Offsets are relative to MOD0.
#define MOD0_OFFSET_WORD        4
#define MOD0_HEADER_SIZE        0x1C
#define MOD0_EXTENSION_SIZE     0xC
#define MOD0_EXTENSIONS_MAX     12
#define MOD0_MAGIC_LIBNX        0x00594E4C  // "LNY", then a digit.
#define MOD0_MAGIC_RELRO        0x31594E4C  // "LNY1": start and end of the data that is read-only after relocation.
#define MOD0_MAGIC_RUNTIME      0x3054524F  // "ORT0": the runtime library's entry.
#define MOD0_MAGIC_TLS_TEMPLATE 0x304C544F  // "OTL0": the initial TLS bytes' start and end.
#define MOD0_MAGIC_TLS_BLOCK    0x314C544F  // "OTL1": start and end of a block of the TLS size.
#define MOD0_MAGIC_TLS_ALIGN    0x324C544F  // "OTL2": the word that holds the TLS alignment.

#define TLS_TCB_SIZE            16

// DT_RELR and its size, for a toolchain whose elf.h has no names for them.
#ifndef DT_RELR
#define DT_RELRSZ 35
#define DT_RELR   36
#endif

typedef struct {
    u32 magic;
    s32 dynamic_offset;
    s32 bss_start_offset;
    s32 bss_end_offset;
    s32 eh_frame_hdr_start_offset;
    s32 eh_frame_hdr_end_offset;
    s32 module_object_offset;
} Mod0Header;

typedef struct {
    u32 magic;
    s32 first;
    s32 second;
} Mod0Extension;

static Result _dynlinkBadImage(void) {
    return MAKERESULT(Module_Libnx, LibnxError_BadInput);
}

// Whether [offset, offset + size) is inside an image of image_size bytes.
static bool _dynlinkIsInside(size_t image_size, u64 offset, u64 size) {
    return offset <= image_size && size <= image_size - offset;
}

// Reads the blocks after MOD0. Of the TLS block only its size is used: it is
// in the BSS, which a file image does not have.
static void _dynlinkReadBlocks(DynlinkModule* m, const u8* mod0, size_t mod0_offset, size_t image_size) {
    for (u32 i = 0; i < MOD0_EXTENSIONS_MAX; i++) {
        const size_t block_offset = MOD0_HEADER_SIZE + i * MOD0_EXTENSION_SIZE;
        if (!_dynlinkIsInside(image_size, mod0_offset + block_offset, MOD0_EXTENSION_SIZE))
            return;

        Mod0Extension block;
        memcpy(&block, mod0 + block_offset, sizeof(block));
        if (block.magic == MOD0_MAGIC_RELRO) {
            m->relro = (void*)(mod0 + block.first);
            m->relro_size = (size_t)(block.second - block.first);
        } else if (block.magic == MOD0_MAGIC_RUNTIME) {
            m->runtime_entry = (uintptr_t)(mod0 + block.first);
        } else if (block.magic == MOD0_MAGIC_TLS_TEMPLATE) {
            m->tls_template = mod0 + block.first;
            m->tls_template_size = (size_t)(block.second - block.first);
        } else if (block.magic == MOD0_MAGIC_TLS_BLOCK) {
            m->tls_size = (size_t)(block.second - block.first);
        } else if (block.magic == MOD0_MAGIC_TLS_ALIGN) {
            const u64 align_offset = mod0_offset + (s64)block.first;
            if (_dynlinkIsInside(image_size, align_offset, sizeof(u64))) {
                u64 align;
                memcpy(&align, mod0 + block.first, sizeof(align));
                m->tls_align = (size_t)align;
            }
        } else if ((block.magic & 0x00FFFFFF) != MOD0_MAGIC_LIBNX && block.magic != NROOUNCE_MAGIC) {
            // Code follows the last block.
            return;
        }
    }
}

// The number of symbols that a GNU hash table reaches: the highest chain
// index, then to the end of that chain.
static u32 _dynlinkGnuSymbolCount(const u32* table) {
    const u32 bucket_count = table[0];
    const u32 symbol_offset = table[1];
    const u32 bloom_size = table[2];
    const u32* buckets = table + 4 + bloom_size * 2;
    const u32* chains = buckets + bucket_count;

    u32 last = 0;
    for (u32 i = 0; i < bucket_count; i++) {
        if (buckets[i] > last)
            last = buckets[i];
    }
    if (last < symbol_offset)
        return symbol_offset;
    while (!(chains[last - symbol_offset] & 1))
        last++;
    return last + 1;
}

Result dynlinkModuleOpen(DynlinkModule* m, const void* image, size_t size) {
    memset(m, 0, sizeof(*m));
    const u8* bytes = (const u8*)image;
    if (!_dynlinkIsInside(size, MOD0_OFFSET_WORD, sizeof(u32)))
        return _dynlinkBadImage();

    u32 mod0_offset;
    memcpy(&mod0_offset, bytes + MOD0_OFFSET_WORD, sizeof(mod0_offset));
    if (!_dynlinkIsInside(size, mod0_offset, MOD0_HEADER_SIZE))
        return _dynlinkBadImage();

    const u8* mod0 = bytes + mod0_offset;
    Mod0Header header;
    memcpy(&header, mod0, sizeof(header));
    const u64 dynamic_offset = (u64)mod0_offset + (s64)header.dynamic_offset;
    if (header.magic != NROMOD0_MAGIC || !_dynlinkIsInside(size, dynamic_offset, sizeof(Elf64_Dyn)))
        return _dynlinkBadImage();

    m->base = (uintptr_t)image;
    m->dynamic = (const Elf64_Dyn*)(bytes + dynamic_offset);
    m->tls_align = 1;
    _dynlinkReadBlocks(m, mod0, mod0_offset, size);

    u64 soname_offset = 0;
    u64 needed_offsets[DYNLINK_MAX_NEEDED];
    bool has_soname = false;
    size_t rela_size = 0, jmprel_size = 0, relr_size = 0, init_size = 0, fini_size = 0;
    for (const Elf64_Dyn* entry = m->dynamic; entry->d_tag != DT_NULL; entry++) {
        if (!_dynlinkIsInside(size, (const u8*)(entry + 1) - bytes, sizeof(Elf64_Dyn)))
            return _dynlinkBadImage();

        const u64 value = entry->d_un.d_val;
        switch (entry->d_tag) {
            case DT_SONAME:       soname_offset = value; has_soname = true; break;
            case DT_NEEDED:
                if (m->needed_count == DYNLINK_MAX_NEEDED)
                    return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
                needed_offsets[m->needed_count++] = value;
                break;
            case DT_SYMTAB:       m->symtab = (const Elf64_Sym*)(bytes + value); break;
            case DT_STRTAB:       m->strtab = (const char*)(bytes + value); break;
            case DT_STRSZ:        m->strtab_size = value; break;
            case DT_HASH:         m->sysv_hash = (const u32*)(bytes + value); break;
            case DT_GNU_HASH:     m->gnu_hash = (const u32*)(bytes + value); break;
            case DT_RELA:         m->rela = (const Elf64_Rela*)(bytes + value); break;
            case DT_RELASZ:       rela_size = value; break;
            case DT_JMPREL:       m->jmprel = (const Elf64_Rela*)(bytes + value); break;
            case DT_PLTRELSZ:     jmprel_size = value; break;
            case DT_RELR:         m->relr = (const Elf64_Relr*)(bytes + value); break;
            case DT_RELRSZ:       relr_size = value; break;
            case DT_INIT_ARRAY:   m->init_array = (void (**)(void))(bytes + value); break;
            case DT_INIT_ARRAYSZ: init_size = value; break;
            case DT_FINI_ARRAY:   m->fini_array = (void (**)(void))(bytes + value); break;
            case DT_FINI_ARRAYSZ: fini_size = value; break;
            default: break;
        }
    }
    if (!m->symtab || !m->strtab || (!m->sysv_hash && !m->gnu_hash))
        return _dynlinkBadImage();

    m->rela_count = rela_size / sizeof(Elf64_Rela);
    m->jmprel_count = jmprel_size / sizeof(Elf64_Rela);
    m->relr_count = relr_size / sizeof(Elf64_Relr);
    m->init_count = init_size / sizeof(void*);
    m->fini_count = fini_size / sizeof(void*);
    // The SysV table's chain count is the symbol count.
    m->symbol_count = m->sysv_hash ? m->sysv_hash[1] : _dynlinkGnuSymbolCount(m->gnu_hash);

    if (has_soname) {
        if (soname_offset >= m->strtab_size)
            return _dynlinkBadImage();
        m->soname = m->strtab + soname_offset;
    }
    for (u32 i = 0; i < m->needed_count; i++) {
        if (needed_offsets[i] >= m->strtab_size)
            return _dynlinkBadImage();
        m->needed[i] = m->strtab + needed_offsets[i];
    }
    return 0;
}

static u32 _dynlinkSysvHash(const char* name) {
    u32 hash = 0;
    for (const u8* c = (const u8*)name; *c; c++) {
        hash = (hash << 4) + *c;
        const u32 high = hash & 0xF0000000;
        if (high)
            hash ^= high >> 24;
        hash &= ~high;
    }
    return hash;
}

static u32 _dynlinkGnuHash(const char* name) {
    u32 hash = 5381;
    for (const u8* c = (const u8*)name; *c; c++)
        hash = hash * 33 + *c;
    return hash;
}

// A definition that other modules can bind to.
static bool _dynlinkIsExported(const Elf64_Sym* symbol) {
    const u8 bind = ELF64_ST_BIND(symbol->st_info);
    const u8 type = ELF64_ST_TYPE(symbol->st_info);
    const u8 visibility = ELF64_ST_VISIBILITY(symbol->st_other);
    return symbol->st_shndx != SHN_UNDEF &&
           (bind == STB_GLOBAL || bind == STB_WEAK) &&
           type != STT_SECTION && type != STT_FILE &&
           (visibility == STV_DEFAULT || visibility == STV_PROTECTED);
}

static const Elf64_Sym* _dynlinkFindWithGnuHash(const DynlinkModule* m, const char* name) {
    const u32* table = m->gnu_hash;
    const u32 bucket_count = table[0];
    const u32 symbol_offset = table[1];
    const u32 bloom_size = table[2];
    const u32 bloom_shift = table[3];
    const u64* bloom = (const u64*)(table + 4);
    const u32* buckets = table + 4 + bloom_size * 2;
    const u32* chains = buckets + bucket_count;
    if (!bucket_count || !bloom_size)
        return NULL;

    const u32 hash = _dynlinkGnuHash(name);
    const u64 word = bloom[(hash / 64) % bloom_size];
    const u64 mask = (1ULL << (hash % 64)) | (1ULL << ((hash >> bloom_shift) % 64));
    if ((word & mask) != mask)
        return NULL;

    u32 index = buckets[hash % bucket_count];
    if (index < symbol_offset)
        return NULL;
    for (;; index++) {
        const u32 chain_hash = chains[index - symbol_offset];
        const Elf64_Sym* symbol = &m->symtab[index];
        if ((chain_hash | 1) == (hash | 1) && !strcmp(m->strtab + symbol->st_name, name) &&
            _dynlinkIsExported(symbol))
            return symbol;
        if (chain_hash & 1)
            return NULL;
    }
}

static const Elf64_Sym* _dynlinkFindWithSysvHash(const DynlinkModule* m, const char* name) {
    const u32 bucket_count = m->sysv_hash[0];
    const u32 chain_count = m->sysv_hash[1];
    const u32* buckets = m->sysv_hash + 2;
    const u32* chains = buckets + bucket_count;
    if (!bucket_count)
        return NULL;

    for (u32 index = buckets[_dynlinkSysvHash(name) % bucket_count];
         index != STN_UNDEF && index < chain_count; index = chains[index]) {
        const Elf64_Sym* symbol = &m->symtab[index];
        if (!strcmp(m->strtab + symbol->st_name, name) && _dynlinkIsExported(symbol))
            return symbol;
    }
    return NULL;
}

const Elf64_Sym* dynlinkModuleFindSymbol(const DynlinkModule* m, const char* name) {
    if (m->gnu_hash)
        return _dynlinkFindWithGnuHash(m, name);
    return _dynlinkFindWithSysvHash(m, name);
}

void dynlinkProgramCreate(DynlinkProgram* p) {
    memset(p, 0, sizeof(*p));
}

Result dynlinkProgramAddModule(DynlinkProgram* p, DynlinkModule* m) {
    if (p->module_count == DYNLINK_MAX_MODULES)
        return MAKERESULT(Module_Libnx, LibnxError_OutOfMemory);
    p->modules[p->module_count++] = m;
    return 0;
}

DynlinkModule* dynlinkProgramFindModule(const DynlinkProgram* p, const char* soname) {
    for (u32 i = 0; i < p->module_count; i++) {
        DynlinkModule* m = p->modules[i];
        if (m->soname && !strcmp(m->soname, soname))
            return m;
    }
    return NULL;
}

const Elf64_Sym* dynlinkProgramFindSymbol(const DynlinkProgram* p, const char* name, const DynlinkModule** out_module) {
    for (u32 i = 0; i < p->module_count; i++) {
        const Elf64_Sym* symbol = dynlinkModuleFindSymbol(p->modules[i], name);
        if (symbol) {
            if (out_module) *out_module = p->modules[i];
            return symbol;
        }
    }
    return NULL;
}

static size_t _dynlinkAlignUp(size_t value, size_t align) {
    return (value + align - 1) & ~(align - 1);
}

void dynlinkProgramLayoutTls(DynlinkProgram* p) {
    p->tls_start_offset = TLS_TCB_SIZE;
    p->tls_size = 0;
    p->tls_align = TLS_TCB_SIZE;
    if (!p->module_count)
        return;

    // The program's code has the distance from the thread pointer to its
    // block built in: the larger of the control block's size and its alignment.
    const size_t program_align = p->modules[0]->tls_align;
    if (program_align > p->tls_start_offset)
        p->tls_start_offset = program_align;

    for (u32 i = 0; i < p->module_count; i++) {
        DynlinkModule* m = p->modules[i];
        const size_t align = m->tls_align ? m->tls_align : 1;
        m->tls_offset = _dynlinkAlignUp(p->tls_size, align);
        p->tls_size = m->tls_offset + m->tls_size;
        if (align > p->tls_align)
            p->tls_align = align;
    }
}

// What a relocation's symbol resolves to.
typedef struct {
    const DynlinkModule* module;    // The module that defines it. NULL for an undefined weak symbol.
    const Elf64_Sym* symbol;
} DynlinkDefinition;

// A reference binds inside its own module when the symbol is local or not
// visible to others; every other reference takes the program's first definition.
static bool _dynlinkResolve(const DynlinkProgram* p, const DynlinkModule* m, u32 symbol_index,
                            DynlinkDefinition* out, const char** out_name) {
    const Elf64_Sym* reference = &m->symtab[symbol_index];
    const u8 visibility = ELF64_ST_VISIBILITY(reference->st_other);
    if (reference->st_shndx != SHN_UNDEF &&
        (ELF64_ST_BIND(reference->st_info) == STB_LOCAL || visibility == STV_HIDDEN ||
         visibility == STV_INTERNAL || visibility == STV_PROTECTED)) {
        out->module = m;
        out->symbol = reference;
        return true;
    }

    const char* name = m->strtab + reference->st_name;
    out->symbol = dynlinkProgramFindSymbol(p, name, &out->module);
    if (out->symbol)
        return true;
    if (ELF64_ST_BIND(reference->st_info) == STB_WEAK) {
        out->module = NULL;
        return true;
    }
    *out_name = name;
    return false;
}

static u64 _dynlinkSymbolAddress(const DynlinkDefinition* definition) {
    if (!definition->module)
        return 0;
    if (definition->symbol->st_shndx == SHN_ABS)
        return definition->symbol->st_value;
    return definition->module->base + definition->symbol->st_value;
}

static Result _dynlinkApplyRela(const DynlinkProgram* p, const DynlinkModule* m, const Elf64_Rela* rela,
                                size_t count, const char** out_symbol) {
    for (; count--; rela++) {
        const u32 type = ELF64_R_TYPE(rela->r_info);
        const u32 symbol_index = ELF64_R_SYM(rela->r_info);
        u64* target = (u64*)(m->base + rela->r_offset);

        if (type == R_AARCH64_NONE)
            continue;
        if (type == R_AARCH64_RELATIVE) {
            if (!m->is_self_relocated)
                *target = m->base + rela->r_addend;
            continue;
        }

        DynlinkDefinition definition = { m, NULL };
        if (symbol_index && !_dynlinkResolve(p, m, symbol_index, &definition, out_symbol))
            return MAKERESULT(Module_Libnx, LibnxError_NotFound);

        switch (type) {
            case R_AARCH64_ABS64:
            case R_AARCH64_GLOB_DAT:
            case R_AARCH64_JUMP_SLOT:
                *target = _dynlinkSymbolAddress(&definition) + rela->r_addend;
                break;

            case R_AARCH64_TLS_TPREL: {
                // An undefined weak thread-local variable has no address to give.
                if (!definition.module)
                    return MAKERESULT(Module_Libnx, LibnxError_BadReloc);
                const u64 value = definition.symbol ? definition.symbol->st_value : 0;
                *target = p->tls_start_offset + definition.module->tls_offset + value + rela->r_addend;
                break;
            }

            // TLS descriptors, copy relocations and indirect functions are
            // not supported: see dynlink.h.
            default:
                return MAKERESULT(Module_Libnx, LibnxError_BadReloc);
        }
    }
    return 0;
}

static void _dynlinkApplyRelr(const DynlinkModule* m) {
    u64* where = NULL;
    const Elf64_Relr* relr = m->relr;
    for (size_t count = m->relr_count; count--; relr++) {
        if (!(*relr & 1)) {
            where = (u64*)(m->base + *relr);
            *where++ += m->base;
            continue;
        }
        for (u64 bitmap = *relr >> 1, bit = 0; bitmap; bitmap >>= 1, bit++) {
            if (bitmap & 1)
                where[bit] += m->base;
        }
        where += 63;
    }
}

Result dynlinkProgramRelocate(DynlinkProgram* p, const char** out_symbol) {
    const char* missing = NULL;
    Result rc = 0;
    for (u32 i = 0; i < p->module_count && R_SUCCEEDED(rc); i++) {
        DynlinkModule* m = p->modules[i];
        if (m->is_relocated)
            continue;

        if (!m->is_self_relocated)
            _dynlinkApplyRelr(m);
        rc = _dynlinkApplyRela(p, m, m->rela, m->rela_count, &missing);
        if (R_SUCCEEDED(rc))
            rc = _dynlinkApplyRela(p, m, m->jmprel, m->jmprel_count, &missing);
        if (R_SUCCEEDED(rc))
            m->is_relocated = true;
    }
    if (out_symbol) *out_symbol = missing;
    return rc;
}

static u32 _dynlinkModuleIndex(const DynlinkProgram* p, const DynlinkModule* m) {
    for (u32 i = 0; i < p->module_count; i++) {
        if (p->modules[i] == m)
            return i;
    }
    return 0;
}

// Runs the constructors of the libraries a module needs, then its own. A
// module is marked before its dependencies run, so a cycle ends.
static void _dynlinkInitialize(DynlinkProgram* p, DynlinkModule* m) {
    if (m->is_initialized)
        return;
    m->is_initialized = true;

    for (u32 i = 0; i < m->needed_count; i++) {
        DynlinkModule* needed = dynlinkProgramFindModule(p, m->needed[i]);
        if (needed)
            _dynlinkInitialize(p, needed);
    }

    for (size_t i = 0; i < m->init_count; i++)
        m->init_array[i]();
    p->init_order[p->init_order_count++] = (u8)_dynlinkModuleIndex(p, m);
}

void dynlinkProgramRunConstructors(DynlinkProgram* p) {
    for (u32 i = 0; i < p->module_count; i++)
        _dynlinkInitialize(p, p->modules[i]);
}

void dynlinkProgramRunDestructors(DynlinkProgram* p) {
    while (p->init_order_count) {
        DynlinkModule* m = p->modules[p->init_order[--p->init_order_count]];
        for (size_t i = m->fini_count; i--;)
            m->fini_array[i]();
        m->is_initialized = false;
    }
}
