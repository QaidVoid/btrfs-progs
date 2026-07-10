#!/usr/bin/env python3
"""Derive logical->physical chunk map from btrscan dense CSV output.

Groups csum-valid tree blocks by delta (physical - bytenr): contiguous runs
sharing one delta correspond to chunk stripes. DUP chunks show the same
bytenr range at two deltas. Overlapping ranges from STALE relocated copies
are demoted by generation (prefer the delta whose blocks are newest).

Output map line: logical length type devid physical [devid2 physical2]
"""
import sys, csv
from collections import defaultdict

NODESIZE = 16384
MERGE_GAP = 64 * 1024 * 1024   # merge same-delta runs separated by < this

BG_DATA, BG_SYSTEM, BG_METADATA, BG_DUP = 0x1, 0x2, 0x4, 0x20

def main(paths):
    rows = []
    for path in paths:
        with open(path) as f:
            for r in csv.DictReader(f):
                if r["csum_ok"] != "1":
                    continue
                rows.append((int(r["bytenr"]), int(r["physical"]),
                             int(r["owner"]), int(r["generation"])))
    rows.sort()
    sys.stderr.write(f"valid tree blocks: {len(rows)}\n")

    # build runs per delta: [lstart, lend, maxgen, nblocks]
    runs = defaultdict(list)
    for bytenr, phys, owner, gen in rows:
        delta = phys - bytenr
        rl = runs[delta]
        if rl and bytenr < rl[-1][1] + MERGE_GAP:
            rl[-1][1] = max(rl[-1][1], bytenr + NODESIZE)
            rl[-1][2] = max(rl[-1][2], gen)
            rl[-1][3] += 1
        else:
            rl.append([bytenr, bytenr + NODESIZE, gen, 1])

    stripes = []   # (lstart, lend, delta, maxgen, nblocks)
    for delta, rl in runs.items():
        for lstart, lend, maxgen, n in rl:
            stripes.append((lstart, lend, delta, maxgen, n))
    stripes.sort()
    sys.stderr.write(f"stripe runs: {len(stripes)}\n")
    for lstart, lend, delta, maxgen, n in stripes:
        sys.stderr.write(f"  run logical {lstart}..{lend} ({(lend-lstart)>>20} MiB) "
                         f"delta {delta} maxgen {maxgen} blocks {n}\n")

    # split logical space at boundaries; per segment collect covering deltas
    bounds = sorted({b for s in stripes for b in (s[0], s[1])})
    out = []
    for i in range(len(bounds) - 1):
        seg_s, seg_e = bounds[i], bounds[i + 1]
        cov = [(maxgen, n, d) for (ls, le, d, maxgen, n) in stripes
               if ls <= seg_s and le >= seg_e]
        if not cov:
            continue
        # newest-generation (then most-populated) stripe first
        cov.sort(key=lambda t: (t[0], t[1]), reverse=True)
        out.append((seg_s, seg_e, tuple(d for _, _, d in cov[:2])))

    # merge adjacent segments with identical delta tuples
    merged = []
    for seg_s, seg_e, deltas in out:
        if merged and merged[-1][1] == seg_s and merged[-1][2] == deltas:
            merged[-1][1] = seg_e
        else:
            merged.append([seg_s, seg_e, deltas])

    print("# logical length type devid physical [devid2 physical2]")
    for seg_s, seg_e, deltas in merged:
        typ = BG_METADATA | (BG_DUP if len(deltas) == 2 else 0)
        fields = [seg_s, seg_e - seg_s, typ]
        for d in deltas:
            fields += [1, seg_s + d]
        print(" ".join(map(str, fields)))
    sys.stderr.write(f"map entries: {len(merged)}\n")

if __name__ == "__main__":
    main(sys.argv[1:])
