# SPDX-License-Identifier: MIT
#
# The NintendoSwitch platform for Switch 2 (see ../Switch2.cmake). It loads
# devkitPro's platform file, which reads NX_ROOT (libounce) for the headers,
# libraries and linker specs, and keeps its helpers (nx_create_nro and the
# others). Then it adds what Switch 2 needs, as libounce's switch2_rules
# does for Makefiles:
# - The Switch 2's CPU flags, from libounce's switch2_cpu.mk, in
#   place of devkitPro's Switch 1 flags.
# - __SWITCH2__, which the libounce headers need.
# - The Switch 2 portlibs headers and libraries, before the Switch 1
#   portlibs.
#
# With OUNCE_SHARED (see ../Switch2.cmake) the project is dynamically linked,
# as with SWITCH2_SHARED_ARCH and SWITCH2_SHARED_SPECS in a Makefile:
# - Thread-local variables use the initial-exec model, and programs and
#   shared libraries link with libounce's switch_shared.specs, which puts
#   libounce.so and the module start object in the link.
# - Shared libraries are available: lib<name>.so, with a DT_SONAME. The
#   loader finds a library by that name, so the file on the SD card has it:
#   ounce_create_library_nro makes it.
# - Libraries are found in lib/shared of libounce and of the Switch 2
#   portlibs before the static ones.
include_guard(GLOBAL)

# devkitPro's platform file turns BUILD_SHARED_LIBS off in the cache. Keep
# what the project was configured with.
set(OUNCE_BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS}")

# The headers: libounce, then the Switch 2 portlibs. devkitPro's platform
# file sets these cache entries only when they are not set.
foreach(lang IN ITEMS C CXX ASM)
	set(CMAKE_${lang}_STANDARD_INCLUDE_DIRECTORIES
		"${NX_ROOT}/include;${OUNCE_PORTLIBS}/include" CACHE STRING "")
endforeach()

include("${DEVKITPRO}/cmake/Platform/NintendoSwitch.cmake")

set(NINTENDO_SWITCH2 TRUE)

# Set the flags again with the Switch 2's CPU in place of the Switch 1's.
file(STRINGS "${NX_ROOT}/switch2_cpu.mk" OUNCE_SWITCH2_CPU REGEX "^SWITCH2_CPU[ \t]*:=")
string(REGEX REPLACE "^SWITCH2_CPU[ \t]*:=[ \t]*" "" OUNCE_SWITCH2_CPU "${OUNCE_SWITCH2_CPU}")
if(NOT OUNCE_SWITCH2_CPU)
	message(FATAL_ERROR "No SWITCH2_CPU in ${NX_ROOT}/switch2_cpu.mk: install the Switch 2 devkit again")
endif()
if(OUNCE_SHARED)
	if(NOT EXISTS "${NX_ROOT}/switch_shared.specs")
		message(FATAL_ERROR "OUNCE_SHARED: ${NX_ROOT} has no switch_shared.specs: install a libounce with libounce.so")
	endif()
	set(NX_ARCH_SETTINGS "${OUNCE_SWITCH2_CPU} -mtp=soft -ftls-model=initial-exec")
	set(NX_LINKER_FLAGS "-L${NX_ROOT}/lib -L${DEVKITPRO}/portlibs/switch/lib -specs=${NX_ROOT}/switch_shared.specs")
else()
	set(NX_ARCH_SETTINGS "${OUNCE_SWITCH2_CPU} -mtp=soft -ftls-model=local-exec")
endif()
__dkp_init_platform_settings(NX)

foreach(lang IN ITEMS C CXX ASM)
	string(APPEND CMAKE_${lang}_FLAGS_INIT " -D__SWITCH2__")
endforeach()

if(NOT DKP_PLATFORM_BOOTSTRAP)
	set(CMAKE_EXE_LINKER_FLAGS_INIT "-L${OUNCE_PORTLIBS}/lib ${CMAKE_EXE_LINKER_FLAGS_INIT}")
endif()

if(OUNCE_SHARED)
	# The shared libraries of the Switch 2 portlibs, before its static ones.
	# -rpath-link is where the linker looks for a library that another shared
	# library needs.
	set(OUNCE_SHARED_PORTLIBS "${OUNCE_PORTLIBS}/lib/shared")
	set(CMAKE_EXE_LINKER_FLAGS_INIT
		"-L${OUNCE_SHARED_PORTLIBS} -Wl,-rpath-link,${OUNCE_SHARED_PORTLIBS} ${CMAKE_EXE_LINKER_FLAGS_INIT}")
	set(CMAKE_SHARED_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT}")
	set(CMAKE_MODULE_LINKER_FLAGS_INIT "${CMAKE_EXE_LINKER_FLAGS_INIT}")

	# find_library, and so find_package and pkg_check_modules, take a shared
	# library before a static one of the same name.
	set(CMAKE_FIND_LIBRARY_PREFIXES "lib")
	set(CMAKE_FIND_LIBRARY_SUFFIXES ".so" ".a")
	list(INSERT CMAKE_LIBRARY_PATH 0 "${OUNCE_SHARED_PORTLIBS}" "${NX_ROOT}/lib/shared")

	# Shared libraries, as on an ELF system: libNAME.so with a DT_SONAME. The
	# loader does not read a run path.
	set_property(GLOBAL PROPERTY TARGET_SUPPORTS_SHARED_LIBS TRUE)
	set(BUILD_SHARED_LIBS "${OUNCE_BUILD_SHARED_LIBS}" CACHE BOOL "Build libraries as shared libraries" FORCE)
	set(CMAKE_SHARED_LIBRARY_PREFIX "lib")
	set(CMAKE_SHARED_LIBRARY_SUFFIX ".so")
	set(CMAKE_SHARED_MODULE_PREFIX "lib")
	set(CMAKE_SHARED_MODULE_SUFFIX ".so")
	foreach(lang IN ITEMS C CXX)
		set(CMAKE_SHARED_LIBRARY_SONAME_${lang}_FLAG "-Wl,-soname,")
		set(CMAKE_SHARED_LIBRARY_RPATH_LINK_${lang}_FLAG "-Wl,-rpath-link,")
	endforeach()
	set(CMAKE_SKIP_RPATH TRUE)
endif()

# There is no libdl to link: dlopen is in libounce.so, and a statically
# linked program has none.
set(CMAKE_DL_LIBS "")

# Makes the NRO of a shared library target when the target is built: the file
# that goes on the SD card, in the program's folder or in sdmc:/kaihen/lib.
# Its name is the library's DT_SONAME, which is the name the loader looks for.
#   ounce_create_library_nro(<target> [OUTPUT_DIRECTORY <folder>])
# The folder defaults to nro in the target's build folder.
function(ounce_create_library_nro target)
	cmake_parse_arguments(PARSE_ARGV 1 LIBRARY_NRO "" "OUTPUT_DIRECTORY" "")
	if(NOT OUNCE_SHARED)
		message(FATAL_ERROR "ounce_create_library_nro: configure with -DOUNCE_SHARED=ON")
	endif()
	if(NOT NX_ELF2NRO_EXE)
		message(FATAL_ERROR "ounce_create_library_nro: could not find elf2nro: install switch-tools")
	endif()
	get_target_property(library_type "${target}" TYPE)
	if(NOT library_type STREQUAL "SHARED_LIBRARY")
		message(FATAL_ERROR "ounce_create_library_nro: ${target} is not a shared library")
	endif()

	if(NOT DEFINED LIBRARY_NRO_OUTPUT_DIRECTORY)
		set(LIBRARY_NRO_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/nro")
	endif()
	get_filename_component(LIBRARY_NRO_OUTPUT_DIRECTORY "${LIBRARY_NRO_OUTPUT_DIRECTORY}"
		ABSOLUTE BASE_DIR "${CMAKE_CURRENT_BINARY_DIR}")

	add_custom_command(TARGET "${target}" POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${LIBRARY_NRO_OUTPUT_DIRECTORY}"
		COMMAND "${NX_ELF2NRO_EXE}" "$<TARGET_FILE:${target}>"
			"${LIBRARY_NRO_OUTPUT_DIRECTORY}/$<TARGET_SONAME_FILE_NAME:${target}>"
		VERBATIM
	)
endfunction()
