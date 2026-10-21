/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _FAT_WRITE_CACHE_H
#define _FAT_WRITE_CACHE_H

#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/xarray.h>

enum fat_write_cache_state {
	FAT_CACHE_OFF,
	FAT_CACHE_ON,
	FAT_CACHE_DRAINING,
	FAT_CACHE_STOPPED,
};

struct fat_write_cache {
	struct mutex control;
	struct rw_semaphore io_lock;
	struct xarray blocks;
	int state;
	int error;
};

void fat_write_cache_init(struct super_block *sb);
void fat_write_cache_destroy(struct super_block *sb);
bool fat_write_cache_active(struct super_block *sb);
bool fat_write_cache_stop(struct super_block *sb);
long fat_write_cache_ioctl(struct file *file, bool enable);
struct buffer_head *fat_cache_bread(struct super_block *sb, sector_t block);
struct buffer_head *fat_cache_getblk(struct super_block *sb, sector_t block);
void fat_cache_mark_dirty(struct super_block *sb, struct buffer_head *bh,
			  struct inode *inode);
int fat_cache_sync_buffer(struct super_block *sb, struct buffer_head *bh);
int fat_cache_prepare_block(struct super_block *sb, struct buffer_head *bh,
			    bool create);
int fat_cache_write_block(struct super_block *sb, sector_t block,
			  struct page *page, unsigned int offset);
int fat_cache_prepare_page(struct page *page);

#endif
