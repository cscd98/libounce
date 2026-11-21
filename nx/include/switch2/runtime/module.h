/* SPDX-License-Identifier: ISC */
/**
 * @file module.h
 * @brief Host-linked NRO modules for Switch 2.
 * @copyright 2026 libounce contributors
 * @license ISC
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef struct Module Module;
typedef void (*ModuleFunction)(void);

typedef struct {
    const char *required_symbol; ///< Optional function checked before constructors run.
    const char *trace_path;      ///< Optional diagnostic log path; NULL disables tracing.
} ModuleLoadOptions;

/// Loads a module using exports from the main executable; NULL on failure.
/// options may be NULL. Does file I/O and RO service calls; use outside frame callbacks.
Module *moduleLoad(const char *path, const ModuleLoadOptions *options);

/// Looks up an exported function. The pointer is valid until the module is closed.
ModuleFunction moduleGetFunction(Module *module, const char *name);

/// Stop all module execution before closing. Live TLS or an unmap failure retains the mapping.
void moduleClose(Module *module);

/// Last error on this thread, or NULL; borrowed until the next module operation.
char *moduleGetError(void);

#ifdef __cplusplus
}
#endif
