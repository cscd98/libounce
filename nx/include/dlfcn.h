/**
 * @file dlfcn.h
 * @brief Loading shared libraries while the program runs (Switch 2).
 * @author kurumuz
 * @copyright 2026 libounce contributors
 * @license ISC
 * @note Only a dynamically linked program has these functions: they are in libounce.so, the
 *       runtime library that linked the program (see switch2/runtime/dynlink.h). A statically
 *       linked program that calls them does not link.
 * @note A library is found by its file name in the program's directory, then in
 *       sdmc:/kaihen/lib. A name with a slash is a path and is opened as it is.
 * @note Every loaded library is in the global scope: its symbols are found by RTLD_DEFAULT and
 *       by libraries that are loaded later. \ref RTLD_LOCAL is accepted and has no effect.
 * @note A library's thread-local variables take room that each thread keeps for libraries
 *       loaded this way (4 KiB). \ref dlopen fails when it is used up.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/// Accepted for compatibility: all symbols are resolved when the library is loaded.
#define RTLD_LAZY   0x001
/// Resolve all symbols when the library is loaded. This is always done.
#define RTLD_NOW    0x002
/// Accepted for compatibility. See the note on the global scope.
#define RTLD_LOCAL  0x000
/// The library's symbols are available to libraries that are loaded later. This is always so.
#define RTLD_GLOBAL 0x100

/// A handle for \ref dlsym: the program and all loaded libraries, in the order they were loaded.
#define RTLD_DEFAULT ((void*)0)

/**
 * @brief Loads a shared library, with the libraries it needs, and runs their constructors.
 * @param[in] name File name or path of the library. NULL gives a handle for the program itself.
 * @param[in] flags RTLD_* flags.
 * @return A handle for \ref dlsym and \ref dlclose, or NULL: \ref dlerror then gives the reason.
 * @note A library that is already loaded is not loaded again. Each successful call needs one
 *       \ref dlclose.
 */
void* dlopen(const char* name, int flags);

/**
 * @brief Finds a symbol by its name.
 * @param[in] handle A handle from \ref dlopen, to search that library and then the libraries it
 *                   needs, or \ref RTLD_DEFAULT.
 * @param[in] name Symbol name.
 * @return The symbol's address, or NULL: \ref dlerror then gives the reason.
 */
void* dlsym(void* handle, const char* name);

/**
 * @brief Gives up a handle. The library is unloaded when no handle and no loaded library needs it.
 * @param[in] handle A handle from \ref dlopen.
 * @return 0, or nonzero for a value that is not a handle.
 * @note Unloading runs the library's destructors. The caller must not use the library's
 *       functions and data after that, and neither may other libraries.
 */
int dlclose(void* handle);

/**
 * @brief Describes the last failure of these functions on the calling thread.
 * @return The text, or NULL when no function failed since the last call.
 */
char* dlerror(void);

#ifdef __cplusplus
}
#endif
