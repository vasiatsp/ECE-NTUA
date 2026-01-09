// SPDX-License-Identifier: GPL-2.0
/*
 * balloc.c
 *
 * This file contains the functions responsible for data blocks management
 *
 * Dimitris Siakavaras <jimsiak@cslab.ece.ntua.gr>
 *
 */

#include <linux/buffer_head.h>
#include "ext2.h"

/*
 * The free blocks are managed by bitmaps. A filesystem contains several
 * blocks groups. Each group contains 1 bitmap block for blocks, 1 bitmap
 * block for inodes, N blocks for the inode table and data blocks.
 *
 * The file system contains group descriptors which are located after the
 * super block. Each descriptor contains the number of the bitmap block and
 * the free blocks count in the block. The descriptors are loaded in memory
 * when a file system is mounted (see ext2_fill_super).
 */

#define ext2_in_range(b, first, len)	((b) >= (first) && (b) <= (first) + (len) - 1)

/**
 * Check whether the block_bitmap of the given block block_group is valid.
 * A valid block_bitmap satisfies the following:
 *  1. The bit that represents the block_bitmap block is set.
 *  2. The bit that represents the inode_bitmap block is set.
 *  3. The bits that represent the inode_table blocks are set.
 */
static int ext2_block_bitmap_valid(struct super_block *sb, struct ext2_group_desc *desc,
                                   unsigned int block_group, struct buffer_head *bh)
{
	ext2_grpblk_t offset;
	ext2_grpblk_t next_zero_bit;
	ext2_fsblk_t bitmap_blk;
	ext2_fsblk_t group_first_block;

	group_first_block = ext2_group_first_block_no(sb, block_group);

	/* 1. check whether block bitmap block number is set */
	bitmap_blk = le32_to_cpu(desc->bg_block_bitmap);
	offset = bitmap_blk - group_first_block;
	if (!test_bit_le(offset, bh->b_data))
		goto err_out; /* bad block bitmap */

	/* 2. check whether the inode bitmap block number is set */
	bitmap_blk = le32_to_cpu(desc->bg_inode_bitmap);
	offset = bitmap_blk - group_first_block;
	if (!test_bit_le(offset, bh->b_data))
		goto err_out; /* bad block bitmap */

	/* 3. check whether the inode table block number is set */
	bitmap_blk = le32_to_cpu(desc->bg_inode_table);
	offset = bitmap_blk - group_first_block;
	next_zero_bit = find_next_zero_bit_le(bh->b_data,
	                                      offset + EXT2_SB(sb)->s_itb_per_group,
	                                      offset);
	if (next_zero_bit < offset + EXT2_SB(sb)->s_itb_per_group)
		goto err_out; /* bad inode table */

	return 1; /* Everything checked and OK */

err_out:
	ext2_error(sb, __func__, "Invalid block bitmap - block_group = %d, block = %lu",
	           block_group, bitmap_blk);
	return 0;
}

/**
 * Read the block bitmap for a given block_group and validate the bits for
 * block/inode/inode tables are set in the bitmaps
 *
 * Return buffer_head on success or NULL in case of failure.
 */
static struct buffer_head *ext2_read_block_bitmap(struct super_block *sb,
                                                  unsigned int block_group)
{
	struct ext2_group_desc *desc;
	struct buffer_head *bh = NULL;
	ext2_fsblk_t bitmap_blk;
	int ret;

	desc = ext2_get_group_desc(sb, block_group, NULL);
	if (!desc)
		return NULL;

	bitmap_blk = le32_to_cpu(desc->bg_block_bitmap);
	bh = sb_getblk(sb, bitmap_blk);
	if (unlikely(!bh)) {
		ext2_error(sb, __func__, "Cannot read block bitmap - block_group = %d, block_bitmap = %u",
		           block_group, le32_to_cpu(desc->bg_block_bitmap));
		return NULL;
	}

	if (likely(bh_uptodate_or_lock(bh)))
		return bh;

	ret = bh_read(bh, 0);
	if (ret < 0)
		return bh;
	if (ret < 0) {
		brelse(bh);
		ext2_error(sb, __func__, "Cannot read block bitmap - block_group = %d, block_bitmap = %u",
		           block_group, le32_to_cpu(desc->bg_block_bitmap));
		return NULL;
	}

	if (!ext2_block_bitmap_valid(sb, desc, block_group, bh)) {
		brelse(bh);
		ext2_error(sb, __func__, "Block bitmap is not valid - block_group = %d, block_bitmap = %u",
		           block_group, le32_to_cpu(desc->bg_block_bitmap));
		return NULL;
	}

	return bh;
}

/**
 * Update desc->bg_free_blocks_count by adding count (can also be negative).
 */
static void group_update_free_blocks(struct super_block *sb, int group_no,
                                     struct ext2_group_desc *desc, struct buffer_head *bh,
                                     int count)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	unsigned free_blocks;

	if (count == 0)
		return;

	spin_lock(sb_bgl_lock(sbi, group_no));
	free_blocks = le16_to_cpu(desc->bg_free_blocks_count);
	desc->bg_free_blocks_count = cpu_to_le16(free_blocks + count);
	spin_unlock(sb_bgl_lock(sbi, group_no));
	mark_buffer_dirty(bh);
}

/**
 * Check whether blocks start_blk-(start_blk+count-1) are valid data blocks.
 * A valid data block satisfies the following:
 *  1. It exists after s_first_data_block.
 *  2. It exists before s_blocks_count.
 *  3. It is not the super_block block.
 *  4. FIXME what about other metadata blocks (group descriptors, bitmaps, ...)
 */
static int ext2_data_blocks_valid(struct ext2_sb_info *sbi,
                                  ext2_fsblk_t start_blk, unsigned int count)
{
	ext2_fsblk_t end_blk = start_blk + count - 1;

	if (end_blk < start_blk)
		return 0;

	if (start_blk <= le32_to_cpu(sbi->s_es->s_first_data_block))
		return 0;

	if (end_blk >= le32_to_cpu(sbi->s_es->s_blocks_count))
		return 0;

	if ((start_blk <= sbi->s_sb_block) && (end_blk >= sbi->s_sb_block))
		return 0;

	return 1;
}

/**
 * Check whether blocks start_blk-(start_blk+count-1) are valid data blocks in
 * the specified block group.
 * A valid data block satisfies the following:
 *  1. It is not the block_bitmap block.
 *  2. It is not the inode_bitmap block.
 *  3. It is part of the inode_table.
 */
static int ext2_data_blocks_valid_bg(struct ext2_group_desc *desc,
                                     struct ext2_sb_info *sbi,
                                     ext2_fsblk_t start_blk, unsigned int count)
{
	ext2_fsblk_t end_blk = start_blk + count - 1;

	if (end_blk < start_blk)
		return 0;

	if (ext2_in_range(le32_to_cpu(desc->bg_block_bitmap), start_blk, count))
		return 0;

	if (ext2_in_range(le32_to_cpu(desc->bg_inode_bitmap), start_blk, count))
		return 0;

	if (ext2_in_range(start_blk, le32_to_cpu(desc->bg_inode_table), sbi->s_itb_per_group))
		return 0;

	if (ext2_in_range(end_blk, le32_to_cpu(desc->bg_inode_table), sbi->s_itb_per_group))
		return 0;

	return 1;
}

/**
 * Find the block group descriptor of the given block_group.
 * On success, returns a pointer to the block group descriptor.
 * On failure, returns NULL.
 */
struct ext2_group_desc *ext2_get_group_desc(struct super_block *sb,
                                            unsigned int block_group,
                                            struct buffer_head **bh)
{
	unsigned long group_desc;
	unsigned long offset;
	struct ext2_group_desc *desc;
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	if (block_group >= sbi->s_groups_count) {
		ext2_error(sb, __func__, "block_group >= groups_count - "
		           "block_group = %d, groups_count = %lu", block_group,
		           sbi->s_groups_count);
		return NULL;
	}

	group_desc = block_group >> EXT2_DESC_PER_BLOCK_BITS(sb);
	offset = block_group & (EXT2_DESC_PER_BLOCK(sb) - 1);
	if (!sbi->s_group_desc[group_desc]) {
		ext2_error(sb, __func__, "Group descriptor not loaded - "
		           "block_group = %d, group_desc = %lu, desc = %lu",
		           block_group, group_desc, offset);
		return NULL;
	}

	desc = (struct ext2_group_desc *)sbi->s_group_desc[group_desc]->b_data;
	if (bh)
		*bh = sbi->s_group_desc[group_desc];
	return desc + offset;
}

/**
 * Free blocks block-(block+count-1)
 */
void ext2_free_blocks(struct inode *inode, unsigned long block, unsigned long count)
{
	struct buffer_head *bitmap_bh = NULL;
	struct buffer_head *bh2;
	struct super_block *sb = inode->i_sb;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_group_desc *desc;
	struct ext2_super_block *es = sbi->s_es;
	u32 fdb = le32_to_cpu(es->s_first_data_block);
	unsigned freed;
	unsigned long block_group, bit, i;

	if (!ext2_data_blocks_valid(sbi, block, count)) {
		ext2_error (sb, __func__, "Freeing invalid data blocks - block = %lu, count = %lu",
		                block, count);
		return;
	}

	block_group = (block - fdb) / EXT2_BLOCKS_PER_GROUP(sb);
	bit = (block - fdb) % EXT2_BLOCKS_PER_GROUP(sb);
	ext2_debug("freeing block(s) %lu-%lu from bg %lu\n", block, block + count - 1, block_group);

	bitmap_bh = ext2_read_block_bitmap(sb, block_group);
	if (!bitmap_bh) {
		brelse(bitmap_bh);
		return;
	}

	desc = ext2_get_group_desc(sb, block_group, &bh2);
	if (!desc) {
		brelse(bitmap_bh);
		return;
	}

	if (!ext2_data_blocks_valid_bg(desc, sbi, block, count)) {
		ext2_error(sb, __func__, "Freeing blocks in system zones - Block = %lu, count = %lu",
		           block, count);
		brelse(bitmap_bh);
		return;
	}

	for (i = 0, freed = 0; i < count; i++) {
		if (!ext2_clear_bit_atomic(sb_bgl_lock(sbi, block_group), bit + i, bitmap_bh->b_data))
			ext2_error(sb, __func__, "bit already cleared for block %lu", block + i);
		else
			freed++;
	}

	mark_buffer_dirty(bitmap_bh);
	if (sb->s_flags & SB_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh);

	group_update_free_blocks(sb, block_group, desc, bh2, freed);

	brelse(bitmap_bh);
	if (freed) {
		percpu_counter_add(&sbi->s_freeblocks_counter, freed);
		inode->i_blocks -= (freed * sb->s_blocksize) / 512;
		mark_inode_dirty(inode);
	}
	ext2_debug("freed: %u\n", freed);
}

/*
 * Finds the first free block in bitmap_bh and allocates up to count
 * consecutive blocks.
 * Returns the group offset of the first allocated block and the number of
 * blocks it managed to allocate (using the count parameter).
 */
/* ? */
/* You can use find_next_zero_bit_le() and ext2_set_bit_atomic() functions to 
 * handle the bitmaps.
 */
static int ext2_allocate_in_bg(struct super_block *sb, int group,
                               struct buffer_head *bitmap_bh, unsigned long *count)
{
	ext2_fsblk_t group_first_block = ext2_group_first_block_no(sb, group);
	ext2_fsblk_t group_last_block = ext2_group_last_block_no(sb, group);
	ext2_grpblk_t nblocks = group_last_block - group_first_block + 1;
	ext2_grpblk_t first_free_bit;
	unsigned long num = 0 ;

	/* ? */
	first_free_bit = find_next_zero_bit_le(bitmap_bh->b_data, nblocks, 0);
	if (first_free_bit >= nblocks)
		return -1;

	while (num < *count && (first_free_bit + num) < nblocks &&
	       !ext2_set_bit_atomic(sb_bgl_lock(EXT2_SB(sb), group), 
                               first_free_bit + num, bitmap_bh->b_data)) {
		num++;
	}
	if (num == 0) return -1;
	*count = num;
	return first_free_bit;
	//return -1;
}

/*
 * Allocates from disk a new block and returns its number on the disk.
 * `*countp` is used both as input and as output. As input it is the max blocks
 * that we are allowed to allocate. As output it show how many blocks we really
 * allocated.
 */
ext2_fsblk_t ext2_new_blocks(struct inode *inode, unsigned long *countp, int *errp)
{
	struct buffer_head *bitmap_bh = NULL, *gdp_bh;
	struct super_block *sb = inode->i_sb;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_inode_info *ei = EXT2_I(inode);
	struct ext2_group_desc *gdp;
	unsigned long ngroups = sbi->s_groups_count;
	unsigned long count = *countp;
	int bgi;
	__u16 free_blocks;
	__u32 group_no = ei->i_block_group;
	ext2_grpblk_t grp_alloc_blk;  /* blockgroup-relative allocated block*/
	ext2_fsblk_t ret_block;       /* filesystem-wide allocated block */

	/*
	 * First, check if there are free blocks available in the whole fs.
	 */
	free_blocks = percpu_counter_read_positive(&sbi->s_freeblocks_counter);
	if (free_blocks == 0) {
		*errp = -ENOSPC;
		return 0;
	}

	/*
	 * Now search each of the groups starting from the inode's group.
	 */
	for (bgi = 0; bgi < ngroups; bgi++, group_no = (group_no + 1) % ngroups) {
		gdp = ext2_get_group_desc(sb, group_no, &gdp_bh);
		if (!gdp) {
			*errp = -EIO;
			return 0;
		}

		//> skip this group if there are no free blocks
		free_blocks = le16_to_cpu(gdp->bg_free_blocks_count);
		if (!free_blocks)
			continue;

		brelse(bitmap_bh);
		bitmap_bh = ext2_read_block_bitmap(sb, group_no);
		if (!bitmap_bh) {
			*errp = -EIO;
			return 0;
		}

		//> try to allocate block(s) from this group.
		grp_alloc_blk = ext2_allocate_in_bg(sb, group_no, bitmap_bh, &count);
		if (grp_alloc_blk < 0)
			continue;


		//> We found and allocated the free block.
		ret_block = grp_alloc_blk + ext2_group_first_block_no(sb, group_no);
		ext2_debug("allocating block %lu located in bg %d (free_blocks: %d)\n",
		           ret_block, group_no, gdp->bg_free_blocks_count);

		group_update_free_blocks(sb, group_no, gdp, gdp_bh, -count);
		percpu_counter_sub(&sbi->s_freeblocks_counter, count);

		mark_buffer_dirty(bitmap_bh);
		if (sb->s_flags & SB_SYNCHRONOUS)
			sync_dirty_buffer(bitmap_bh);

		*errp = 0;
		brelse(bitmap_bh);
		if (count < *countp) {
			mark_inode_dirty(inode);
			*countp = count;
		}
		return ret_block;
	}

	//> No space left on the device.
	*errp = -ENOSPC;
	return 0;
}

/**
 * Count the number of free blocks in the super_block.
 */
unsigned long ext2_count_free_blocks(struct super_block *sb)
{
	struct ext2_group_desc *desc;
	unsigned long count = 0;
	int i;

        for (i = 0; i < EXT2_SB(sb)->s_groups_count; i++) {
                desc = ext2_get_group_desc(sb, i, NULL);
                if (!desc)
                        continue;
                count += le16_to_cpu(desc->bg_free_blocks_count);
	}
	return count;
}

/**
 * Get the number of blocks used by the superblock (primary or backup) in this
 * group.
 *
 * In ext2-lite we only consider file systems with backup superblocks in all
 * block groups.
 */
int ext2_bg_has_super(struct super_block *sb, int group)
{
	return 1;
}

/**
 * Get the number of blocks used by the group descriptor table (primary or
 * backup) in this group.
 */
unsigned long ext2_bg_num_gdb(struct super_block *sb, int group)
{
	return ext2_bg_has_super(sb, group) ? EXT2_SB(sb)->s_gdb_count : 0;
}
