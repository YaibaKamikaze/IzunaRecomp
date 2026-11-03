#!/usr/bin/env python3
"""Compare a native frame dump with a reference (both raw RGBA, same size).

  tools/native/framediff.py <reference.rgba> <frame.rgba>

Prints the mean absolute difference per channel (0-255) and the share of
pixels that differ by more than 48 in any channel. The title's mist moves, so
expect a few units of mean difference between good frames; a broken frame
(missing layer, wrong colours) differs by tens. References are made from the
game: keep them out of the repo (~/ng2-ps5-deps/hosttest/reference/).
"""
import sys

a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
if len(a) != len(b):
    sys.exit(f"size differs: {len(a)} vs {len(b)}")
n = len(a) // 4
step = 7  # (a sample of the pixels is plenty)
tot = [0, 0, 0]
big = cnt = 0
for i in range(0, n, step):
    o = i * 4
    d = [abs(a[o + c] - b[o + c]) for c in range(3)]
    for c in range(3):
        tot[c] += d[c]
    big += max(d) > 48
    cnt += 1
print(f"mean |diff| R {tot[0]/cnt:.1f} G {tot[1]/cnt:.1f} B {tot[2]/cnt:.1f}; {100*big/cnt:.1f}% of pixels differ by > 48")
