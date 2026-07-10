# Chunk tree recovery workflow

Tools and workflow for recovering a btrfs filesystem whose **chunk tree is
unreadable** — typically because all copies of the SYSTEM chunk were lost
(the classic self-inflicted case: a volatile device was added to work around
ENOSPC, and a balance relocated the SYSTEM chunk onto it before the device
vanished).

The symptom:

```
BTRFS error: failed to read chunk root
bad tree block <bytenr>, bytenr mismatch, want=<bytenr>, have=0
```

and `btrfs rescue chunk-recover` crashing with `BUG_ON(!ce)` (issue #393)
when a device is missing.

## Why this is recoverable

Two properties of btrfs make the chunk tree reconstructible from first
principles:

1. **Metadata blocks are self-describing.** Every tree block header records
   its own logical address, the filesystem UUID, its owner tree, generation
   and a CRC32C checksum. A block found at physical offset *P* claiming
   logical address *L* yields one point of the logical→physical map.
2. **The device tree is the chunk tree's inverse.** `DEV_EXTENT` items map
   physical ranges back to chunk logical addresses — for *all* chunks,
   including data chunks whose blocks carry no headers. The device tree
   lives in ordinary METADATA chunks and survives the loss of the SYSTEM
   chunk.

So: scan for surviving metadata block headers → derive the metadata-chunk
map → use it to read the device tree → obtain the complete map → either
extract files (`btrfs restore --chunk-map`) or rebuild the chunk tree
in place (`btrfs rescue inject-chunk-tree`).

## Safety rules

- Work on an image (`dd`), or put a device-mapper snapshot overlay in front
  of the real device for anything that writes:

  ```bash
  losetup --read-only --find --show image.dd          # -> /dev/loopN
  truncate -s 16G cow.img
  losetup --find --show cow.img                       # -> /dev/loopM
  dmsetup create rescue-overlay \
      --table "0 $(blockdev --getsz /dev/loopN) snapshot /dev/loopN /dev/loopM P 16"
  ```

  Writes to `/dev/mapper/rescue-overlay` land in `cow.img`; the image stays
  pristine. Test the whole procedure on the overlay (including a kernel
  mount of the result) before touching real hardware.

## Workflow

### 1. Build the scanner

Edit the `FSID` bytes at the top of `btrscan.c` to your filesystem UUID
(`btrfs inspect-internal dump-super <dev> | grep fsid`), then:

```bash
gcc -O2 -msse4.2 -o btrscan btrscan.c
./btrscan calib <dev>        # must print "MATCH: inverted flavor"
```

### 2. Find metadata regions (probe scan)

Metadata chunks are large and contiguous (SYSTEM ≥ 32 MiB, METADATA
typically 256 MiB–1 GiB), so sampling 64 KiB every 16 MiB cannot miss one.
This turns a full-device scan into minutes:

```bash
./btrscan probe <dev> 16777216 > probe-regions.txt
```

### 3. Dense-scan the found regions

```bash
# edit IMG= in dense_all.sh, or run per region:
#   ./btrscan dense <dev> <start> <end> > region.csv
./dense_all.sh               # -> dense-all.csv
```

Every candidate block is CRC32C-verified; the output records physical
offset, claimed logical address, owner tree, generation and level.

### 4. Derive the metadata chunk map

```bash
python3 derive_map.py dense-all.csv > map-meta.txt
```

Blocks are clustered by `delta = physical − logical`; contiguous runs of one
delta are chunk stripes. Overlapping ranges from **stale relocated copies**
(byproducts of old balances — checksum-valid but outdated) are demoted by
generation: the newest wins.

### 5. Read the device tree, build the full map

```bash
btrfs inspect-internal dump-tree --chunk-map map-meta.txt -t device <dev> > devtree.txt
python3 parse_devtree.py devtree.txt map-meta.txt > map-full.txt
```

`parse_devtree.py` reports chunks whose stripes were all on missing devices
(**LOST**). If the only lost chunk is a 32 MiB SYSTEM chunk, you are in the
fully-recoverable case: the only thing destroyed was the map itself.

### 6a. Extract files (no writes to the patient)

```bash
btrfs restore --chunk-map map-full.txt -l <dev>                # list subvols
btrfs restore --chunk-map map-full.txt -r <subvolid> \
      -m -S -x -i --exclude /.cache/ --exclude /node_modules/ \
      <dev> /destination/
```

### 6b. Or rebuild the chunk tree in place

```bash
btrfs rescue inject-chunk-tree --chunk-map map-full.txt <dev>      # dry run
btrfs rescue inject-chunk-tree --chunk-map map-full.txt -y <dev>   # write
btrfs check --readonly <dev>                                       # verify
mount -o ro <dev> /mnt                                             # verify
```

After a read-write mount (the free-space tree is rebuilt automatically),
run `btrfs scrub` for a full data integrity verification, and consider a
small `btrfs balance -dusage=10` if the device was allocation-starved —
that being how this failure mode usually begins.

## Files

| file | purpose |
|---|---|
| `btrscan.c` | raw scanner: `calib` / `probe` / `dense` modes |
| `dense_all.sh` | drive dense scans over all probe regions |
| `derive_map.py` | tree-block CSV → metadata chunk map |
| `parse_devtree.py` | device tree dump → complete chunk map |
