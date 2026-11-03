#!/usr/bin/env python3
"""The game's files by name: list them, copy them out, start a mod from them.

Ninja Gaiden II keeps its files in the .ng2 archives with no names in them:
the names are a table in default.xex (one path per file id, e.g.
char\\ryu\\ryu.gmd), the sizes in load_img.bin and the places in the
archives in pack_img.bin. This reads all three from your own copy of the game
(its folder, the one with default.xex) - nothing of the game is in this tool.

    ng2_files.py GAME list                      every file: id, size, archive, path
    ng2_files.py GAME list ryu                  the ones whose path has "ryu" in it
    ng2_files.py GAME extract OUT 'char\\ryu\\*' copies them to OUT/char/ryu/...
    ng2_files.py GAME new-mod MODS "Ryu's look" 'char\\ryu\\ryu.gmd'
        makes MODS/Ryu's look/ with a mod.toml and files/char/ryu/ryu.gmd -
        edit that file, then copy the folder to the console's Mods folder

Patterns match the whole path, any case, * and ? as wildcards. The mods'
folder layout is in docs/MODDING.md. Needs Python 3.8+ and the
'cryptography' package (default.xex is encrypted).
"""

import argparse
import fnmatch
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from xex_image import XexImage  # noqa: E402

FILE_NAMES = 0x83AEBC98  # a path string per file id (as izuna_mods.cpp)
FILE_COUNT = 5099
# pack_img.bin's first six slots (the executable's table at 0x83AF0D40).
SLOTS = [["cmn.ng2"], ["char.ng2"], ["s_chap_%02d.ng2" % c for c in range(1, 15)],
         ["rtm.ng2"], ["other.ng2"], ["other_rtm.ng2"]]


def be32(b, o):
    return struct.unpack_from(">I", b, o)[0]


class Game:
    def __init__(self, folder, cache):
        self.folder = folder
        xex = os.path.join(folder, "default.xex")
        if not os.path.exists(xex):
            sys.exit(f"{folder}: no default.xex - give the folder of your copy of the game")
        img = XexImage.load(xex, cache)
        self.names = []
        for i in range(FILE_COUNT):
            o = img.offset(FILE_NAMES + 4 * i)
            p = img.offset(struct.unpack(">I", img.data[o:o + 4])[0])
            self.names.append(img.data[p:img.data.index(b"\0", p)].decode("latin-1"))
        load = open(os.path.join(folder, "load_img.bin"), "rb").read()
        self.sizes = [be32(load, 16 + 8 * i) for i in range(FILE_COUNT)]
        pack = open(os.path.join(folder, "pack_img.bin"), "rb").read()
        tables = [be32(pack, 4 * i) for i in range(4)]
        self.where = {}  # id -> (archive, offset, size in the archive)
        for slot, archives in enumerate(SLOTS):
            for a, archive in enumerate(archives):
                p = [be32(pack, tables[i] + 4 * slot) for i in range(4)]
                count = be32(pack, p[1] + 4 * a)
                ids, offs, szs = (be32(pack, p[i] + 4 * a) for i in (0, 2, 3))
                for k in range(count):
                    self.where.setdefault(be32(pack, ids + 4 * k),
                                          (archive, be32(pack, offs + 4 * k), be32(pack, szs + 4 * k)))

    def match(self, patterns):
        pats = [p.replace("/", "\\").lower() for p in patterns]
        for i, name in enumerate(self.names):
            n = name.lower()
            if not pats or any(fnmatch.fnmatchcase(n, p) or (not any(c in p for c in "*?") and p in n)
                               for p in pats):
                yield i

    def read(self, i):
        # A DLC file is not on the disc: the archives keep a placeholder for
        # it, and load_img.bin no size.
        if i not in self.where or not 0 < self.sizes[i] <= self.where[i][2]:
            return None
        archive, offset, _ = self.where[i]
        with open(os.path.join(self.folder, archive), "rb") as f:
            f.seek(offset)
            return f.read(self.sizes[i])


def write_file(root, name, data):
    path = os.path.join(root, *name.split("\\"))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)
    return path


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("game", help="your copy of the game: the folder with default.xex and the .ng2 archives")
    ap.add_argument("--cache", default=os.path.join(os.path.expanduser("~"), ".cache", "izuna", "image.bin"),
                    help="where the decrypted executable is kept between runs")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list")
    p.add_argument("pattern", nargs="*")
    p = sub.add_parser("extract")
    p.add_argument("out")
    p.add_argument("pattern", nargs="+")
    p = sub.add_parser("new-mod")
    p.add_argument("mods", help="the folder to make the mod in")
    p.add_argument("name")
    p.add_argument("pattern", nargs="+", help="the game files it starts from")
    args = ap.parse_args()

    game = Game(args.game, args.cache)
    if args.cmd == "list":
        n = 0
        for i in game.match(args.pattern):
            where = game.where.get(i)
            dlc = not where or not game.sizes[i]
            print(f"{i:5d}  {game.sizes[i]:10d}  {'(DLC)' if dlc else where[0]:14s}  {game.names[i]}")
            n += 1
        print(f"{n} file(s)", file=sys.stderr)
        return 0

    root = args.out if args.cmd == "extract" else os.path.join(args.mods, args.name, "files")
    done = 0
    for i in game.match(args.pattern):
        data = game.read(i)
        if data is None:
            print(f"skipped {game.names[i]}: not in the archives (a DLC file)", file=sys.stderr)
            continue
        print(write_file(root, game.names[i], data))
        done += 1
    if args.cmd == "new-mod" and done:
        toml = os.path.join(args.mods, args.name, "mod.toml")
        if not os.path.exists(toml):
            with open(toml, "w", encoding="utf-8") as f:
                f.write(f'name = "{args.name}"\nauthor = ""\nversion = "1.0"\n'
                        'description = ""\n')
            print(toml)
    print(f"{done} file(s)", file=sys.stderr)
    return 0 if done else 1


if __name__ == "__main__":
    sys.exit(main())
