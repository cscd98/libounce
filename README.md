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

Each release has a package for devkitPro's pacman:

```sh
dkp-pacman -U libounce-20261117-1-any.pkg.tar.zst
```

It installs the same files as `make install`. `packaging/Dockerfile` builds
it with `dkp-makepkg` in devkitPro's image. Pushing a version tag (the date,
as `20261118`) builds the package on GitHub and publishes the release, with
the annotated tag's message as its notes; `packaging/release.sh` does the
same locally.

## Use

In a Makefile where libnx is used, include libounce's rules in place of libnx's:

```make
include $(DEVKITPRO)/libounce/switch2_rules
```

Then use `-lnx` to link libounce. If you want a small program that uses
newlib's malloc, link `-lounce_core` instead. `switch2_cpu.mk` gives the CPU
flags for Makefiles that do not use the rules.

## License

libnx and libounce: ISC (`LICENSE` and `LICENSES/`). mimalloc
(`nx/external/mimalloc`): MIT.

libnx is by switchbrew and its contributors:
[https://github.com/switchbrew/libnx](https://github.com/switchbrew/libnx).
