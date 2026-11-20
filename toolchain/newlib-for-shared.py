#!/usr/bin/env python3
# SPDX-License-Identifier: ISC
"""Selects the objects of newlib that go into libounce.so.

libounce.so is the C library of a dynamically linked program, so it holds all
of newlib and not only the objects that libounce itself uses. Two kinds of
objects cannot go in:

  - an object that defines a function libounce defines itself (the allocator,
    fcntl), or that an earlier archive already gave (libsysbase's abort after
    newlib's). A static link never takes these: the symbol is already defined.
  - an object that needs a function no archive has (the long double complex
    functions, regcomp). A static link takes one only when a program uses it,
    and then fails; a shared library with it would not load at all.

The script writes a copy of each archive without those objects, and a report
of what it left out and why.

  newlib-for-shared.py --nm NM --ar AR --out DIR --report FILE \\
      --first libounce_shared.a --provided libgcc.a --allow main \\
      libc.a libm.a libsysbase.a
"""
import argparse
import os
import shutil
import subprocess
import sys


def read_archive(nm, path):
    """Returns the archive's members in order: (name, strong definitions, all definitions, needs)."""
    output = subprocess.run([nm, "-g", path], check=True, capture_output=True, text=True).stdout
    members = []
    for line in output.splitlines():
        if line.endswith(":") and " " not in line:
            members.append((line[:-1], set(), set(), set()))
            continue
        fields = line.split()
        if len(fields) < 2 or not members:
            continue
        kind, symbol = fields[-2], fields[-1]
        _, strong, defined, needs = members[-1]
        if kind == "U":
            needs.add(symbol)
        elif kind in "wv":
            continue
        else:
            defined.add(symbol)
            if kind not in "WV":
                strong.add(symbol)
    return members


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--nm", required=True)
    parser.add_argument("--ar", required=True)
    parser.add_argument("--out", required=True, help="directory for the copies of the archives")
    parser.add_argument("--report", required=True)
    parser.add_argument("--first", action="append", default=[], help="archive linked whole before newlib")
    parser.add_argument("--provided", action="append", default=[], help="archive that the link searches after newlib")
    parser.add_argument("--allow", action="append", default=[], help="symbol that the program defines")
    parser.add_argument("archives", nargs="+")
    arguments = parser.parse_args()

    strong = set()
    available = set(arguments.allow)
    for path in arguments.first:
        for _, member_strong, member_defined, _ in read_archive(arguments.nm, path):
            strong |= member_strong
            available |= member_defined
    for path in arguments.provided:
        for _, _, member_defined, _ in read_archive(arguments.nm, path):
            available |= member_defined

    # An object whose strong definition is already given is left out, as a
    # static link would leave it in its archive.
    kept = []
    dropped = []
    for path in arguments.archives:
        for name, member_strong, member_defined, needs in read_archive(arguments.nm, path):
            taken = sorted(member_strong & strong)
            if taken:
                dropped.append((path, name, "defines %s, which is already defined" % taken[0]))
                continue
            strong |= member_strong
            kept.append((path, name, member_defined, needs))

    # An object that needs a symbol nothing defines is left out, and then the
    # objects that needed it, until none is left.
    while True:
        defined = set(available)
        for _, _, member_defined, _ in kept:
            defined |= member_defined
        remaining = []
        for path, name, member_defined, needs in kept:
            missing = sorted(needs - defined)
            if missing:
                dropped.append((path, name, "needs %s, which nothing defines" % missing[0]))
            else:
                remaining.append((path, name, member_defined, needs))
        if len(remaining) == len(kept):
            break
        kept = remaining

    os.makedirs(arguments.out, exist_ok=True)
    for path in arguments.archives:
        names = [name for dropped_path, name, _ in dropped if dropped_path == path]
        # ar removes an object by its name, so the name must be the only one.
        all_names = [name for name, _, _, _ in read_archive(arguments.nm, path)]
        for name in names:
            if all_names.count(name) != 1:
                sys.exit("%s: more than one object is named %s" % (path, name))
        copy = os.path.join(arguments.out, os.path.basename(path))
        shutil.copyfile(path, copy)
        # ar takes a limited number of arguments at once on some hosts.
        for start in range(0, len(names), 200):
            subprocess.run([arguments.ar, "d", copy] + names[start:start + 200], check=True)
        subprocess.run([arguments.ar, "s", copy], check=True)

    with open(arguments.report, "w") as report:
        report.write("Objects of newlib that are not in libounce.so:\n")
        for path, name, reason in dropped:
            report.write("%s(%s): %s\n" % (os.path.basename(path), name, reason))


if __name__ == "__main__":
    main()
