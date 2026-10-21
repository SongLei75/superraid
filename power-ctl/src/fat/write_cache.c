// SPDX-License-Identifier: GPL-2.0
/* Temporary, per-mount FAT block contents. No backing-device replacement. */
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/capability.h>
#include <linux/highmem.h>
#include <linux/module.h>
#include <linux/slab.h>
#include "fat.h"

struct fat_cached_block {
	struct mutex lock;
	struct buffer_head *bh;
	void *data;
	bool valid;
	bool dirty;
};

void fat_write_cache_init(struct super_block *sb)
{
	struct fat_write_cache *cache = &MSDOS_SB(sb)->write_cache;

	mutex_init(&cache->control);
	init_rwsem(&cache->io_lock);
	xa_init(&cache->blocks);
}

bool fat_write_cache_active(struct super_block *sb)
{
	return READ_ONCE(MSDOS_SB(sb)->write_cache.state) != FAT_CACHE_OFF;
}

/* Caller holds io_lock. Entries live until successful close or unmount. */
static struct fat_cached_block *fat_cache_reserve(struct super_block *sb,
						struct buffer_head *bh)
{
	struct xarray *blocks = &MSDOS_SB(sb)->write_cache.blocks;
	struct fat_cached_block *entry, *old;

	entry = xa_load(blocks, bh->b_blocknr);
	if (entry)
		return entry;
	entry = kzalloc(sizeof(*entry), GFP_NOFS);
	if (!entry)
		return ERR_PTR(-ENOMEM);
	entry->data = kmalloc(sb->s_blocksize, GFP_NOFS);
	if (!entry->data) {
		kfree(entry);
		return ERR_PTR(-ENOMEM);
	}
	mutex_init(&entry->lock);
	get_bh(bh);
	entry->bh = bh;
	old = xa_cmpxchg(blocks, bh->b_blocknr, NULL, entry, GFP_NOFS);
	if (old) {
		brelse(bh);
		kfree(entry->data);
		kfree(entry);
		return xa_is_err(old) ? ERR_PTR(xa_err(old)) : old;
	}
	return entry;
}

/* Reserve before metadata is modified: dirty notification cannot fail. */
static struct buffer_head *fat_cache_hold_buffer(struct super_block *sb,
						struct buffer_head *bh)
{
	struct fat_write_cache *cache = &MSDOS_SB(sb)->write_cache;
	struct fat_cached_block *entry;

	if (!bh || !fat_write_cache_active(sb))
		return bh;
	down_read(&cache->io_lock);
	if (cache->state != FAT_CACHE_OFF) {
		entry = fat_cache_reserve(sb, bh);
		if (IS_ERR(entry)) {
			brelse(bh);
			bh = NULL;
		}
	}
	up_read(&cache->io_lock);
	return bh;
}

struct buffer_head *fat_cache_bread(struct super_block *sb, sector_t block)
{
	struct fat_write_cache *cache = &MSDOS_SB(sb)->write_cache;
	struct fat_cached_block *entry;
	struct buffer_head *bh;

	if (!fat_write_cache_active(sb))
		return sb_bread(sb, block);
	bh = sb_getblk(sb, block);
	if (!bh)
		return NULL;
	down_read(&cache->io_lock);
	entry = xa_load(&cache->blocks, block);
	if (entry) {
		mutex_lock(&entry->lock);
		if (entry->valid && !buffer_uptodate(bh)) {
			lock_buffer(bh);
			memcpy(bh->b_data, entry->data, sb->s_blocksize);
			set_buffer_uptodate(bh);
			unlock_buffer(bh);
		}
		mutex_unlock(&entry->lock);
	}
	up_read(&cache->io_lock);
	if (bh_read(bh, 0) < 0) {
		brelse(bh);
		return NULL;
	}
	return fat_cache_hold_buffer(sb, bh);
}

struct buffer_head *fat_cache_getblk(struct super_block *sb, sector_t block)
{
	return fat_cache_hold_buffer(sb, sb_getblk(sb, block));
}

/* entry->lock protects the submitted bytes against concurrent modification. */
static int fat_cache_submit(struct super_block *sb,
			    struct fat_cached_block *entry)
{
	struct bio *bio;
	int err;

	if (!fat_writable(sb))
		return -EROFS;
	bio = bio_alloc(sb->s_bdev, 1, REQ_OP_WRITE | REQ_SYNC, GFP_NOIO);
	bio->bi_iter.bi_sector = entry->bh->b_blocknr <<
				(sb->s_blocksize_bits - 9);
	bio_add_page(bio, virt_to_page(entry->data), sb->s_blocksize,
		     offset_in_page(entry->data));
	err = submit_bio_wait(bio);
	bio_put(bio);
	if (!err)
		entry->dirty = false;
	return err;
}

static void fat_cache_record_error(struct fat_write_cache *cache, int err)
{
	cmpxchg(&cache->error, 0, err);
	/* A failed close resumes caching; it does not fail APP writes. */
	cmpxchg(&cache->state, FAT_CACHE_DRAINING, FAT_CACHE_ON);
}

void fat_cache_mark_dirty(struct super_block *sb, struct buffer_head *bh,
			  struct inode *inode)
{
	struct fat_write_cache *cache = &MSDOS_SB(sb)->write_cache;
	struct fat_cached_block *entry;
	int err;

	down_read(&cache->io_lock);
	if (cache->state == FAT_CACHE_OFF) {
		if (inode)
			mark_buffer_dirty_inode(bh, inode);
		else
			mark_buffer_dirty(bh);
	} else if (cache->state != FAT_CACHE_STOPPED) {
		entry = xa_load(&cache->blocks, bh->b_blocknr);
		if (WARN_ON_ONCE(!entry)) {
			fat_cache_record_error(cache, -ENOMEM);
			goto out;
		}
		mutex_lock(&entry->lock);
		memcpy(entry->data, bh->b_data, sb->s_blocksize);
		entry->valid = entry->dirty = true;
		if (READ_ONCE(cache->state) == FAT_CACHE_DRAINING) {
			err = fat_cache_submit(sb, entry);
			if (err)
				fat_cache_record_error(cache, err);
		}
		mutex_unlock(&entry->lock);
	}
out:
	up_read(&cache->io_lock);
}
EXPORT_SYMBOL_GPL(fat_cache_mark_dirty);

int fat_cache_sync_buffer(struct super_block *sb, struct buffer_head *bh)
{
	/* Cached metadata buffers are deliberately never marked BH_Dirty. */
	if (fat_write_cache_active(sb))
		return 0;
	return sync_dirty_buffer(bh);
}
EXPORT_SYMBOL_GPL(fat_cache_sync_buffer);

int fat_cache_prepare_block(struct super_block *sb, struct buffer_head *bh,
			    bool create)
{
	struct fat_write_cache *cache = &MSDOS_SB(sb)->write_cache;
	struct fat_cached_block *entry;
	struct buffer_head *disk_bh;
	void *addr;
	int err = 0;

	if (!fat_write_cache_active(sb) || !buffer_mapped(bh))
		return 0;
	down_read(&cache->io_lock);
	if (cache->state == FAT_CACHE_OFF)
		goto out;
	entry = xa_load(&cache->blocks, bh->b_blocknr);
	if (!entry && create) {
		disk_bh = sb_getblk(sb, bh->b_blocknr);
		if (!disk_bh) {
			err = -ENOMEM;
			goto out;
		}
		entry = fat_cache_reserve(sb, disk_bh);
		brelse(disk_bh);
		if (IS_ERR(entry)) {
			err = PTR_ERR(entry);
			goto out;
		}
	}
	if (entry && bh->b_page && !buffer_new(bh) && !buffer_uptodate(bh)) {
		mutex_lock(&entry->lock);
		if (entry->valid) {
			addr = kmap_local_page(bh->b_page);
			memcpy(addr + bh_offset(bh), entry->data, sb->s_blocksize);
			kunmap_local(addr);
			set_buffer_uptodate(bh);
		}
		mutex_unlock(&entry->lock);
	}
out:
	up_read(&cache->io_lock);
	return err;
}

int fat_cache_write_block(struct super_block *sb, sector_t block,
			  struct page *page, unsigned int offset)
{
	struct buffer_head *bh;
	void *addr;
	int err;

	bh = fat_cache_getblk(sb, block);
	if (!bh)
		return -ENOMEM;
	lock_buffer(bh);
	addr = kmap_local_page(page);
	memcpy(bh->b_data, addr + offset, sb->s_blocksize);
	kunmap_local(addr);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);
	fat_cache_mark_dirty(sb, bh, NULL);
	err = fat_cache_sync_buffer(sb, bh);
	brelse(bh);
	return err;
}

static void fat_cache_clear(struct super_block *sb, bool discard)
{
	struct fat_write_cache *cache = &MSDOS_SB(sb)->write_cache;
	struct fat_cached_block *entry;
	unsigned long index;

	xa_for_each(&cache->blocks, index, entry) {
		xa_erase(&cache->blocks, index);
		if (discard && entry->valid)
			clear_buffer_uptodate(entry->bh);
		brelse(entry->bh);
		kfree(entry->data);
		kfree(entry);
	}
}

void fat_write_cache_destroy(struct super_block *sb)
{
	/* Unmount is not a request to commit this temporary cache. */
	fat_cache_clear(sb, true);
	xa_destroy(&MSDOS_SB(sb)->write_cache.blocks);
}

bool fat_write_cache_stop(struct super_block *sb)
{
	struct fat_write_cache *cache = &MSDOS_SB(sb)->write_cache;
	bool active;

	/* Do not wait for control: a close must yield to power-off. */
	down_write(&cache->io_lock);
	active = cache->state != FAT_CACHE_OFF;
	if (active)
		WRITE_ONCE(cache->state, FAT_CACHE_STOPPED);
	up_write(&cache->io_lock);
	return active;
}

static int fat_cache_enable(struct super_block *sb)
{
	struct fat_write_cache *cache = &MSDOS_SB(sb)->write_cache;
	int err;

	if (cache->state == FAT_CACHE_ON)
		return 0;
	/* Also establishes the boundary when enabling again on a live mount. */
	err = freeze_super(sb);
	if (err)
		return err;
	down_write(&cache->io_lock);
	if (!fat_writable(sb)) {
		err = -EROFS;
	} else {
		cache->error = 0;
		WRITE_ONCE(cache->state, FAT_CACHE_ON);
	}
	up_write(&cache->io_lock);
	if (thaw_super(sb) && !err)
		err = -EIO;
	return err;
}

static int fat_cache_disable(struct super_block *sb)
{
	struct fat_write_cache *cache = &MSDOS_SB(sb)->write_cache;
	struct fat_cached_block *entry;
	unsigned long index;
	int err = 0;

	down_write(&cache->io_lock);
	if (cache->state == FAT_CACHE_OFF) {
		up_write(&cache->io_lock);
		return 0;
	}
	cache->error = 0;
	WRITE_ONCE(cache->state, FAT_CACHE_DRAINING);
	up_write(&cache->io_lock);

	/* Bring pre-existing file pages and inode metadata into the block cache.
	 * New cache updates in DRAINING write through under their block lock.
	 */
	down_read(&sb->s_umount);
	err = sync_filesystem(sb);
	up_read(&sb->s_umount);
	if (err)
		fat_cache_record_error(cache, err);

	down_read(&cache->io_lock);
	xa_for_each(&cache->blocks, index, entry) {
		if (!fat_writable(sb) || READ_ONCE(cache->state) != FAT_CACHE_DRAINING)
			break;
		mutex_lock(&entry->lock);
		if (entry->dirty) {
			err = fat_cache_submit(sb, entry);
			if (err)
				fat_cache_record_error(cache, err);
		}
		mutex_unlock(&entry->lock);
		cond_resched();
	}
	up_read(&cache->io_lock);

	/* Only the final device flush/switch excludes new cache operations.
	 * Keep all entries until this succeeds, so a failed close is retryable.
	 */
	down_write(&cache->io_lock);
	if (!fat_writable(sb) || cache->state == FAT_CACHE_STOPPED) {
		err = -EROFS;
		WRITE_ONCE(cache->state, FAT_CACHE_STOPPED);
	} else {
		err = cache->error;
		if (!err)
			err = blkdev_issue_flush(sb->s_bdev);
		if (err) {
			WRITE_ONCE(cache->state, FAT_CACHE_ON);
			/* Flush failure also requires replaying previously sent bytes. */
			xa_for_each(&cache->blocks, index, entry)
				entry->dirty = entry->valid;
		} else {
			fat_cache_clear(sb, false);
			WRITE_ONCE(cache->state, FAT_CACHE_OFF);
		}
	}
	up_write(&cache->io_lock);
	return err;
}

long fat_write_cache_ioctl(struct file *file, bool enable)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct fat_write_cache *cache = &MSDOS_SB(sb)->write_cache;
	int err;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (!is_fat32(MSDOS_SB(sb)) || !MSDOS_SB(sb)->options.isvfat)
		return -EOPNOTSUPP;
	mutex_lock(&cache->control);
	if (sb_rdonly(sb) || !fat_writable(sb))
		err = -EROFS;
	else
		err = enable ? fat_cache_enable(sb) : fat_cache_disable(sb);
	mutex_unlock(&cache->control);
	return err;
}
