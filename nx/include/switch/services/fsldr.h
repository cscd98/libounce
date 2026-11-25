/**
 * @file fsldr.h
 * @brief FilesystemProxy-ForLoader (fsp-ldr) service IPC wrapper.
 * @author SciresM
 * @copyright libnx Authors
 */
#pragma once
#include "../types.h"
#include "../sf/service.h"
#include "../services/fs.h"
#include "../crypto/sha256.h"
#include "../services/ncm_types.h"

/// Code verification data that OpenCodeFileSystem returns.
typedef struct {
#if defined(__SWITCH2__)
    // [Switch 2] The signature field is 0x180 bytes, so the structure is
    // 0x1A4 bytes. The Loader-root OpenCodeFileSystem that kaiHEN sends
    // receives 0x1A4 bytes: the hash at 0x180 and is_signed at 0x1A0.
    u8 signature[0x180];
#else
    u8 signature[0x100];
#endif
    u8 hash[SHA256_HASH_SIZE];
    bool is_signed;
    u8 reserved[3];
} FsCodeInfo;

#if defined(__SWITCH2__)
#ifdef __cplusplus
static_assert(sizeof(FsCodeInfo) == 0x1A4, "Switch 2 code verification data");
#else
_Static_assert(sizeof(FsCodeInfo) == 0x1A4, "Switch 2 code verification data");
#endif
#endif

/// Initialize fsp-ldr.
Result fsldrInitialize(void);

/// Exit fsp-ldr.
void fsldrExit(void);

/// Gets the Service object for the actual fsp-ldr service session.
Service* fsldrGetServiceSession(void);

Result fsldrOpenCodeFileSystem(FsCodeInfo* out_code_info, u64 tid, NcmStorageId storage_id, const char *path, FsContentAttributes attr, FsFileSystem* out);
Result fsldrIsArchivedProgram(u64 pid, bool *out);
