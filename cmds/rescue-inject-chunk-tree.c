/*
 * btrfs rescue inject-chunk-tree
 *
 * Rebuild the chunk tree of a filesystem whose SYSTEM chunks were destroyed
 * (classic case: a volatile device was added and a balance relocated the
 * SYSTEM chunk onto it before the device vanished).
 *
 * The filesystem must be opened via chunk-map injection (--chunk-map FILE or
 * the BTRFS_CHUNK_MAP environment variable) so that the device tree and
 * extent tree are readable without a chunk tree; see recovery-tools/ for how
 * to derive such a map from a raw device scan.
 *
 * The rebuilt chunk tree is derived from the two surviving sources of truth:
 *   - DEV_EXTENT items in the device tree  (stripe placement)
 *   - BLOCK_GROUP_ITEMs in the extent tree (chunk type flags)
 *
 * A fresh SYSTEM chunk is allocated in unallocated space on a present
 * device and the superblock's sys_chunk_array is rebuilt around it.  All
 * pre-existing SYSTEM chunks are erased: their only content, the old chunk
 * tree, is superseded by the rebuild.
 *
 * Modes:
 *   default (clean)  every non-SYSTEM chunk must have all stripes on
 *                    present devices; devices that only ever backed the
 *                    lost SYSTEM chunk are removed entirely, leaving a
 *                    filesystem with no missing-device residue.
 *   --degraded       missing devices are kept (device items rebuilt from
 *                    the device tree); chunks with lost stripes remain,
 *                    readable where redundancy allows.  Mount with
 *                    -o degraded afterwards.
 *
 * Limitations: chunks with stripe-order-dependent profiles (RAID0/10/5/6)
 * cannot be reconstructed from dev extents alone (the on-disk order is not
 * recorded there) and are rejected.  Mirror profiles (single/DUP/RAID1*)
 * are order-independent and fully supported.
 */

#include "kerncompat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "kernel-lib/list.h"
#include "kernel-shared/ctree.h"
#include "kernel-shared/volumes.h"
#include "kernel-shared/disk-io.h"
#include "kernel-shared/transaction.h"
#include "kernel-shared/extent_io.h"
#include "kernel-shared/uapi/btrfs_tree.h"
#include "common/messages.h"
#include "cmds/rescue.h"

#define ICX_MAX_STRIPES 4

#define ICX_ORDER_DEPENDENT (BTRFS_BLOCK_GROUP_RAID0 |	\
			     BTRFS_BLOCK_GROUP_RAID10 |	\
			     BTRFS_BLOCK_GROUP_RAID5 |	\
			     BTRFS_BLOCK_GROUP_RAID6)

struct icx_stripe {
	u64 devid;
	u64 physical;
};

struct icx_chunk {
	u64 logical;
	u64 length;
	u64 flags;
	int nstripes;
	int nmissing;
	int have_bg;
	struct icx_stripe s[ICX_MAX_STRIPES];
};

struct icx_ctx {
	struct btrfs_fs_info *fs_info;
	struct icx_chunk *chunks;
	int nchunks;
	int chunks_cap;
	/* devids seen in the device tree, present or not */
	u64 devids[256];
	int ndevids;
	int degraded;
	u64 sys_size;
};

static int icx_dev_present(struct btrfs_fs_info *fs_info, u64 devid)
{
	struct btrfs_device *dev = btrfs_find_device(fs_info, devid, NULL, NULL);

	return dev && dev->fd >= 0;
}

static struct icx_chunk *icx_get_chunk(struct icx_ctx *ctx, u64 logical)
{
	int i;

	for (i = 0; i < ctx->nchunks; i++)
		if (ctx->chunks[i].logical == logical)
			return &ctx->chunks[i];
	if (ctx->nchunks == ctx->chunks_cap) {
		ctx->chunks_cap = ctx->chunks_cap ? ctx->chunks_cap * 2 : 256;
		ctx->chunks = realloc(ctx->chunks,
				ctx->chunks_cap * sizeof(*ctx->chunks));
		if (!ctx->chunks)
			return NULL;
	}
	memset(&ctx->chunks[ctx->nchunks], 0, sizeof(struct icx_chunk));
	ctx->chunks[ctx->nchunks].logical = logical;
	return &ctx->chunks[ctx->nchunks++];
}

static void icx_note_devid(struct icx_ctx *ctx, u64 devid)
{
	int i;

	for (i = 0; i < ctx->ndevids; i++)
		if (ctx->devids[i] == devid)
			return;
	if (ctx->ndevids < (int)ARRAY_SIZE(ctx->devids))
		ctx->devids[ctx->ndevids++] = devid;
}

/* walk the device tree: collect stripes per chunk, keyed by chunk logical */
static int icx_harvest_dev_extents(struct icx_ctx *ctx)
{
	struct btrfs_fs_info *fs_info = ctx->fs_info;
	struct btrfs_root *dev_root = fs_info->dev_root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key = { 0 }, found;
	int ret;

	ret = btrfs_search_slot(NULL, dev_root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;
	while (1) {
		btrfs_item_key_to_cpu(path.nodes[0], &found, path.slots[0]);
		if (found.type == BTRFS_DEV_EXTENT_KEY) {
			struct btrfs_dev_extent *dext;
			struct icx_chunk *c;
			u64 logical, len;

			dext = btrfs_item_ptr(path.nodes[0], path.slots[0],
					      struct btrfs_dev_extent);
			logical = btrfs_dev_extent_chunk_offset(path.nodes[0], dext);
			len = btrfs_dev_extent_length(path.nodes[0], dext);
			icx_note_devid(ctx, found.objectid);
			c = icx_get_chunk(ctx, logical);
			if (!c)
				return -ENOMEM;
			if (!c->length)
				c->length = len;
			if (c->nstripes >= ICX_MAX_STRIPES) {
				error("chunk %llu has more than %d stripes",
				      logical, ICX_MAX_STRIPES);
				btrfs_release_path(&path);
				return -EOVERFLOW;
			}
			c->s[c->nstripes].devid = found.objectid;
			c->s[c->nstripes].physical = found.offset;
			c->nstripes++;
			if (!icx_dev_present(fs_info, found.objectid))
				c->nmissing++;
		}
		ret = btrfs_next_item(dev_root, &path);
		if (ret)
			break;
	}
	btrfs_release_path(&path);
	return 0;
}

/* walk BLOCK_GROUP_ITEMs for chunk type flags */
static int icx_harvest_block_groups(struct icx_ctx *ctx)
{
	struct btrfs_fs_info *fs_info = ctx->fs_info;
	struct btrfs_root *root = btrfs_extent_root(fs_info, 0);
	struct btrfs_path path = { 0 };
	struct btrfs_key key, found;
	int ret;

	key.objectid = 0;
	key.type = BTRFS_BLOCK_GROUP_ITEM_KEY;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;
	while (1) {
		btrfs_item_key_to_cpu(path.nodes[0], &found, path.slots[0]);
		if (found.type == BTRFS_BLOCK_GROUP_ITEM_KEY) {
			struct btrfs_block_group_item bgi;
			struct icx_chunk *c;

			read_extent_buffer(path.nodes[0], &bgi,
				btrfs_item_ptr_offset(path.nodes[0], path.slots[0]),
				sizeof(bgi));
			c = icx_get_chunk(ctx, found.objectid);
			if (!c) {
				btrfs_release_path(&path);
				return -ENOMEM;
			}
			c->flags = btrfs_stack_block_group_flags(&bgi);
			c->have_bg = 1;
			if (!c->length)
				c->length = found.offset;
			else if (c->length != found.offset)
				warning(
			"chunk %llu: length %llu (bg) != %llu (dev extents)",
					found.objectid, found.offset, c->length);
		}
		ret = btrfs_next_item(root, &path);
		if (ret)
			break;
	}
	btrfs_release_path(&path);
	return 0;
}

/* is this old chunk erased by the rebuild (all old SYSTEM chunks are)? */
static int icx_chunk_erased(const struct icx_chunk *c)
{
	return (c->flags & BTRFS_BLOCK_GROUP_SYSTEM) || !c->have_bg ||
	       c->nstripes == 0;
}

/* find `need` free bytes on a present device; returns devid via *devid_ret */
static int icx_find_gap(struct icx_ctx *ctx, u64 need, u64 *devid_ret,
			u64 *start_ret)
{
	struct btrfs_fs_info *fs_info = ctx->fs_info;
	int d, i, j, n;
	/* worst case every chunk has a stripe on this device */
	u64 (*ext)[2] = malloc(sizeof(u64[2]) * (ctx->nchunks * ICX_MAX_STRIPES + 1));

	if (!ext)
		return -ENOMEM;
	for (d = 0; d < ctx->ndevids; d++) {
		u64 devid = ctx->devids[d];
		struct btrfs_device *dev;
		u64 pos = SZ_1M;

		if (!icx_dev_present(fs_info, devid))
			continue;
		dev = btrfs_find_device(fs_info, devid, NULL, NULL);
		n = 0;
		for (i = 0; i < ctx->nchunks; i++) {
			struct icx_chunk *c = &ctx->chunks[i];

			for (j = 0; j < c->nstripes; j++)
				if (c->s[j].devid == devid) {
					ext[n][0] = c->s[j].physical;
					ext[n][1] = c->length;
					n++;
				}
		}
		/* insertion sort by physical offset */
		for (i = 1; i < n; i++) {
			u64 p0 = ext[i][0], p1 = ext[i][1];

			for (j = i - 1; j >= 0 && ext[j][0] > p0; j--) {
				ext[j + 1][0] = ext[j][0];
				ext[j + 1][1] = ext[j][1];
			}
			ext[j + 1][0] = p0;
			ext[j + 1][1] = p1;
		}
		for (i = 0; i < n; i++) {
			if (ext[i][0] >= pos + need)
				goto found;
			if (ext[i][0] + ext[i][1] > pos)
				pos = ext[i][0] + ext[i][1];
		}
		if (dev->total_bytes >= pos + need)
			goto found;
		continue;
found:
		*devid_ret = devid;
		*start_ret = pos;
		free(ext);
		return 0;
	}
	free(ext);
	return -ENOSPC;
}

static void icx_fill_chunk_item(struct btrfs_fs_info *fs_info,
				struct btrfs_chunk *chunk,
				const struct icx_chunk *c)
{
	/* struct btrfs_chunk embeds the first stripe as its last member */
	struct btrfs_stripe *stripes = &chunk->stripe;
	int i;

	memset(chunk, 0, btrfs_chunk_item_size(c->nstripes));
	btrfs_set_stack_chunk_length(chunk, c->length);
	btrfs_set_stack_chunk_owner(chunk, BTRFS_EXTENT_TREE_OBJECTID);
	btrfs_set_stack_chunk_stripe_len(chunk, BTRFS_STRIPE_LEN);
	btrfs_set_stack_chunk_type(chunk, c->flags);
	btrfs_set_stack_chunk_io_align(chunk, BTRFS_STRIPE_LEN);
	btrfs_set_stack_chunk_io_width(chunk, BTRFS_STRIPE_LEN);
	btrfs_set_stack_chunk_sector_size(chunk, fs_info->sectorsize);
	btrfs_set_stack_chunk_num_stripes(chunk, c->nstripes);
	btrfs_set_stack_chunk_sub_stripes(chunk, 1);
	for (i = 0; i < c->nstripes; i++) {
		struct btrfs_device *dev = btrfs_find_device(fs_info,
						c->s[i].devid, NULL, NULL);

		btrfs_set_stack_stripe_devid(&stripes[i], c->s[i].devid);
		btrfs_set_stack_stripe_offset(&stripes[i], c->s[i].physical);
		if (dev)
			memcpy(stripes[i].dev_uuid, dev->uuid, BTRFS_UUID_SIZE);
	}
}

static int icx_insert_dev_item(struct btrfs_trans_handle *trans,
			       struct btrfs_root *chunk_root, u64 devid,
			       u64 total_bytes, u64 bytes_used,
			       const u8 *dev_uuid, int first,
			       struct btrfs_fs_info *fs_info)
{
	struct btrfs_dev_item dev_item;
	struct btrfs_key key;
	int ret;

	key.objectid = BTRFS_DEV_ITEMS_OBJECTID;
	key.type = BTRFS_DEV_ITEM_KEY;
	key.offset = devid;
	memset(&dev_item, 0, sizeof(dev_item));
	btrfs_set_stack_device_generation(&dev_item, 0);
	btrfs_set_stack_device_id(&dev_item, devid);
	btrfs_set_stack_device_total_bytes(&dev_item, total_bytes);
	btrfs_set_stack_device_bytes_used(&dev_item, bytes_used);
	btrfs_set_stack_device_io_align(&dev_item, fs_info->sectorsize);
	btrfs_set_stack_device_io_width(&dev_item, fs_info->sectorsize);
	btrfs_set_stack_device_sector_size(&dev_item, fs_info->sectorsize);
	memcpy(dev_item.uuid, dev_uuid, BTRFS_UUID_SIZE);
	memcpy(dev_item.fsid, fs_info->fs_devices->metadata_uuid,
	       BTRFS_FSID_SIZE);

	if (first) {
		/*
		 * First insertion into the still-empty chunk root: the tree
		 * checker rejects empty chunk-tree leaves, so bypass it with
		 * a manual path.
		 */
		struct btrfs_path ipath = { 0 };

		ipath.skip_check_block = 1;
		ret = btrfs_insert_empty_item(trans, chunk_root, &ipath, &key,
					      sizeof(dev_item));
		if (ret == 0) {
			write_extent_buffer(ipath.nodes[0], &dev_item,
				btrfs_item_ptr_offset(ipath.nodes[0],
						      ipath.slots[0]),
				sizeof(dev_item));
			btrfs_mark_buffer_dirty(ipath.nodes[0]);
		}
		btrfs_release_path(&ipath);
	} else {
		ret = btrfs_insert_item(trans, chunk_root, &key, &dev_item,
					sizeof(dev_item));
	}
	return ret;
}

int btrfs_rescue_inject_chunk_tree(const char *path, const char *chunk_map,
				   int degraded, u64 sys_size, int yes)
{
	struct btrfs_fs_info *fs_info;
	struct btrfs_super_block *sb;
	struct btrfs_trans_handle *trans;
	struct btrfs_root *chunk_root;
	struct open_ctree_args oca = { 0 };
	struct extent_buffer *cow;
	struct btrfs_disk_key disk_key;
	struct btrfs_key key;
	struct icx_ctx ctx = { 0 };
	struct icx_chunk sys_chunk = { 0 };
	u64 sys_devid = 0, sys_phys = 0, sys_logical = 0;
	u64 total_present = 0, dead_bytes = 0;
	u8 chunk_buf[sizeof(struct btrfs_chunk) +
		     ICX_MAX_STRIPES * sizeof(struct btrfs_stripe)];
	struct btrfs_chunk *chunk = (struct btrfs_chunk *)chunk_buf;
	int i, j, ret, first_item = 1;
	int n_alive = 0, n_partial = 0, n_dead = 0, n_sys = 0;

	if (!sys_size)
		sys_size = SZ_32M;

	if (!chunk_map && !getenv("BTRFS_CHUNK_MAP")) {
		error("a chunk map is required: --chunk-map FILE (see recovery-tools/)");
		return 1;
	}

	oca.filename = path;
	oca.chunk_map = chunk_map;
	oca.flags = OPEN_CTREE_WRITES | OPEN_CTREE_PARTIAL |
		    OPEN_CTREE_ALLOW_TRANSID_MISMATCH;
	fs_info = open_ctree_fs_info(&oca);
	if (!fs_info) {
		error("cannot open filesystem");
		return 1;
	}
	sb = fs_info->super_copy;
	ctx.fs_info = fs_info;
	ctx.degraded = degraded;
	ctx.sys_size = sys_size;

	if (!fs_info->chunk_map_injected) {
		error("filesystem was not opened via chunk map injection");
		goto fail;
	}
	if (!fs_info->dev_root || !fs_info->dev_root->node) {
		error("device tree not readable");
		goto fail;
	}
	/* the tree checker rejects the transiently-empty new chunk root */
	fs_info->suppress_check_block_errors = 1;

	/* recover the real chunk tree uuid from the dev tree root header */
	read_extent_buffer(fs_info->dev_root->node, fs_info->chunk_tree_uuid,
			btrfs_header_chunk_tree_uuid(fs_info->dev_root->node),
			BTRFS_UUID_SIZE);

	/* injected open skips dev items; primary device fields from sb */
	{
		struct btrfs_device *dev = btrfs_find_device(fs_info,
			btrfs_stack_device_id(&sb->dev_item), NULL, NULL);

		if (dev && !dev->total_bytes) {
			dev->total_bytes =
				btrfs_stack_device_total_bytes(&sb->dev_item);
			memcpy(dev->uuid, sb->dev_item.uuid, BTRFS_UUID_SIZE);
		}
	}

	printf("harvesting device tree and extent tree...\n");
	ret = icx_harvest_dev_extents(&ctx);
	if (ret) {
		error("harvest dev extents: %d", ret);
		goto fail;
	}
	ret = icx_harvest_block_groups(&ctx);
	if (ret) {
		error("harvest block groups: %d", ret);
		goto fail;
	}

	/* classify */
	for (i = 0; i < ctx.nchunks; i++) {
		struct icx_chunk *c = &ctx.chunks[i];

		if (c->flags & BTRFS_BLOCK_GROUP_SYSTEM) {
			n_sys++;
			continue;	/* always erased+replaced */
		}
		if (c->flags & ICX_ORDER_DEPENDENT) {
			error(
	"chunk %llu uses a stripe-order-dependent profile (flags 0x%llx); "
	"dev extents do not record stripe order — unsupported",
			      c->logical, c->flags);
			goto fail;
		}
		if (!c->have_bg) {
			error("chunk %llu has dev extents but no block group; "
			      "run btrfs check first, aborting", c->logical);
			goto fail;
		}
		if (c->nstripes == 0 || c->nmissing == c->nstripes)
			n_dead++;
		else if (c->nmissing)
			n_partial++;
		else
			n_alive++;
	}
	printf("chunks: %d intact, %d degraded, %d lost, %d system (replaced)\n",
	       n_alive, n_partial, n_dead, n_sys);

	if (!degraded && (n_partial || n_dead)) {
		error(
"non-SYSTEM chunks with missing stripes exist; re-run with --degraded "
"(data on missing devices is unreadable but structure is preserved)");
		goto fail;
	}

	/* place the new SYSTEM chunk: prefer DUP, fall back to single */
	sys_chunk.flags = BTRFS_BLOCK_GROUP_SYSTEM | BTRFS_BLOCK_GROUP_DUP;
	sys_chunk.nstripes = 2;
	ret = icx_find_gap(&ctx, 2 * sys_size, &sys_devid, &sys_phys);
	if (ret == -ENOSPC) {
		warning("no %llu MiB gap for SYSTEM|DUP, trying single copy",
			2 * sys_size >> 20);
		sys_chunk.flags = BTRFS_BLOCK_GROUP_SYSTEM;
		sys_chunk.nstripes = 1;
		ret = icx_find_gap(&ctx, sys_size, &sys_devid, &sys_phys);
	}
	if (ret) {
		error("no free space on any present device for a new SYSTEM chunk");
		goto fail;
	}
	for (i = 0; i < ctx.nchunks; i++) {
		u64 end = ctx.chunks[i].logical + ctx.chunks[i].length;

		if (end > sys_logical)
			sys_logical = end;
	}
	sys_logical = round_up(sys_logical, SZ_1G);
	sys_chunk.logical = sys_logical;
	sys_chunk.length = sys_size;
	for (i = 0; i < sys_chunk.nstripes; i++) {
		sys_chunk.s[i].devid = sys_devid;
		sys_chunk.s[i].physical = sys_phys + i * sys_size;
	}
	printf("new SYSTEM%s chunk: logical %llu -> devid %llu physical %llu (%llu MiB%s)\n",
	       sys_chunk.nstripes == 2 ? "|DUP" : "", sys_logical, sys_devid,
	       sys_phys, sys_size >> 20, sys_chunk.nstripes == 2 ? " x2" : "");

	if (!yes) {
		printf("\nDRY RUN: no changes written.  Re-run with -y to rebuild "
		       "the chunk tree of %s.\n", path);
		goto fail;	/* not a failure; shares cleanup */
	}

	/* in-memory mapping for the new system chunk so writes can route */
	{
		struct map_lookup *map;
		struct btrfs_device *sdev = btrfs_find_device(fs_info,
						sys_devid, NULL, NULL);

		map = kmalloc(btrfs_map_lookup_size(sys_chunk.nstripes), GFP_NOFS);
		if (!map)
			goto fail;
		map->ce.start = sys_logical;
		map->ce.size = sys_size;
		map->num_stripes = sys_chunk.nstripes;
		map->io_width = BTRFS_STRIPE_LEN;
		map->io_align = BTRFS_STRIPE_LEN;
		map->sector_size = fs_info->sectorsize;
		map->stripe_len = BTRFS_STRIPE_LEN;
		map->type = sys_chunk.flags;
		map->sub_stripes = 1;
		for (i = 0; i < sys_chunk.nstripes; i++) {
			map->stripes[i].physical = sys_chunk.s[i].physical;
			map->stripes[i].dev = sdev;
		}
		ret = insert_cache_extent(&fs_info->mapping_tree.cache_tree,
					  &map->ce);
		if (ret) {
			error("insert new system chunk mapping: %d", ret);
			goto fail;
		}
	}

	/* neutralize erased/dead block group caches so the allocator avoids them */
	for (i = 0; i < ctx.nchunks; i++) {
		struct icx_chunk *c = &ctx.chunks[i];

		if (icx_chunk_erased(c) || c->nmissing) {
			struct btrfs_block_group *bg =
				btrfs_lookup_block_group(fs_info, c->logical);

			if (bg)
				bg->used = bg->length;
		}
	}

	trans = btrfs_start_transaction(fs_info->tree_root, 1);
	if (IS_ERR(trans)) {
		error("cannot start transaction");
		goto fail;
	}

	/* block group (cache + item + free space) for the new system chunk */
	ret = btrfs_make_block_group(trans, fs_info, 0, sys_chunk.flags,
				     sys_logical, sys_size);
	if (ret) {
		error("make block group: %d", ret);
		goto fail;
	}

	/* resurrect the chunk root (injected open leaves it NULL) */
	if (!fs_info->chunk_root) {
		chunk_root = kzalloc(sizeof(*chunk_root), GFP_NOFS);
		if (!chunk_root)
			goto fail;
		btrfs_setup_root(chunk_root, fs_info,
				 BTRFS_CHUNK_TREE_OBJECTID);
		fs_info->chunk_root = chunk_root;
	}
	chunk_root = fs_info->chunk_root;

	btrfs_set_disk_key_objectid(&disk_key, BTRFS_DEV_ITEMS_OBJECTID);
	btrfs_set_disk_key_type(&disk_key, BTRFS_DEV_ITEM_KEY);
	btrfs_set_disk_key_offset(&disk_key, 1);
	cow = btrfs_alloc_tree_block(trans, chunk_root, 0,
				     BTRFS_CHUNK_TREE_OBJECTID, &disk_key, 0,
				     0, 0, BTRFS_NESTING_NORMAL);
	if (IS_ERR(cow)) {
		error("cannot allocate chunk root block: %ld", PTR_ERR(cow));
		goto fail;
	}
	btrfs_set_header_bytenr(cow, cow->start);
	btrfs_set_header_generation(cow, trans->transid);
	btrfs_set_header_nritems(cow, 0);
	btrfs_set_header_level(cow, 0);
	btrfs_set_header_backref_rev(cow, BTRFS_MIXED_BACKREF_REV);
	btrfs_set_header_owner(cow, BTRFS_CHUNK_TREE_OBJECTID);
	write_extent_buffer(cow, fs_info->fs_devices->metadata_uuid,
			    btrfs_header_fsid(), BTRFS_FSID_SIZE);
	write_extent_buffer(cow, fs_info->chunk_tree_uuid,
			    btrfs_header_chunk_tree_uuid(cow),
			    BTRFS_UUID_SIZE);
	chunk_root->node = cow;
	btrfs_mark_buffer_dirty(cow);
	printf("new chunk root at logical %llu\n", cow->start);

	/* device items */
	for (i = 0; i < ctx.ndevids; i++) {
		u64 devid = ctx.devids[i];
		int present = icx_dev_present(fs_info, devid);
		u64 tb = 0, used = 0;
		u8 uuid[BTRFS_UUID_SIZE] = { 0 };
		struct btrfs_device *dev;

		if (!present && !degraded)
			continue;	/* clean mode drops missing devices */

		/* bytes_used = sum of surviving dev extents on this device */
		for (j = 0; j < ctx.nchunks; j++) {
			struct icx_chunk *c = &ctx.chunks[j];
			int k;

			if (icx_chunk_erased(c))
				continue;
			for (k = 0; k < c->nstripes; k++)
				if (c->s[k].devid == devid) {
					used += c->length;
					if (c->s[k].physical + c->length > tb)
						tb = c->s[k].physical + c->length;
				}
		}
		dev = btrfs_find_device(fs_info, devid, NULL, NULL);
		if (present) {
			if (dev->total_bytes)
				tb = dev->total_bytes;
			memcpy(uuid, dev->uuid, BTRFS_UUID_SIZE);
			total_present += tb;
			if (devid == sys_devid) {
				used += sys_chunk.nstripes * sys_size;
				dev->bytes_used = used;
			}
		} else if (dev) {
			memcpy(uuid, dev->uuid, BTRFS_UUID_SIZE);
		}

		ret = icx_insert_dev_item(trans, chunk_root, devid, tb, used,
					  uuid, first_item, fs_info);
		if (ret) {
			error("insert dev item %llu: %d", devid, ret);
			goto fail;
		}
		first_item = 0;
	}

	/* chunk items: the new system chunk + every surviving chunk */
	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.type = BTRFS_CHUNK_ITEM_KEY;

	icx_fill_chunk_item(fs_info, chunk, &sys_chunk);
	key.offset = sys_chunk.logical;
	ret = btrfs_insert_item(trans, chunk_root, &key, chunk,
				btrfs_chunk_item_size(sys_chunk.nstripes));
	if (ret) {
		error("insert system chunk item: %d", ret);
		goto fail;
	}

	{
		int inserted = 1;

		for (i = 0; i < ctx.nchunks; i++) {
			struct icx_chunk *c = &ctx.chunks[i];

			if (icx_chunk_erased(c))
				continue;
			if (!degraded && c->nmissing)
				continue;
			icx_fill_chunk_item(fs_info, chunk, c);
			key.offset = c->logical;
			ret = btrfs_insert_item(trans, chunk_root, &key, chunk,
					btrfs_chunk_item_size(c->nstripes));
			if (ret) {
				error("insert chunk item %llu: %d",
				      c->logical, ret);
				goto fail;
			}
			inserted++;
		}
		printf("inserted %d chunk items\n", inserted);
	}

	/* dev extents for the new system chunk stripes */
	{
		struct btrfs_device *sdev = btrfs_find_device(fs_info,
						sys_devid, NULL, NULL);

		sdev->dev_root = fs_info->dev_root;
		for (i = 0; i < sys_chunk.nstripes; i++) {
			ret = btrfs_insert_dev_extent(trans, sdev,
					sys_chunk.logical, sys_size,
					sys_chunk.s[i].physical);
			if (ret) {
				error("insert dev extent %d: %d", i, ret);
				goto fail;
			}
		}
	}

	/*
	 * Erase old SYSTEM chunks: their dev extents, extent items (the old
	 * chunk tree blocks) and block group items.  In clean mode, also
	 * erase dev extents referencing dropped devices.
	 */
	for (i = 0; i < ctx.nchunks; i++) {
		struct icx_chunk *c = &ctx.chunks[i];
		struct btrfs_root *eroot;
		struct btrfs_path dpath = { 0 };
		struct btrfs_key found;
		int deleted = 0;

		if (!icx_chunk_erased(c))
			continue;

		/* its dev extents */
		for (j = 0; j < c->nstripes; j++) {
			struct btrfs_path xpath = { 0 };

			key.objectid = c->s[j].devid;
			key.type = BTRFS_DEV_EXTENT_KEY;
			key.offset = c->s[j].physical;
			ret = btrfs_search_slot(trans, fs_info->dev_root, &key,
						&xpath, -1, 1);
			if (ret == 0) {
				btrfs_del_item(trans, fs_info->dev_root, &xpath);
				deleted++;
			}
			btrfs_release_path(&xpath);
		}

		/* its extent items and block group item */
		eroot = btrfs_extent_root(fs_info, c->logical);
		while (1) {
			key.objectid = c->logical;
			key.type = 0;
			key.offset = 0;
			ret = btrfs_search_slot(trans, eroot, &key, &dpath, -1, 1);
			if (ret < 0)
				break;
			btrfs_item_key_to_cpu(dpath.nodes[0], &found,
					      dpath.slots[0]);
			if (found.objectid >= c->logical + c->length &&
			    !(found.objectid == c->logical &&
			      found.type == BTRFS_BLOCK_GROUP_ITEM_KEY)) {
				btrfs_release_path(&dpath);
				break;
			}
			if (found.type == BTRFS_METADATA_ITEM_KEY)
				dead_bytes += fs_info->nodesize;
			else if (found.type == BTRFS_EXTENT_ITEM_KEY)
				dead_bytes += found.offset;
			ret = btrfs_del_item(trans, eroot, &dpath);
			btrfs_release_path(&dpath);
			if (ret)
				break;
			deleted++;
		}
		printf("erased chunk %llu: %d items removed\n",
		       c->logical, deleted);
	}

	/* dev stats of dropped devices (clean mode) */
	if (!degraded) {
		for (i = 0; i < ctx.ndevids; i++) {
			struct btrfs_path spath = { 0 };

			if (icx_dev_present(fs_info, ctx.devids[i]))
				continue;
			key.objectid = BTRFS_DEV_STATS_OBJECTID;
			key.type = BTRFS_PERSISTENT_ITEM_KEY;
			key.offset = ctx.devids[i];
			ret = btrfs_search_slot(trans, fs_info->dev_root, &key,
						&spath, -1, 1);
			if (ret == 0) {
				btrfs_del_item(trans, fs_info->dev_root, &spath);
				printf("removed dev stats of dropped devid %llu\n",
				       ctx.devids[i]);
			}
			btrfs_release_path(&spath);
		}
	}

	/* superblock */
	if (!degraded) {
		int npresent = 0;

		for (i = 0; i < ctx.ndevids; i++)
			if (icx_dev_present(fs_info, ctx.devids[i]))
				npresent++;
		btrfs_set_super_num_devices(sb, npresent);
		btrfs_set_super_total_bytes(sb, total_present);
	}
	btrfs_set_super_bytes_used(sb,
			btrfs_super_bytes_used(sb) - dead_bytes);
	if (btrfs_stack_device_id(&sb->dev_item) == sys_devid) {
		struct btrfs_device *sdev = btrfs_find_device(fs_info,
						sys_devid, NULL, NULL);

		btrfs_set_stack_device_bytes_used(&sb->dev_item,
						  sdev->bytes_used);
	}
	/* invalidate the free space tree so the kernel rebuilds it */
	btrfs_set_super_compat_ro_flags(sb,
		btrfs_super_compat_ro_flags(sb) &
		~BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID);

	/* sys_chunk_array: just the new system chunk */
	btrfs_set_super_sys_array_size(sb, 0);
	icx_fill_chunk_item(fs_info, chunk, &sys_chunk);
	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = sys_chunk.logical;
	ret = btrfs_add_system_chunk(fs_info, &key, chunk,
				btrfs_chunk_item_size(sys_chunk.nstripes));
	if (ret) {
		error("rebuild sys array: %d", ret);
		goto fail;
	}

	ret = btrfs_commit_transaction(trans, fs_info->tree_root);
	if (ret) {
		error("commit failed: %d", ret);
		goto fail;
	}
	printf("chunk tree rebuilt and committed successfully\n");
	if (degraded)
		printf("mount with -o degraded\n");

	free(ctx.chunks);
	close_ctree(fs_info->tree_root);
	return 0;
fail:
	free(ctx.chunks);
	close_ctree(fs_info->tree_root);
	return 1;
}
