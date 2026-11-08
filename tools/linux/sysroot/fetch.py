#!/usr/bin/env python3
# Ubuntu 22.04 (jammy) sysroot for IzunaRecomp's Linux build: the newest
# version of each package across jammy / -updates / -security, main + universe,
# downloaded from the archive and unpacked into ./root (no root needed).
import gzip, io, os, subprocess, sys, urllib.request
ARCHIVE = "http://archive.ubuntu.com/ubuntu"
here = os.path.dirname(os.path.abspath(__file__))
pkgs = sys.argv[1:]
index = {}
def vkey(v):
    return v
for dist in ("jammy", "jammy-updates", "jammy-security"):
    for comp in ("main", "universe"):
        url = f"{ARCHIVE}/dists/{dist}/{comp}/binary-amd64/Packages.gz"
        data = gzip.decompress(urllib.request.urlopen(url).read()).decode("utf-8", "replace")
        for block in data.split("\n\n"):
            f = {}
            for line in block.split("\n"):
                if ": " in line and not line.startswith(" "):
                    k, v = line.split(": ", 1); f[k] = v
            if "Package" in f and "Filename" in f:
                n = f["Package"]
                old = index.get(n)
                if old is None or subprocess.run(["dpkg", "--compare-versions", f["Version"], "gt", old["Version"]]).returncode == 0:
                    index[n] = f
debs = os.path.join(here, "debs"); os.makedirs(debs, exist_ok=True)
root = os.path.join(here, "root"); os.makedirs(root, exist_ok=True)
missing = [p for p in pkgs if p not in index]
if missing:
    sys.exit("not in jammy: " + " ".join(missing))
for p in pkgs:
    f = index[p]; dst = os.path.join(debs, os.path.basename(f["Filename"]))
    if not os.path.exists(dst):
        urllib.request.urlretrieve(f"{ARCHIVE}/{f['Filename']}", dst)
    subprocess.run(["dpkg-deb", "-x", dst, root], check=True)
    print(p, f["Version"])
