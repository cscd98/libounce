#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
"""Decides which symbols libounce.so exports, and checks them against the ABI list.

A symbol that libounce.so exports is one that a program or another shared
library can bind to, so it is one that a later libounce.so.1 must keep.
libounce's objects define many global symbols that are for the library's own
files only. A symbol of libounce is exported when it is one of these:

  - declared in a public header: one under libounce's include directories or
    one of the C library's (fcntl and gethostname are libounce's). The script
    preprocesses every header and takes the identifiers in it.
  - a weak definition: a hook that a program replaces, such as __appInit.
  - needed by the toolchain's own libraries (newlib, libstdc++, libgcc): the
    functions under the C library, such as __syscall_lock_acquire.
  - named with --extra: a symbol that compiled code refers to and no header
    declares.

Every global symbol of the newlib archives is exported: the C library's ABI
is newlib's own.

The script writes the linker's version script, and compares libounce's part
with the ABI list, a file in the repository with one name on each line. A
difference fails the build: adding a name there is how a symbol becomes part
of the ABI, and a name can only be removed with a new library version
(libounce.so.2). --update writes the list instead.

  libounce-exports.py --nm NM --cc CC --flag FLAG... --library libounce_shared.a
      --newlib libc.a ... --toolchain libstdc++.a ... --include DIR...
      --system-include DIR --extra NAME... --abi libounce.abi
      --version-script OUT [--update]
"""
import argparse
import os
import re
import subprocess
import sys

IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")


def read_symbols(nm, path):
    """Returns the archive's global symbols: (defined, weakly defined, needed)."""
    output = subprocess.run([nm, "-g", path], check=True, capture_output=True, text=True).stdout
    defined, weak, needed = set(), set(), set()
    for line in output.splitlines():
        fields = line.split()
        if len(fields) < 2 or line.endswith(":"):
            continue
        kind, symbol = fields[-2], fields[-1]
        if kind == "U":
            needed.add(symbol)
        elif kind not in "wv":
            defined.add(symbol)
            if kind in "WV":
                weak.add(symbol)
    return defined, weak, needed


def header_identifiers(cc, flags, include_dirs, header_dirs):
    """Returns the identifiers in every header under the directories, with the
    macros' own text (-dD), and the number of headers that did not preprocess."""
    identifiers = set()
    failed = 0
    seen = set()
    command = [cc, "-E", "-dD", "-x", "c", "-std=gnu11"] + flags
    for directory in include_dirs:
        command += ["-I", directory]
    for directory in header_dirs:
        for root, _, files in sorted(os.walk(directory)):
            for name in sorted(files):
                # One directory can be inside another.
                path = os.path.realpath(os.path.join(root, name))
                if not name.endswith(".h") or path in seen:
                    continue
                seen.add(path)
                result = subprocess.run(command + [os.path.join(root, name)], capture_output=True, text=True)
                if result.returncode:
                    # A header that is only for C++, or that another header includes.
                    failed += 1
                    continue
                identifiers.update(IDENTIFIER.findall(result.stdout))
    return identifiers, failed


def read_abi(path):
    if not os.path.exists(path):
        return set()
    with open(path) as file:
        return {line.strip() for line in file if line.strip() and not line.startswith("#")}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--cc", required=True)
    parser.add_argument("--flag", action="append", default=[], help="compiler flag for the headers, such as -D__SWITCH2__")
    parser.add_argument("--library", required=True, help="libounce's objects")
    parser.add_argument("--newlib", action="append", default=[], help="newlib archive that goes into libounce.so")
    parser.add_argument("--toolchain", action="append", default=[], help="toolchain library that libounce.so serves")
    parser.add_argument("--include", action="append", default=[], help="directory of libounce's public headers")
    parser.add_argument("--system-include", required=True, help="directory of the C library's headers")
    parser.add_argument("--extra", action="append", default=[])
    parser.add_argument("--abi", required=True)
    parser.add_argument("--version-script", required=True)
    parser.add_argument("--update", action="store_true")
    arguments = parser.parse_args()

    defined, weak, _ = read_symbols(arguments.nm, arguments.library)
    newlib_defined = set()
    toolchain_needs = set()
    for path in arguments.newlib:
        archive_defined, _, archive_needs = read_symbols(arguments.nm, path)
        newlib_defined |= archive_defined
        toolchain_needs |= archive_needs
    for path in arguments.toolchain:
        toolchain_needs |= read_symbols(arguments.nm, path)[2]

    declared, failed = header_identifiers(arguments.cc, arguments.flag, arguments.include,
                                          arguments.include + [arguments.system_include])
    exported = {symbol for symbol in defined
                if symbol in weak or symbol in declared or symbol in toolchain_needs or symbol in arguments.extra}
    missing_extra = sorted(set(arguments.extra) - defined)
    if missing_extra:
        sys.exit("libounce-exports: libounce does not define --extra %s" % " ".join(missing_extra))

    header = [
        "# The symbols of libounce that libounce.so.1 exports: its ABI. One name on each line.",
        "# toolchain/libounce-exports.py makes this list and the build checks it; see that file",
        "# for the rule. A name can be added. A name can be removed only with libounce.so.2.",
    ]
    if arguments.update:
        with open(arguments.abi, "w") as file:
            file.write("\n".join(header + sorted(exported)) + "\n")
    else:
        recorded = read_abi(arguments.abi)
        added = sorted(exported - recorded)
        removed = sorted(recorded - exported)
        if added or removed:
            for symbol in added:
                print("libounce-exports: not in %s: %s" % (arguments.abi, symbol), file=sys.stderr)
            for symbol in removed:
                print("libounce-exports: no longer exported: %s" % symbol, file=sys.stderr)
            sys.exit("libounce-exports: the exported symbols differ from the ABI list; "
                     "if the change is meant, run make shared-abi-update")

    with open(arguments.version_script, "w") as file:
        file.write("{\nglobal:\n")
        for symbol in sorted(exported | newlib_defined):
            file.write("    %s;\n" % symbol)
        file.write("local:\n    *;\n};\n")
    print("libounce-exports: %d symbols of libounce and %d of newlib are exported; %d of libounce are not "
          "(%d headers did not preprocess)" % (len(exported), len(newlib_defined - exported),
                                              len(defined - exported), failed))


if __name__ == "__main__":
    main()
