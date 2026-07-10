#!/usr/bin/env python3
"""Parse `btrfs inspect-internal dump-tree -t device` output into a full chunk map.

DEV_EXTENT items give (devid, physical) -> (chunk logical, length).
Emits final map lines: logical length type devid physical [devid2 physical2]
Dev-2 extents (the lost ramdisk) are reported as LOST ranges on stderr.

Usage: parse_devtree.py devtree.txt meta-map.txt > final-map.txt
  meta-map.txt (from derive_map.py) is used to tag metadata ranges;
  everything else is assumed DATA.
"""
import sys, re
from collections import defaultdict

BG_DATA, BG_SYSTEM, BG_METADATA, BG_DUP = 0x1, 0x2, 0x4, 0x20

def load_meta_ranges(path):
    ranges = []
    with open(path) as f:
        for line in f:
            if line.startswith("#"): continue
            p = line.split()
            if len(p) >= 3:
                ranges.append((int(p[0]), int(p[0]) + int(p[1])))
    return ranges

def is_meta(logical, meta_ranges):
    return any(s <= logical < e for s, e in meta_ranges)

def main(devtree_path, metamap_path):
    meta_ranges = load_meta_ranges(metamap_path)
    # parse dump
    extents = []  # (logical, length, devid, physical)
    devid = physical = None
    with open(devtree_path) as f:
        lines = f.readlines()
    for i, line in enumerate(lines):
        m = re.search(r"item \d+ key \((\d+) DEV_EXTENT (\d+)\)", line)
        if not m: continue
        devid, physical = int(m.group(1)), int(m.group(2))
        # following lines contain chunk_offset & length
        chunk_offset = length = None
        for j in range(i + 1, min(i + 5, len(lines))):
            mm = re.search(r"chunk_offset (\d+) length (\d+)", lines[j])
            if mm:
                chunk_offset, length = int(mm.group(1)), int(mm.group(2))
                break
            mm = re.search(r"chunk_offset (\d+)", lines[j])
            if mm: chunk_offset = int(mm.group(1))
            mm = re.search(r"^\s*length (\d+)", lines[j])
            if mm: length = int(mm.group(1))
            if chunk_offset is not None and length is not None: break
        if chunk_offset is None or length is None:
            sys.stderr.write(f"WARN: unparsed dev extent at line {i}\n")
            continue
        extents.append((chunk_offset, length, devid, physical))

    sys.stderr.write(f"dev extents parsed: {len(extents)}\n")
    bychunk = defaultdict(list)
    for logical, length, devid, physical in extents:
        bychunk[(logical, length)].append((devid, physical))

    lost = []
    print("# final map from dev tree: logical length type devid physical [devid2 physical2]")
    for (logical, length), stripes in sorted(bychunk.items()):
        alive = [(d, p) for d, p in stripes if d == 1]
        dead = [(d, p) for d, p in stripes if d != 1]
        meta = is_meta(logical, meta_ranges)
        if not alive:
            lost.append((logical, length, "META" if meta else "DATA"))
            continue
        typ = (BG_METADATA if meta else BG_DATA) | (BG_DUP if len(alive) == 2 else 0)
        fields = [logical, length, typ]
        for d, p in alive[:2]:
            fields += [1, p]
        print(" ".join(map(str, fields)))
        if dead:
            sys.stderr.write(f"NOTE: chunk {logical} len {length} lost {len(dead)} stripe(s) on dev2 (partial redundancy loss only)\n")
    total_lost = sum(l for _, l, _ in lost)
    sys.stderr.write(f"LOST chunks (all stripes on dev2): {len(lost)}, {total_lost>>20} MiB\n")
    for logical, length, kind in lost:
        sys.stderr.write(f"  LOST {kind} logical {logical} len {length}\n")

if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
