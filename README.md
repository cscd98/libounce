# libounce

libounce is libnx with changes and additions made for Switch 2.

## Build and install

With devkitPro's devkitA64 (`switch-dev`):

```sh
make
make install
```

`make install` installs libounce into `$DEVKITPRO/libounce`.

`make switch1` and `make install-switch1` build plain libnx and install it
into `$DEVKITPRO/libnx`, as upstream does.

## Packages

libounce is in the `[ounce]` repository for devkitPro's pacman, with OunceVK
and libkaihen: see <https://github.com/ouncebrew/pacman>. Each release also
has the package attached, for `dkp-pacman -U`. It installs the same files as
`make install` and pulls in the devkitPro packages it builds on.

`packaging/Dockerfile` builds the package with `dkp-makepkg` in devkitPro's
image. Pushing a version tag (the date, as `20261118`) builds it on GitHub
and publishes the release, with the annotated tag's message as its notes;
`packaging/release.sh` does the same locally.

## Use

In a Makefile where libnx is used, include libounce's rules in place of libnx's:

```make
include $(DEVKITPRO)/libounce/switch2_rules
```

Then use `-lnx` to link libounce. If you want a small program that uses
newlib's malloc, link `-lounce_core` instead. `switch2_cpu.mk` gives the CPU
flags for Makefiles that do not use the rules.

For CMake, Meson or autotools, `make install` also installs
`$DEVKITPRO/switch2vars.sh`, the CMake toolchain
(`$DEVKITPRO/libounce/cmake/Switch2.cmake`) and `aarch64-none-elf-cmake`:

```sh
source $DEVKITPRO/switch2vars.sh
aarch64-none-elf-cmake -S . -B build && cmake --build build
```

## Dynamic modules

For Make, define `TARGET` (ending in `.elf`), `OBJECTS` and your compile
flags, then include `$(DEVKITPRO)/libounce/libounce-module.mk` before your
rules. Link with `$(LDFLAGS)` and `$(LIBS)`; the helper adds the `.nro`
target.

For CMake, configure with
`-DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/libounce/cmake/Switch2Module.cmake`.
Create a `SHARED` library target, include
`$ENV{DEVKITPRO}/libounce/libounce-module.cmake`, then call
`libounce_add_module(target)`. This toolchain is for modules only;
applications continue to use `Switch2.cmake`.

Hosts use `moduleLoad`, `moduleGetFunction`, `moduleClose` and
`moduleGetError` from `<switch2/runtime/module.h>`. Link hosts with
`--export-dynamic,--hash-style=sysv` and explicitly retain the runtime
symbols and libraries their modules need. The host chooses this export
policy. `ModuleLoadOptions.required_symbol` optionally checks an entry
point before constructors run; `trace_path` optionally enables a log.

The loader supports host imports, constructors, C++ finalizers and
traditional AArch64 TLS. It does not resolve dependencies between modules
or support C++ exception unwinding across module boundaries. Loading and
closing perform file/service I/O and belong outside frame callbacks.
Stop module execution before closing; live TLS or a failed unmap retains
the mapping and its backing storage.

## License

libnx and libounce: ISC (`LICENSE` and `LICENSES/`). mimalloc
(`nx/external/mimalloc`): MIT. The CMake toolchain and `switch2vars.sh`
(`toolchain/`): MIT (`LICENSES/toolchain-MIT.txt`).

libnx is by switchbrew and its contributors:
[https://github.com/switchbrew/libnx](https://github.com/switchbrew/libnx).
