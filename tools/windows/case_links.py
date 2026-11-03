#!/usr/bin/env python3
"""Make a copy of the MSVC and Windows SDK headers and libraries usable from
Linux's case-sensitive filesystem (tools/windows/README.md).

    tools/windows/case_links.py ~/ng2-ps5-deps/winsdk [project folders to scan...]

Windows does not care about case; its headers include each other in whatever
case their authors typed (<Windows.h>, <WinSock2.h>, "../shared/WinDef.h") and
projects link kernel32.lib or Kernel32.Lib alike. This adds, next to each
file, a symbolic link for every other spelling of its name that is used:
the lower-case name, and each spelling an #include in the headers names. A
link only, never a copy; the originals are left as they are.
"""
import os
import re
import sys

INCLUDE = re.compile(rb'^\s*#\s*include\s*[<"]([^>"]+)[>"]', re.M)


def main():
    root = os.path.abspath(os.path.expanduser(sys.argv[1]))
    # Every file, by its folder and its name in lower case.
    by_dir = {}
    for d, _, files in os.walk(root):
        by_dir[d] = {f.lower(): f for f in files if not os.path.islink(os.path.join(d, f))}
    wanted = {}  # folder -> set of spellings
    for d, files in by_dir.items():
        for low, real in files.items():
            if low != real:
                wanted.setdefault(d, set()).add(low)
            if "." in low and not low.endswith((".h", ".hpp", ".hxx", ".inl", ".ipp", ".tlh", ".tli")):
                continue  # (headers only; extension-less ones are the C++ library's)
            try:
                text = open(os.path.join(d, real), "rb").read()
            except OSError:
                continue
            for inc in INCLUDE.findall(text):
                name = inc.decode("latin-1").replace("\\", "/").split("/")[-1]
                wanted.setdefault("*", set()).add(name)
    # The project's own #includes of Windows headers (<ObjBase.h>, ...).
    for extra in sys.argv[2:]:
        for d, _, files in os.walk(os.path.expanduser(extra)):
            for f in files:
                if not f.endswith((".h", ".hpp", ".c", ".cc", ".cpp", ".inl")):
                    continue
                try:
                    text = open(os.path.join(d, f), "rb").read()
                except OSError:
                    continue
                for inc in INCLUDE.findall(text):
                    name = inc.decode("latin-1").replace("\\", "/").split("/")[-1]
                    wanted.setdefault("*", set()).add(name)
    made = 0
    names_anywhere = wanted.pop("*", set())
    for d, files in by_dir.items():
        spellings = set(wanted.get(d, ())) | {n for n in names_anywhere if n.lower() in files}
        for name in spellings:
            real = files.get(name.lower())
            if not real or name == real:
                continue
            link = os.path.join(d, name)
            if not os.path.lexists(link):
                os.symlink(real, link)
                made += 1
    print(f"{made} links made under {root}")


if __name__ == "__main__":
    main()
