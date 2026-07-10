/*
 * RECOVERY HACK: rebuild the chunk tree from the (intact) device tree and
 * extent tree, for a filesystem whose SYSTEM chunk was destroyed (e.g. it
 * lived on a volatile device that vanished mid-balance).
 *
 * Requires the fs to be opened with BTRFS_CHUNK_MAP injection so the device
 * and extent trees are readable without a chunk tree.
 *
 * End state: clean single-device fs.  A new SYSTEM|DUP chunk is allocated in
 * free device space, a fresh chunk tree written there, the superblock's
 * sys_chunk_array/num_devices/total_bytes updated, and all traces of the
 * ghost device (dev extents, dev stats, the dead SYSTEM block group) removed.
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

#define ICX_MAX_CHUNKS 4096
#define ICX_SYS_CHUNK_LEN SZ_32M

struct icx_chunk {
	u64 logical;
	u64 length;
	u64 flags;
	int nstripes;
	u64 stripe_phys[2];	/* devid 1 only */
	int have_bg;
};

static struct icx_chunk icx_chunks[ICX_MAX_CHUNKS];
static int icx_nchunks;

/* dead chunks: block group exists but no stripes on present devices */
static u64 icx_dead_logical[16];
static u64 icx_dead_length[16];
static int icx_ndead;

static struct icx_chunk *icx_find(u64 logical)
{
	int i;

	for (i = 0; i < icx_nchunks; i++)
		if (icx_chunks[i].logical == logical)
			return &icx_chunks[i];
	return NULL;
}

/* collect (devid 1) dev extents keyed by chunk logical; count ghost extents */
static int icx_harvest_dev_extents(struct btrfs_fs_info *fs_info,
				   u64 *ghost_keys, int *n_ghost,
				   u64 *dev1_used)
{
	struct btrfs_root *dev_root = fs_info->dev_root;
	struct btrfs_path path = { 0 };
	struct btrfs_key key, found;
	int ret;

	*dev1_used = 0;
	*n_ghost = 0;
	key.objectid = 0;
	key.type = 0;
	key.offset = 0;
	ret = btrfs_search_slot(NULL, dev_root, &key, &path, 0, 0);
	if (ret < 0)
		return ret;
	while (1) {
		btrfs_item_key_to_cpu(path.nodes[0], &found, path.slots[0]);
		if (found.type == BTRFS_DEV_EXTENT_KEY) {
			struct btrfs_dev_extent *dext;
			u64 logical, len;

			dext = btrfs_item_ptr(path.nodes[0], path.slots[0],
					      struct btrfs_dev_extent);
			logical = btrfs_dev_extent_chunk_offset(path.nodes[0], dext);
			len = btrfs_dev_extent_length(path.nodes[0], dext);
			if (found.objectid == 1) {
				struct icx_chunk *c = icx_find(logical);

				if (!c) {
					if (icx_nchunks >= ICX_MAX_CHUNKS) {
						error("too many chunks");
						btrfs_release_path(&path);
						return -EOVERFLOW;
					}
					c = &icx_chunks[icx_nchunks++];
					c->logical = logical;
					c->length = len;
					c->nstripes = 0;
					c->flags = 0;
					c->have_bg = 0;
				}
				if (c->nstripes < 2)
					c->stripe_phys[c->nstripes] = found.offset;
				c->nstripes++;
				*dev1_used += len;
			} else {
				/* ghost device extent: remember (devid,physical) */
				ghost_keys[(*n_ghost) * 2] = found.objectid;
				ghost_keys[(*n_ghost) * 2 + 1] = found.offset;
				(*n_ghost)++;
				printf("ghost dev extent: devid %llu physical %llu chunk %llu\n",
				       found.objectid, found.offset, logical);
			}
		}
		ret = btrfs_next_item(dev_root, &path);
		if (ret)
			break;
	}
	btrfs_release_path(&path);
	return 0;
}

/* read block group items to learn each chunk's flags */
static int icx_harvest_block_groups(struct btrfs_fs_info *fs_info)
{
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
			c = icx_find(found.objectid);
			if (c) {
				c->flags = btrfs_stack_block_group_flags(&bgi);
				c->have_bg = 1;
				if (c->length != found.offset)
					warning("chunk %llu length mismatch bg %llu vs devext %llu",
						found.objectid, found.offset, c->length);
			} else {
				if (icx_ndead < 16) {
					icx_dead_logical[icx_ndead] = found.objectid;
					icx_dead_length[icx_ndead] = found.offset;
					icx_ndead++;
				}
				printf("dead block group (no stripes): logical %llu len %llu flags 0x%llx\n",
				       found.objectid, found.offset,
				       btrfs_stack_block_group_flags(&bgi));
			}
		}
		ret = btrfs_next_item(root, &path);
		if (ret)
			break;
	}
	btrfs_release_path(&path);
	return 0;
}

/* find a free physical gap of `need` bytes from harvested dev1 extents */
static int icx_find_gap(struct btrfs_fs_info *fs_info, u64 need, u64 *start)
{
	u64 phys[ICX_MAX_CHUNKS * 2][2];
	int n = 0, i, j;
	u64 pos = SZ_1M;
	u64 dev_total = btrfs_super_total_bytes(fs_info->super_copy);

	for (i = 0; i < icx_nchunks; i++)
		for (j = 0; j < icx_chunks[i].nstripes && j < 2; j++) {
			phys[n][0] = icx_chunks[i].stripe_phys[j];
			phys[n][1] = icx_chunks[i].length;
			n++;
		}
	/* insertion sort by physical */
	for (i = 1; i < n; i++) {
		u64 p0 = phys[i][0], p1 = phys[i][1];

		for (j = i - 1; j >= 0 && phys[j][0] > p0; j--) {
			phys[j + 1][0] = phys[j][0];
			phys[j + 1][1] = phys[j][1];
		}
		phys[j + 1][0] = p0;
		phys[j + 1][1] = p1;
	}
	for (i = 0; i < n; i++) {
		if (phys[i][0] >= pos + need) {
			*start = pos;
			return 0;
		}
		if (phys[i][0] + phys[i][1] > pos)
			pos = phys[i][0] + phys[i][1];
	}
	if (dev_total >= pos + need) {
		*start = pos;
		return 0;
	}
	return -ENOSPC;
}

static void icx_fill_chunk_item(struct btrfs_chunk *chunk,
				struct btrfs_stripe *stripes,
				struct icx_chunk *c,
				struct btrfs_fs_info *fs_info,
				const u8 *dev_uuid)
{
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
		btrfs_set_stack_stripe_devid(&stripes[i], 1);
		btrfs_set_stack_stripe_offset(&stripes[i], c->stripe_phys[i]);
		memcpy(stripes[i].dev_uuid, dev_uuid, BTRFS_UUID_SIZE);
	}
}

int btrfs_rescue_inject_chunk_tree(const char *path, int yes)
{
	struct btrfs_fs_info *fs_info;
	struct btrfs_super_block *sb;
	struct btrfs_trans_handle *trans;
	struct btrfs_root *chunk_root;
	struct btrfs_device *dev1 = NULL, *dev_iter;
	struct btrfs_block_group *bg;
	struct open_ctree_args oca = { 0 };
	struct extent_buffer *cow;
	struct btrfs_disk_key disk_key;
	struct btrfs_key key;
	u64 ghost_keys[64];
	int n_ghost = 0;
	u64 dev1_used = 0;
	u64 sys_logical = 0, sys_phys = 0;
	u8 dev_uuid[BTRFS_UUID_SIZE];
	struct icx_chunk sys_chunk;
	/*
	 * chunk item buffer: struct btrfs_chunk embeds the FIRST stripe as
	 * its `stripe` member; additional stripes follow it directly.
	 */
	u8 chunk_buf[sizeof(struct btrfs_chunk) + 2 * sizeof(struct btrfs_stripe)];
	struct btrfs_chunk *chunk = (struct btrfs_chunk *)chunk_buf;
	struct btrfs_stripe *stripes = &chunk->stripe;
	int i, ret;

	if (!getenv("BTRFS_CHUNK_MAP")) {
		error("inject-chunk-tree requires BTRFS_CHUNK_MAP to be set");
		return 1;
	}

	oca.filename = path;
	oca.flags = OPEN_CTREE_WRITES | OPEN_CTREE_PARTIAL |
		    OPEN_CTREE_ALLOW_TRANSID_MISMATCH;
	fs_info = open_ctree_fs_info(&oca);
	if (!fs_info) {
		error("cannot open filesystem");
		return 1;
	}
	sb = fs_info->super_copy;

	if (!fs_info->chunk_map_injected) {
		error("filesystem was not opened via chunk map injection");
		goto fail;
	}
	/* the tree checker rejects the transiently-empty new chunk root */
	fs_info->suppress_check_block_errors = 1;

	/* recover the real chunk tree uuid from the dev tree root header */
	if (fs_info->dev_root && fs_info->dev_root->node)
		read_extent_buffer(fs_info->dev_root->node,
				   fs_info->chunk_tree_uuid,
				   btrfs_header_chunk_tree_uuid(fs_info->dev_root->node),
				   BTRFS_UUID_SIZE);

	/* injected open nulls chunk_root; resurrect an empty root struct */
	if (!fs_info->chunk_root) {
		chunk_root = kzalloc(sizeof(*chunk_root), GFP_NOFS);
		if (!chunk_root)
			goto fail;
		btrfs_setup_root(chunk_root, fs_info,
				 BTRFS_CHUNK_TREE_OBJECTID);
		fs_info->chunk_root = chunk_root;
	}
	chunk_root = fs_info->chunk_root;
	if (!fs_info->dev_root || !fs_info->dev_root->node) {
		error("device tree not readable");
		goto fail;
	}

	/* the present device */
	list_for_each_entry(dev_iter, &fs_info->fs_devices->devices, dev_list) {
		if (dev_iter->devid == 1)
			dev1 = dev_iter;
	}
	if (!dev1 || dev1->fd < 0) {
		error("device 1 not present/open");
		goto fail;
	}
	/* injected open skips reading dev items: fill from superblock */
	dev1->total_bytes = btrfs_stack_device_total_bytes(&sb->dev_item);
	dev1->io_align = btrfs_stack_device_io_align(&sb->dev_item);
	dev1->io_width = btrfs_stack_device_io_width(&sb->dev_item);
	dev1->sector_size = btrfs_stack_device_sector_size(&sb->dev_item);
	dev1->type = btrfs_stack_device_type(&sb->dev_item);
	memcpy(dev_uuid, sb->dev_item.uuid, BTRFS_UUID_SIZE);
	memcpy(dev1->uuid, sb->dev_item.uuid, BTRFS_UUID_SIZE);
	dev1->dev_root = fs_info->dev_root;

	printf("harvesting device extents...\n");
	ret = icx_harvest_dev_extents(fs_info, ghost_keys, &n_ghost, &dev1_used);
	if (ret) {
		error("harvest dev extents: %d", ret);
		goto fail;
	}
	printf("harvesting block groups...\n");
	ret = icx_harvest_block_groups(fs_info);
	if (ret) {
		error("harvest block groups: %d", ret);
		goto fail;
	}
	printf("chunks: %d alive, %d dead, %d ghost dev extents\n",
	       icx_nchunks, icx_ndead, n_ghost);

	for (i = 0; i < icx_nchunks; i++) {
		if (!icx_chunks[i].have_bg) {
			error("chunk %llu has dev extent but no block group; aborting",
			      icx_chunks[i].logical);
			goto fail;
		}
		if (icx_chunks[i].nstripes > 2) {
			error("chunk %llu has %d stripes (>2 unsupported)",
			      icx_chunks[i].logical, icx_chunks[i].nstripes);
			goto fail;
		}
	}

	/* pick placement for the new SYSTEM|DUP chunk */
	ret = icx_find_gap(fs_info, 2 * ICX_SYS_CHUNK_LEN, &sys_phys);
	if (ret) {
		error("no free 64MiB gap on device for new system chunk");
		goto fail;
	}
	for (i = 0; i < icx_nchunks; i++) {
		u64 end = icx_chunks[i].logical + icx_chunks[i].length;

		if (end > sys_logical)
			sys_logical = end;
	}
	for (i = 0; i < icx_ndead; i++) {
		u64 end = icx_dead_logical[i] + icx_dead_length[i];

		if (end > sys_logical)
			sys_logical = end;
	}
	sys_logical = round_up(sys_logical, SZ_1G);
	printf("new SYSTEM|DUP chunk: logical %llu physical %llu+%u x2\n",
	       sys_logical, sys_phys, ICX_SYS_CHUNK_LEN);

	if (!yes) {
		printf("This will REWRITE the chunk tree and superblocks of %s.\n", path);
		printf("Re-run with -y to confirm.\n");
		goto fail;
	}

	/* in-memory mapping for the new system chunk so writes can route */
	{
		struct map_lookup *map;

		map = kmalloc(btrfs_map_lookup_size(2), GFP_NOFS);
		if (!map)
			goto fail;
		map->ce.start = sys_logical;
		map->ce.size = ICX_SYS_CHUNK_LEN;
		map->num_stripes = 2;
		map->io_width = BTRFS_STRIPE_LEN;
		map->io_align = BTRFS_STRIPE_LEN;
		map->sector_size = fs_info->sectorsize;
		map->stripe_len = BTRFS_STRIPE_LEN;
		map->type = BTRFS_BLOCK_GROUP_SYSTEM | BTRFS_BLOCK_GROUP_DUP;
		map->sub_stripes = 1;
		for (i = 0; i < 2; i++) {
			map->stripes[i].physical = sys_phys + i * ICX_SYS_CHUNK_LEN;
			map->stripes[i].dev = dev1;
		}
		ret = insert_cache_extent(&fs_info->mapping_tree.cache_tree,
					  &map->ce);
		if (ret) {
			error("insert new system chunk mapping: %d", ret);
			goto fail;
		}
	}

	/* neutralize dead block group caches so the allocator avoids them */
	for (i = 0; i < icx_ndead; i++) {
		bg = btrfs_lookup_block_group(fs_info, icx_dead_logical[i]);
		if (bg) {
			bg->used = bg->length;
			printf("marked dead bg %llu full\n", icx_dead_logical[i]);
		}
	}

	trans = btrfs_start_transaction(fs_info->tree_root, 1);
	if (IS_ERR(trans)) {
		error("cannot start transaction");
		goto fail;
	}

	/* block group (cache + item + free space) for the new system chunk */
	ret = btrfs_make_block_group(trans, fs_info, 0,
				     BTRFS_BLOCK_GROUP_SYSTEM | BTRFS_BLOCK_GROUP_DUP,
				     sys_logical, ICX_SYS_CHUNK_LEN);
	if (ret) {
		error("make block group: %d", ret);
		goto fail;
	}

	/* empty chunk root */
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

	/* DEV_ITEM for devid 1 */
	dev1->bytes_used = dev1_used + 2 * ICX_SYS_CHUNK_LEN;
	{
		struct btrfs_dev_item dev_item;

		key.objectid = BTRFS_DEV_ITEMS_OBJECTID;
		key.type = BTRFS_DEV_ITEM_KEY;
		key.offset = 1;
		memset(&dev_item, 0, sizeof(dev_item));
		btrfs_set_stack_device_generation(&dev_item, 0);
		btrfs_set_stack_device_type(&dev_item, dev1->type);
		btrfs_set_stack_device_id(&dev_item, 1);
		btrfs_set_stack_device_total_bytes(&dev_item, dev1->total_bytes);
		btrfs_set_stack_device_bytes_used(&dev_item, dev1->bytes_used);
		btrfs_set_stack_device_io_align(&dev_item, dev1->io_align);
		btrfs_set_stack_device_io_width(&dev_item, dev1->io_width);
		btrfs_set_stack_device_sector_size(&dev_item, dev1->sector_size);
		memcpy(dev_item.uuid, dev_uuid, BTRFS_UUID_SIZE);
		memcpy(dev_item.fsid, fs_info->fs_devices->metadata_uuid,
		       BTRFS_FSID_SIZE);
		/*
		 * First insertion into the still-empty chunk root: the tree
		 * checker rejects empty chunk-tree leaves, so bypass it via
		 * a manual path with skip_check_block.
		 */
		{
			struct btrfs_path ipath = { 0 };

			ipath.skip_check_block = 1;
			ret = btrfs_insert_empty_item(trans, chunk_root,
						      &ipath, &key,
						      sizeof(dev_item));
			if (ret) {
				error("insert dev item: %d", ret);
				btrfs_release_path(&ipath);
				goto fail;
			}
			write_extent_buffer(ipath.nodes[0], &dev_item,
				btrfs_item_ptr_offset(ipath.nodes[0],
						      ipath.slots[0]),
				sizeof(dev_item));
			btrfs_mark_buffer_dirty(ipath.nodes[0]);
			btrfs_release_path(&ipath);
		}
	}

	/* chunk items: new system chunk + all harvested chunks */
	memset(&sys_chunk, 0, sizeof(sys_chunk));
	sys_chunk.logical = sys_logical;
	sys_chunk.length = ICX_SYS_CHUNK_LEN;
	sys_chunk.flags = BTRFS_BLOCK_GROUP_SYSTEM | BTRFS_BLOCK_GROUP_DUP;
	sys_chunk.nstripes = 2;
	sys_chunk.stripe_phys[0] = sys_phys;
	sys_chunk.stripe_phys[1] = sys_phys + ICX_SYS_CHUNK_LEN;

	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.type = BTRFS_CHUNK_ITEM_KEY;

	icx_fill_chunk_item(chunk, stripes, &sys_chunk, fs_info, dev_uuid);
	key.offset = sys_logical;
	ret = btrfs_insert_item(trans, chunk_root, &key, chunk,
				btrfs_chunk_item_size(2));
	if (ret) {
		error("insert system chunk item: %d", ret);
		goto fail;
	}

	for (i = 0; i < icx_nchunks; i++) {
		icx_fill_chunk_item(chunk, stripes, &icx_chunks[i], fs_info,
				    dev_uuid);
		key.offset = icx_chunks[i].logical;
		ret = btrfs_insert_item(trans, chunk_root, &key, chunk,
				btrfs_chunk_item_size(icx_chunks[i].nstripes));
		if (ret) {
			error("insert chunk item %llu: %d",
			      icx_chunks[i].logical, ret);
			goto fail;
		}
	}
	printf("inserted %d chunk items\n", icx_nchunks + 1);

	/* dev extents for the new system chunk stripes */
	for (i = 0; i < 2; i++) {
		ret = btrfs_insert_dev_extent(trans, dev1, sys_logical,
					      ICX_SYS_CHUNK_LEN,
					      sys_phys + i * ICX_SYS_CHUNK_LEN);
		if (ret) {
			error("insert dev extent %d: %d", i, ret);
			goto fail;
		}
	}

	/* delete ghost dev extents */
	for (i = 0; i < n_ghost; i++) {
		struct btrfs_path path = { 0 };

		key.objectid = ghost_keys[i * 2];
		key.type = BTRFS_DEV_EXTENT_KEY;
		key.offset = ghost_keys[i * 2 + 1];
		ret = btrfs_search_slot(trans, fs_info->dev_root, &key, &path,
					-1, 1);
		if (ret == 0) {
			ret = btrfs_del_item(trans, fs_info->dev_root, &path);
			printf("deleted ghost dev extent devid %llu phys %llu: %d\n",
			       key.objectid, key.offset, ret);
		}
		btrfs_release_path(&path);
	}

	/* delete ghost device stats */
	{
		struct btrfs_path path = { 0 };

		key.objectid = BTRFS_DEV_STATS_OBJECTID;
		key.type = BTRFS_PERSISTENT_ITEM_KEY;
		key.offset = 2;
		ret = btrfs_search_slot(trans, fs_info->dev_root, &key, &path,
					-1, 1);
		if (ret == 0) {
			ret = btrfs_del_item(trans, fs_info->dev_root, &path);
			printf("deleted ghost dev stats: %d\n", ret);
		}
		btrfs_release_path(&path);
	}

	/* delete dead block groups + any extent items inside them */
	for (i = 0; i < icx_ndead; i++) {
		struct btrfs_root *eroot =
			btrfs_extent_root(fs_info, icx_dead_logical[i]);
		struct btrfs_path path = { 0 };
		struct btrfs_key found;
		int deleted = 0;

		/* extent items within the dead range */
		while (1) {
			key.objectid = icx_dead_logical[i];
			key.type = 0;
			key.offset = 0;
			ret = btrfs_search_slot(trans, eroot, &key, &path, -1, 1);
			if (ret < 0)
				break;
			btrfs_item_key_to_cpu(path.nodes[0], &found,
					      path.slots[0]);
			if (found.objectid >= icx_dead_logical[i] + icx_dead_length[i] ||
			    (found.objectid == icx_dead_logical[i] &&
			     found.type == BTRFS_BLOCK_GROUP_ITEM_KEY)) {
				/* handle BG item separately below */
				if (found.type == BTRFS_BLOCK_GROUP_ITEM_KEY &&
				    found.objectid == icx_dead_logical[i]) {
					ret = btrfs_del_item(trans, eroot, &path);
					deleted++;
					btrfs_release_path(&path);
					continue;
				}
				btrfs_release_path(&path);
				break;
			}
			ret = btrfs_del_item(trans, eroot, &path);
			btrfs_release_path(&path);
			if (ret)
				break;
			deleted++;
		}
		printf("dead bg %llu: deleted %d items\n",
		       icx_dead_logical[i], deleted);
	}

	/* superblock updates */
	btrfs_set_super_num_devices(sb, 1);
	btrfs_set_super_total_bytes(sb, dev1->total_bytes);
	btrfs_set_stack_device_bytes_used(&sb->dev_item, dev1->bytes_used);
	/* invalidate free space tree so the kernel rebuilds it */
	btrfs_set_super_compat_ro_flags(sb,
		btrfs_super_compat_ro_flags(sb) &
		~BTRFS_FEATURE_COMPAT_RO_FREE_SPACE_TREE_VALID);

	/* rebuild sys_chunk_array with only the new system chunk */
	btrfs_set_super_sys_array_size(sb, 0);
	icx_fill_chunk_item(chunk, stripes, &sys_chunk, fs_info, dev_uuid);
	key.objectid = BTRFS_FIRST_CHUNK_TREE_OBJECTID;
	key.type = BTRFS_CHUNK_ITEM_KEY;
	key.offset = sys_logical;
	ret = btrfs_add_system_chunk(fs_info, &key, chunk,
				     btrfs_chunk_item_size(2));
	if (ret) {
		error("rebuild sys array: %d", ret);
		goto fail;
	}

	ret = btrfs_commit_transaction(trans, fs_info->tree_root);
	if (ret) {
		error("commit failed: %d", ret);
		goto fail;
	}
	printf("chunk tree injected and committed successfully\n");

	close_ctree(fs_info->tree_root);
	return 0;
fail:
	close_ctree(fs_info->tree_root);
	return 1;
}
