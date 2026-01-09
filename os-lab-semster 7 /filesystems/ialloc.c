// SPDX-License-Identifier: GPL-2.0
/*
 * ialloc.c
 *
 * This file contains the functions responsible for inode allocation/deallocation
 *
 * Dimitris Siakavaras <jimsiak@cslab.ece.ntua.gr>
 *
 */

#include <linux/backing-dev.h>
#include <linux/buffer_head.h>
#include "ext2.h"

/*
 * The free inodes are managed by bitmaps. A file system contains several
 * blocks groups. Each group contains 1 bitmap block for blocks, 1 bitmap block
 * for inodes, N blocks for the inode table and data blocks.
 *
 * The file system contains group descriptors which are located after the super
 * block. Each descriptor contains the number of the bitmap block and the free
 * blocks count in the block. The descriptors are loaded in memory when a file
 * system is mounted (see ext2_fill_super).
 */

/**
 * Read the inode bitmap for a given block_group.
 */
static struct buffer_head *read_inode_bitmap(struct super_block *sb,
                                             unsigned long block_group)
{
	struct ext2_group_desc *desc;
	struct buffer_head *bh = NULL;

	desc = ext2_get_group_desc(sb, block_group, NULL);
	if (!desc)
		return NULL;

	bh = sb_bread(sb, le32_to_cpu(desc->bg_inode_bitmap));
	if (!bh)
		ext2_error(sb, __func__, "Cannot read inode bitmap - block_group = %lu, inode_bitmap = %u",
		           block_group, le32_to_cpu(desc->bg_inode_bitmap));
	return bh;
}

/**
 * Update the Block Group Descriptor, one inode was removed.
 */
static void ext2_release_inode(struct super_block *sb, int group, int dir)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_group_desc *desc;
	struct buffer_head *bh;

	desc = ext2_get_group_desc(sb, group, &bh);
	if (!desc) {
		ext2_error(sb, __func__, "can't get descriptor for group %d", group);
		return;
	}

	spin_lock(sb_bgl_lock(sbi, group));
	le16_add_cpu(&desc->bg_free_inodes_count, 1);
	if (dir)
		le16_add_cpu(&desc->bg_used_dirs_count, -1);
	spin_unlock(sb_bgl_lock(sbi, group));

	percpu_counter_inc(&sbi->s_freeinodes_counter);
	if (dir)
		percpu_counter_dec(&sbi->s_dirs_counter);
	mark_buffer_dirty(bh);
}

/*
 * Mark the inode in the disk as free.
 */
void ext2_free_inode(struct inode *inode)
{
	struct buffer_head *bitmap_bh;
	struct super_block *sb = inode->i_sb;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block *es = sbi->s_es;
	unsigned long ino = inode->i_ino;
	unsigned long block_group, bit;

	ext2_debug("freeing inode %lu\n", ino);

	if (ino < EXT2_FIRST_INO(sb) || ino > le32_to_cpu(es->s_inodes_count)) {
		ext2_error (sb, __func__, "reserved or nonexistent inode %lu", ino);
		return;
	}

	block_group = (ino - 1) / EXT2_INODES_PER_GROUP(sb);
	bit = (ino - 1) % EXT2_INODES_PER_GROUP(sb);
	bitmap_bh = read_inode_bitmap(sb, block_group);
	if (!bitmap_bh)
		return;

	/* Ok, now we can actually update the inode bitmaps.. */
	if (!ext2_clear_bit_atomic(sb_bgl_lock(sbi, block_group), bit, (void *)bitmap_bh->b_data))
		ext2_error(sb, __func__, "bit already cleared for inode %lu", ino);
	else
		ext2_release_inode(sb, block_group, S_ISDIR(inode->i_mode));

	mark_buffer_dirty(bitmap_bh);
	if (sb->s_flags & SB_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh);

	brelse(bitmap_bh);
}

/**
 * Find an appropriate group for an inode.
 */
static int find_group(struct super_block *sb, struct inode *parent)
{
	int parent_group = EXT2_I(parent)->i_block_group;
	int ngroups = EXT2_SB(sb)->s_groups_count;
	struct ext2_group_desc *desc;
	int group, i;

	/* Try to place the inode in its parent directory */
	group = parent_group;
	desc = ext2_get_group_desc(sb, group, NULL);
	if (desc && le16_to_cpu(desc->bg_free_inodes_count) &&
	            le16_to_cpu(desc->bg_free_blocks_count))
		return group;

	/*
	 * Use a quadratic hash to find a group with a free inode and some
	 * free blocks.
	 *
	 * We're going to place this inode in a different blockgroup from its
	 * parent.  We want to cause files in a common directory to all land in
	 * the same blockgroup.  But we want files which are in a different
	 * directory which shares a blockgroup with our parent to land in a
	 * different blockgroup.
	 *
	 * So add our directory's i_ino into the starting point for the hash.
	 */
	group = (group + parent->i_ino) % ngroups;
	for (i = 1; i < ngroups; i <<= 1) {
		group += i;
		if (group >= ngroups)
			group -= ngroups;

		desc = ext2_get_group_desc(sb, group, NULL);
		if (desc && le16_to_cpu(desc->bg_free_inodes_count) &&
		            le16_to_cpu(desc->bg_free_blocks_count))
			return group;
	}

	/*
	 * That failed: try linear search for a free inode, even if that group
	 * has no free blocks.
	 */
	group = parent_group;
	for (i = 0; i < ngroups; i++) {
		group = (group+1) % ngroups;
		desc = ext2_get_group_desc(sb, group, NULL);
		if (desc && le16_to_cpu(desc->bg_free_inodes_count))
			return group;
	}

	/* No available inode. */
	return -1;
}

/**
 * Allocate a new in-memory inode and "connect" it with an on-disk one.
 */
struct inode *ext2_new_inode(struct inode *dir, umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block *es = sbi->s_es;
	struct buffer_head *bh2, *bitmap_bh = NULL;
	struct inode *inode;
	struct ext2_group_desc *gdp;
	struct ext2_inode_info *ei;
	int group, i, err;
	ino_t ino = 0;
	unsigned long inodes_pg = EXT2_INODES_PER_GROUP(sb);

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	ei = EXT2_I(inode);

	group = find_group(sb, dir);
	if (group == -1) {
		err = -ENOSPC;
		goto fail;
	}

	for (i = 0; i < sbi->s_groups_count; i++) {
		gdp = ext2_get_group_desc(sb, group, &bh2);
		if (!gdp) {
			group = (group+1) % sbi->s_groups_count;
			continue;
		}

		brelse(bitmap_bh);
		bitmap_bh = read_inode_bitmap(sb, group);
		if (!bitmap_bh) {
			err = -EIO;
			goto fail;
		}
		ino = 0;

repeat_in_this_group:
		ino = find_next_zero_bit_le((unsigned long *)bitmap_bh->b_data, inodes_pg, ino);
		if (ino >= inodes_pg) {
			/*
			 * Rare race: find_group() decided that there were free
			 * inodes in this group, but by the time we tried to
			 * allocate one, they're all gone.  Just go and search
			 * the next block group.
			 */
			group = (group+1) % sbi->s_groups_count;
			continue;
		}
		if (ext2_set_bit_atomic(sb_bgl_lock(sbi, group), ino, bitmap_bh->b_data)) {
			/*
			 * Race again, the inode marked as occupied before we do so
			 */
			if (++ino >= inodes_pg) {
				/* This group is exhausted, try next group. */
				group = (group+1) % sbi->s_groups_count;
				continue;
			}
			/* Try to find free inode in the same group. */
			goto repeat_in_this_group;
		}

		/* We finally got an inode. */
		goto got;
	}

	/* Scanned all blockgroups, no free inode found. */
	brelse(bitmap_bh);
	err = -ENOSPC;
	goto fail;

got:
	mark_buffer_dirty(bitmap_bh);
	if (sb->s_flags & SB_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh);
	brelse(bitmap_bh);

	ino += group * inodes_pg + 1;
	if (ino < EXT2_FIRST_INO(sb) || ino > le32_to_cpu(es->s_inodes_count)) {
		ext2_error (sb, __func__, "reserved inode or inode > inodes count - "
			    "block_group = %d,inode=%lu", group, ino);
		err = -EIO;
		goto fail;
	}

	percpu_counter_dec(&sbi->s_freeinodes_counter);
	if (S_ISDIR(mode))
		percpu_counter_inc(&sbi->s_dirs_counter);

	spin_lock(sb_bgl_lock(sbi, group));
	le16_add_cpu(&gdp->bg_free_inodes_count, -1);
	if (S_ISDIR(mode))
		le16_add_cpu(&gdp->bg_used_dirs_count, 1);
	spin_unlock(sb_bgl_lock(sbi, group));

	mark_buffer_dirty(bh2);
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);

	inode->i_ino = ino;
	inode->i_blocks = 0;
	simple_inode_init_ts(inode);
	memset(ei->i_data, 0, sizeof(ei->i_data));
	ei->i_flags = EXT2_I(dir)->i_flags;
	ei->i_dtime = 0;
	ei->i_block_group = group;
	ei->i_state = EXT2_STATE_NEW;
	ext2_set_inode_flags(inode);
	if (insert_inode_locked(inode) < 0) {
		ext2_error(sb, __func__, "inode number already in use - inode=%lu", ino);
		err = -EIO;
		goto fail;
	}

	mark_inode_dirty(inode);
	ext2_debug("allocating inode %lu\n", inode->i_ino);
	return inode;

fail:
	make_bad_inode(inode);
	iput(inode);
	return ERR_PTR(err);
}

/**
 * Count the number of free inodes in the super_block.
 */
unsigned long ext2_count_free_inodes(struct super_block *sb)
{
	struct ext2_group_desc *desc;
	unsigned long count = 0;
	int i;	

	for (i = 0; i < EXT2_SB(sb)->s_groups_count; i++) {
		desc = ext2_get_group_desc(sb, i, NULL);
		if (!desc)
			continue;
		count += le16_to_cpu(desc->bg_free_inodes_count);
	}
	return count;
}

/**
 * Count the number of directories in the super_block.
 */
unsigned long ext2_count_dirs(struct super_block *sb)
{
	struct ext2_group_desc *desc;
	unsigned long count = 0;
	int i;

	for (i = 0; i < EXT2_SB(sb)->s_groups_count; i++) {
		desc = ext2_get_group_desc(sb, i, NULL);
		if (!desc)
			continue;
		count += le16_to_cpu(desc->bg_used_dirs_count);
	}
	return count;
}
