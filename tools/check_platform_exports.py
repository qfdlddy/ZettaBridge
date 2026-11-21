#!/usr/bin/env python3
"""Compare the arm32 guest stub libraries against the platform's real export sets.

Why this exists: Android links a guest library at load time with BIND_NOW, so a single strong
undefined symbol that the platform provides and we do not stops the whole library from loading,
and the plugin then fails with nothing but `JNI_ERR returned from JNI_OnLoad`. Punch Quest and
Topsoil both died that way before the GLES 1 OES entry points and the `SL_IID_*` objects were
added.

Baselines live in tools/platform_exports/<lib>.baseline.txt: the exported names of the Android 15
platform library with the same name. Symbol names do not depend on the ABI, so an x86_64 image is
a valid name baseline for an arm32 guest. `<lib>.gap.txt` records the names we knowingly do not
export yet, so the build fails when a gap is *added* - which is exactly the mistake that is
invisible until a game is on the device - and also when a gap entry becomes stale.

Usage:
  python tools/check_platform_exports.py [--libs build/guest/lib]

Exits 0 and prints SKIP when the guest libraries have not been built, so a host-only checkout
stays green; tools/build_guest.sh runs it immediately after a real build.
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE_DIR = os.path.join(ROOT, "tools", "platform_exports")
NDK = os.environ.get("NDK", os.path.expanduser("~/android-ndk-r29"))
NDK_HOST = os.environ.get("NDK_HOST", "linux-x86_64")

# Only these libraries have a captured baseline. A library without one is not checked: its
# surface is defined by the NDK headers and the games that use it, not by the platform.
LIBS = ("libGLESv1_CM", "libGLESv2", "libOpenSLES")


def llvm_nm():
    hosts = [NDK_HOST]
    if sys.platform == "win32":
        hosts.append("windows-x86_64")
    elif sys.platform == "darwin":
        hosts.append("darwin-x86_64")
    else:
        hosts.append("linux-x86_64")
    for host in dict.fromkeys(hosts):
        for name in ("llvm-nm", "llvm-nm.exe"):
            path = os.path.join(NDK, "toolchains", "llvm", "prebuilt", host, "bin", name)
            if os.path.isfile(path):
                return path
    found = None
    for directory in os.environ.get("PATH", "").split(os.pathsep):
        for name in ("llvm-nm", "llvm-nm.exe"):
            candidate = os.path.join(directory, name)
            if os.path.isfile(candidate):
                found = candidate
                break
        if found:
            break
    if not found:
        sys.exit("check_platform_exports: llvm-nm not found; set NDK or add it to PATH")
    return found


def read_names(path):
    names = set()
    with open(path) as handle:
        for line in handle:
            line = line.strip()
            if line and not line.startswith("#"):
                names.add(line)
    return names


def exported_names(nm, path):
    out = subprocess.run([nm, "-D", "--defined-only", "--format=posix", path],
                         capture_output=True, text=True).stdout
    names = set()
    for line in out.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        name = parts[0].split("@")[0]
        if name and not name.startswith("$"):
            names.add(name)
    return names


def main(argv):
    libs_dir = os.path.join(ROOT, "build", "guest", "lib")
    if "--libs" in argv:
        libs_dir = argv[argv.index("--libs") + 1]

    present = [lib for lib in LIBS if os.path.isfile(os.path.join(libs_dir, lib + ".so"))]
    if not present:
        print("SKIP check_platform_exports: no guest libraries in " + libs_dir)
        return 0

    nm = llvm_nm()
    failures = []
    all_baseline = set()
    ours_by_lib = {}
    for lib in LIBS:
        all_baseline |= read_names(os.path.join(BASELINE_DIR, lib + ".baseline.txt"))

    for lib in present:
        baseline = read_names(os.path.join(BASELINE_DIR, lib + ".baseline.txt"))
        gap = read_names(os.path.join(BASELINE_DIR, lib + ".gap.txt"))
        ours = exported_names(nm, os.path.join(libs_dir, lib + ".so"))
        ours_by_lib[lib] = ours

        unknown_gap = gap - baseline
        if unknown_gap:
            failures.append("%s.gap.txt names %d symbol(s) the platform does not export: %s" % (
                lib, len(unknown_gap), ", ".join(sorted(unknown_gap)[:8])))

        missing = baseline - ours - gap
        if missing:
            failures.append(
                "%s exports %d fewer platform symbol(s) than the recorded gap allows: %s" % (
                    lib, len(missing), ", ".join(sorted(missing)[:12])))

        stale = gap & ours
        if stale:
            failures.append(
                "%s.gap.txt lists %d symbol(s) that are exported now; delete them: %s" % (
                    lib, len(stale), ", ".join(sorted(stale)[:12])))

        unknown_export = ours - all_baseline
        if unknown_export:
            failures.append(
                "%s exports %d name(s) no platform library exports: %s" % (
                    lib, len(unknown_export), ", ".join(sorted(unknown_export)[:12])))

    skipped = [lib for lib in LIBS if lib not in present]
    if skipped:
        print("skipped (not built): " + " ".join(skipped))
    if failures:
        for failure in failures:
            print("FAIL " + failure)
        return 1
    print("platform exports match the recorded baseline")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
