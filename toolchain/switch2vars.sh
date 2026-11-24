# SPDX-License-Identifier: MIT
#
# The build environment for Switch 2 with libounce. Source this file before
# you configure a CMake, Meson or autotools project. It is the Switch 2
# version of devkitPro's switchvars.sh.
#
# With OUNCE_SHARED=1 in the environment, the flags are for a dynamically
# linked program or a shared library, as SWITCH2_SHARED_ARCH and
# SWITCH2_SHARED_SPECS are in a Makefile, and aarch64-none-elf-pkg-config
# gives the shared libraries of the Switch 2 portlibs. For CMake, pass
# -DOUNCE_SHARED=ON to aarch64-none-elf-cmake.
. "${DEVKITPRO}/devkita64.sh"

export PORTLIBS_PREFIX="${DEVKITPRO}/portlibs/switch2"
export PATH="${PORTLIBS_PREFIX}/bin:${DEVKITPRO}/portlibs/switch/bin:$PATH"

# The Switch 2's CPU, from libounce's switch2_cpu.mk.
SWITCH2_CPU="$(sed -n 's/^SWITCH2_CPU[[:space:]]*:=[[:space:]]*//p' "${DEVKITPRO}/libounce/switch2_cpu.mk")"
if [ -n "${OUNCE_SHARED:-}" ]; then
    export OUNCE_SHARED
    export ARCH="${SWITCH2_CPU} -mtp=soft -fPIC -ftls-model=initial-exec"
    SWITCH2_LINK="-specs=${DEVKITPRO}/libounce/switch_shared.specs -L${PORTLIBS_PREFIX}/lib/shared -Wl,-rpath-link,${PORTLIBS_PREFIX}/lib/shared"
else
    export ARCH="${SWITCH2_CPU} -mtp=soft -fPIC -ftls-model=local-exec"
    SWITCH2_LINK=""
fi
export CFLAGS="${ARCH} -O2 -ffunction-sections -fdata-sections"
export CXXFLAGS="${CFLAGS}"
export CPPFLAGS="-D__SWITCH__ -D__SWITCH2__ -I${PORTLIBS_PREFIX}/include -I${DEVKITPRO}/portlibs/switch/include -isystem ${DEVKITPRO}/libounce/include"
export LDFLAGS="${ARCH} ${SWITCH2_LINK} -L${PORTLIBS_PREFIX}/lib -L${DEVKITPRO}/portlibs/switch/lib -L${DEVKITPRO}/libounce/lib"
export LIBS="-lounce"
