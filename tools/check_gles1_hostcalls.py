#!/usr/bin/env python3
"""Check core/include/zb/gles1_hostcalls.h against the generator output and the NDK headers.

tools/gen_stubs.py owns the index assignment; core/src/gen/hostcalls.inc is its committed
output. This checker keeps the hand-written header honest:

  * exactly 87 libGLESv1_CM.so rows, contiguous 492-578;
  * the header's table names and pointer flags match the rows and GLES/gl.h;
  * every name is declared by GLES/gl.h;
  * no name is declared by GLES2/gl2.h, because a shared name must keep its existing index.

Run with --check (the ctest form) or without it; both are the same read-only check.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NDK = os.environ.get("NDK", os.path.expanduser("~/android-ndk-r29"))
NDK_HOST = os.environ.get("NDK_HOST", "linux-arm64")
INCLUDE = os.path.join(NDK, "toolchains", "llvm", "prebuilt", NDK_HOST, "sysroot", "usr", "include")

HOSTCALLS = os.path.join(ROOT, "core", "src", "gen", "hostcalls.inc")
HEADER = os.path.join(ROOT, "core", "include", "zb", "gles1_hostcalls.h")
FIRST = 492
LAST = 578
COUNT = 87
POINTERS = 37


def fail(message):
    print("gles1_hostcalls: " + message)
    return 1


def declared(path):
    """name -> parameter list text, for every GL_API ... GL_APIENTRY prototype."""
    text = open(os.path.join(INCLUDE, path), encoding="utf-8", errors="replace").read()
    found = {}
    pattern = re.compile(r"GL_API[^\n]*?\bGL_APIENTRY\s+(gl\w+)\s*\(([^;]*)\);")
    for match in pattern.finditer(text):
        found[match.group(1)] = match.group(2)
    return found


def hostcall_rows():
    rows = []
    for line in open(HOSTCALLS, encoding="ascii"):
        match = re.match(r'\{\s*(\d+)u?,\s*"([^"]+)",\s*"([^"]+)"\s*\}', line.strip())
        if match is None:
            continue
        if match.group(2) != "libGLESv1_CM.so":
            continue
        rows.append((int(match.group(1)), match.group(3)))
    return rows


def header_rows():
    text = open(HEADER, encoding="ascii").read()
    rows = []
    for match in re.finditer(r'\{\s*(\d+)u,\s*"(gl\w+)",\s*(true|false)\s*\}', text):
        rows.append((int(match.group(1)), match.group(2), match.group(3) == "true"))
    constants = {}
    for match in re.finditer(r"ZB_GL1_HC_(gl\w+)\s*=\s*(\d+)u;", text):
        constants[match.group(1)] = int(match.group(2))
    return rows, constants


def main():
    try:
        gles1 = declared("GLES/gl.h")
        gles2 = declared("GLES2/gl2.h")
    except FileNotFoundError as error:
        return fail("NDK headers not found (%s); set NDK and NDK_HOST" % error)

    rows = hostcall_rows()
    if len(rows) != COUNT:
        return fail("hostcalls.inc has %d libGLESv1_CM.so rows, expected %d" % (len(rows), COUNT))
    expected = [(FIRST + i, rows[i][1]) for i in range(COUNT)]
    if rows != expected:
        return fail("libGLESv1_CM.so rows are not contiguous %d-%d" % (FIRST, LAST))
    pointer_names = set(name for name, params in gles1.items() if "*" in params)
    pointer_count = sum(1 for _, name in rows if name in pointer_names)
    if pointer_count != POINTERS:
        return fail("expected %d pointer-taking rows, found %d" % (POINTERS, pointer_count))

    table, constants = header_rows()
    if len(table) != COUNT:
        return fail("gles1_hostcalls.h has %d table rows, expected %d" % (len(table), COUNT))
    if len(constants) != COUNT:
        return fail("gles1_hostcalls.h has %d index constants, expected %d"
                    % (len(constants), COUNT))
    for index, name in rows:
        if name not in gles1:
            return fail("%s (index %d) is not declared by GLES/gl.h" % (name, index))
        if name in gles2:
            return fail("%s (index %d) is also declared by GLES2/gl2.h; a shared name must "
                        "keep its existing index" % (name, index))
        if constants.get(name) != index:
            return fail("ZB_GL1_HC_%s is %s, expected %d"
                        % (name, constants.get(name), index))
    expected_table = [(index, name, name in pointer_names) for index, name in rows]
    if table != expected_table:
        for got, want in zip(table, expected_table):
            if got != want:
                return fail("table row %r, expected %r" % (got, want))
        return fail("table rows differ in length")

    print("gles1_hostcalls: %d names, %d with pointers, %d-%d"
          % (COUNT, POINTERS, FIRST, LAST))
    return 0


if __name__ == "__main__":
    sys.exit(main())
