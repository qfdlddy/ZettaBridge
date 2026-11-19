#!/usr/bin/env python3
"""Import-time fixups for old arm32 guest libraries (NDK r8-era output).

The Android 17 GSI bionic linker no longer carries the pre-M (targetSdk < 23)
compatibility paths, so old libraries need two fixes before the guest linker sees them.
The fixes edit a private extracted copy, never the APK:

1. DT_NEEDED entries holding build-machine paths
   ("C:\\Development\\ndk/platforms/android-9/arch-arm/usr/lib/libc.so") are pointed at
   their own basename. d_val is moved to the tail of the same string, so no string
   bytes change.

2. DT_TEXTREL (and DF_TEXTREL in DT_FLAGS) is replaced by the marker tag
   DT_ZB_TEXTREL. The guest linker ignores the unknown tag. zbrun sees the marker when
   the library's executable segment is mmapped and keeps those pages writable inside
   the emulator, so the linker's text relocations succeed without ever presenting a
   writable+executable segment.

Usage: fix_guest_lib.py <lib.so> [...]   (edits in place; prints what changed)
"""
import os
import struct
import sys
import tempfile

PT_LOAD = 1
PT_DYNAMIC = 2
DT_NULL = 0
DT_NEEDED = 1
DT_STRTAB = 5
DT_TEXTREL = 22
DT_FLAGS = 30
DF_TEXTREL = 0x4
# Keep in sync with core/include/zb/elf_fixups.h.
DT_ZB_TEXTREL = 0x6000_5A42


def vaddr_to_offset(loads, vaddr):
    for p_offset, p_vaddr, p_filesz in loads:
        if p_vaddr <= vaddr < p_vaddr + p_filesz:
            return p_offset + (vaddr - p_vaddr)
    raise ValueError("address 0x%x is not in a file-backed PT_LOAD" % vaddr)


def fix(path):
    with open(path, "rb") as f:
        data = bytearray(f.read())
    if len(data) < 52 or data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
        return "skip (not a little-endian ELF32)"
    e_machine = struct.unpack_from("<H", data, 18)[0]
    if e_machine != 40:
        return "skip (not ARM)"
    e_phoff, = struct.unpack_from("<I", data, 28)
    e_phentsize, e_phnum = struct.unpack_from("<HH", data, 42)
    if e_phnum > 1024:
        return "error: implausible e_phnum (%d)" % e_phnum
    # Packed 32-bit ELFs (e.g. 5play's libRMS.so) sometimes spoof e_phentsize < 32; keep 32-byte
    # Elf32_Phdr stride to match core/src/elf_fixups.cpp and the bionic linker.
    phdr_stride = e_phentsize if e_phentsize >= 32 else 32
    if e_phoff + e_phnum * phdr_stride > len(data):
        return "error: program header table exceeds file size"

    loads = []
    dynamic = None
    for i in range(e_phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, _, _, _ = struct.unpack_from("<IIIIIIII", data, e_phoff + i * phdr_stride)
        if p_type == PT_LOAD:
            loads.append((p_offset, p_vaddr, p_filesz))
        elif p_type == PT_DYNAMIC:
            dynamic = (p_offset, p_filesz)
    if dynamic is None:
        return "skip (no PT_DYNAMIC)"
    if dynamic[0] + dynamic[1] > len(data):
        return "error: PT_DYNAMIC exceeds file size"

    entries = []
    dyn_end = dynamic[0] + (dynamic[1] & ~7)
    for off in range(dynamic[0], dyn_end, 8):
        tag, val = struct.unpack_from("<iI", data, off)
        entries.append((off, tag, val))
        if tag == DT_NULL:
            break

    strtab = next((vaddr_to_offset(loads, val) for _, tag, val in entries if tag == DT_STRTAB), None)
    changes = []
    textrel = False
    for off, tag, val in entries:
        if tag == DT_NEEDED and strtab is not None:
            start = strtab + val
            if start >= len(data):
                return "error: DT_NEEDED offset out of range"
            end = data.find(b"\0", start)
            if end < 0:
                return "error: unterminated DT_NEEDED string"
            name = data[start:end].decode("latin-1")
            cut = max(name.rfind("/"), name.rfind("\\"))
            if cut >= 0:
                struct.pack_into("<iI", data, off, tag, val + cut + 1)
                changes.append("DT_NEEDED %s -> %s" % (name, name[cut + 1:]))
        elif tag == DT_TEXTREL:
            struct.pack_into("<iI", data, off, DT_ZB_TEXTREL, 1)
            textrel = True
        elif tag == DT_FLAGS and val & DF_TEXTREL:
            struct.pack_into("<iI", data, off, tag, val & ~DF_TEXTREL)
            textrel = True
    if textrel:
        if not any(tag == DT_ZB_TEXTREL for _, tag, _ in entries) and \
                not any(struct.unpack_from("<i", data, off)[0] == DT_ZB_TEXTREL for off, _, _ in entries):
            # DF_TEXTREL without a DT_TEXTREL entry: reuse the first spare DT_NULL slot if any.
            nulls = [off for off, tag, _ in entries if tag == DT_NULL]
            spare = dynamic[0] + len(entries) * 8
            if nulls and spare + 8 <= dyn_end:
                struct.pack_into("<iI", data, nulls[0], DT_ZB_TEXTREL, 1)
                struct.pack_into("<iI", data, spare, DT_NULL, 0)
            else:
                return "error: DF_TEXTREL without room for the marker entry"
        changes.append("DT_TEXTREL -> DT_ZB_TEXTREL")

    if changes:
        parent = os.path.dirname(os.path.abspath(path))
        fd, tmp_path = tempfile.mkstemp(prefix=".zb-fix-", suffix=".tmp", dir=parent)
        try:
            with os.fdopen(fd, "wb") as out:
                out.write(data)
            os.replace(tmp_path, path)
        except BaseException:
            try:
                os.unlink(tmp_path)
            except OSError:
                pass
            raise
    return "; ".join(changes) if changes else "unchanged"


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    status = 0
    for path in sys.argv[1:]:
        result = fix(path)
        print("%s: %s" % (path, result))
        if result.startswith("error"):
            status = 1
    sys.exit(status)


if __name__ == "__main__":
    main()
