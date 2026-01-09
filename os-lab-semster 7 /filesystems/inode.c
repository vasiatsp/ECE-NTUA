// SPDX-License-Identifier: GPL-2.0
/*
 * inode.c
 *
 * This file contains everything related to inode handling.
 *
 * Dimitris Siakavaras <jimsiak@cslab.ece.ntua.gr>
 *
 */

#include <linux/writeback.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/namei.h>
#include <linux/uio.h>
#include "ext2.h"

/* Necessary forward declarations of functions. */
static void ext2_truncate_blocks(struct inode *inode, loff_t offset);

static inline int ext2_inode_is_fast_symlink(struct inode *inode)
{
	return (S_ISLNK(inode->i_mode) && inode->i_blocks == 0);
}

static int ext2_get_blocks(struct inode *inode,
			   sector_t iblock, unsigned long maxblocks,
			   u32 *bno, bool *new, int create)
{
	struct ext2_inode_info *ei = EXT2_I(inode);
	u32 block_no = 0;

	ext2_debug("looking for block: %llu of inode: %lu create: %d\n",
	           iblock, inode->i_ino, create);

	/* We currently only support direct blocks. */
	if (iblock >= EXT2_NDIR_BLOCKS)
		return -EIO;

	block_no = le32_to_cpu(ei->i_data[iblock]);
	if (block_no > 0) {
		/* Block found, just return its number. */
		*bno = block_no;
		ext2_debug("found block %llu of inode %lu: %u\n",
		           iblock, inode->i_ino, block_no);
		return 1;
	} else if (!create) {
		/* Not found and the kernel did not ask from us to create it. */
		*bno = 0;
		ext2_debug("could not find block %llu of inode %lu: %u\n",
		           iblock, inode->i_ino, block_no);
		return 0;
	} else {
		/* Not found and the kernel asks from us to create (allocate) it. */
		ext2_fsblk_t resb;
		unsigned long count = 1;
		int errp;

		resb = ext2_new_blocks(inode, &count, &errp);
		if (errp < 0)
			return errp;

		ei->i_data[iblock] = resb;
		inode->i_blocks += (count * inode->i_sb->s_blocksize) / 512 ;
		mark_inode_dirty(inode);
		*bno = resb;
		*new = true;
		ext2_debug("allocated new block %llu for inode %lu: %lu"
		           " inode->i_blocks: %llu count: %lu\n",
		           iblock, inode->i_ino, resb, inode->i_blocks, count);
		return count;
	}
}

/*
 * This is the function that is passed to the page cache subsystem.
 * Its work is to appropriately find and map the desired inode's block (iblock)
 * in the page cache of the kernel.
 */
int ext2_get_block(struct inode *inode, sector_t iblock,
                   struct buffer_head *bh_result, int create)
{
	unsigned max_blocks;
	bool new = false;
	u32 bno;
	int ret;

	//> How many inode blocks can fit in the given buffer?
	max_blocks = bh_result->b_size >> inode->i_blkbits;
	ext2_debug("requesting iblock: %llu max_blocks: %u\n", iblock, max_blocks);

	ret = ext2_get_blocks(inode, iblock, max_blocks, &bno, &new, create);
	if (ret <= 0)
		return ret;

	map_bh(bh_result, inode->i_sb, bno);
	bh_result->b_size = (ret << inode->i_blkbits);
	if (new)
		set_buffer_new(bh_result);

	return 0;
}

static void ext2_write_failed(struct address_space *mapping, loff_t to)
{
	struct inode *inode = mapping->host;

	if (to > inode->i_size) {
		truncate_pagecache(inode, inode->i_size);
		ext2_truncate_blocks(inode, inode->i_size);
	}
}

static int ext2_read_folio(struct file *file, struct folio *folio)
{
	return mpage_read_folio(folio, ext2_get_block);
}

static void ext2_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, ext2_get_block);
}

static int ext2_write_begin(struct file *file, struct address_space *mapping,
                            loff_t pos, unsigned len,
                            struct page **pagep, void **fsdata)
{
	int ret;

	ret = block_write_begin(mapping, pos, len, pagep, ext2_get_block);
	if (ret < 0)
		ext2_write_failed(mapping, pos + len);
	return ret;
}

static int ext2_write_end(struct file *file, struct address_space *mapping,
                          loff_t pos, unsigned len, unsigned copied,
                          struct page *page, void *fsdata)
{
	int ret;

	ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
	if (ret < len)
		ext2_write_failed(mapping, pos + len);
	return ret;
}

static sector_t ext2_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, ext2_get_block);
}

static int ext2_writepages(struct address_space *mapping,
                           struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, ext2_get_block);
}

const struct address_space_operations ext2_aops = {
	.dirty_folio           = block_dirty_folio,
	.invalidate_folio      = block_invalidate_folio,
	.read_folio            = ext2_read_folio,
	.readahead             = ext2_readahead,
	.write_begin           = ext2_write_begin,
	.write_end             = ext2_write_end,
	.bmap                  = ext2_bmap,
	.writepages            = ext2_writepages,
	.migrate_folio         = buffer_migrate_folio,
	.is_partially_uptodate = block_is_partially_uptodate,
	.error_remove_folio    = generic_error_remove_folio,
};

/**
 *	ext2_free_data - free a list of data blocks
 *	@inode:	inode we are dealing with
 *	@p:	array of block numbers
 *	@q:	points immediately past the end of array
 *
 *	We are freeing all blocks referred from that array (numbers are
 *	stored as little-endian 32-bit) and updating @inode->i_blocks
 *	appropriately.
 */
static inline void ext2_free_data(struct inode *inode, __le32 *p, __le32 *q)
{
	unsigned long block_to_free = 0, count = 0;
	unsigned long nr;

	for ( ; p < q ; p++) {
		nr = le32_to_cpu(*p);
		if (nr) {
			*p = 0;
			/* accumulate blocks to free if they're contiguous */
			if (count == 0) {
				goto free_this;
			} else if (block_to_free == nr - count) {
				count++;
			} else {
				ext2_free_blocks(inode, block_to_free, count);
				mark_inode_dirty(inode);
			free_this:
				block_to_free = nr;
				count = 1;
			}
		}
	}
	if (count > 0) {
		ext2_free_blocks(inode, block_to_free, count);
		mark_inode_dirty(inode);
	}
}

/* Truncate the inode to the size of `offset` */
static void ext2_truncate_blocks(struct inode *inode, loff_t offset)
{
	__le32 *i_data = EXT2_I(inode)->i_data;
	long iblock;
	unsigned blocksize;

	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode)))
		return;
	if (ext2_inode_is_fast_symlink(inode))
		return;

	/* Which will now be the final block of the file? */
	blocksize = inode->i_sb->s_blocksize;
	iblock = (offset + blocksize-1) >> EXT2_BLOCK_SIZE_BITS(inode->i_sb);

	ext2_free_data(inode, &i_data[iblock], &i_data[EXT2_NDIR_BLOCKS]);
}

static struct ext2_inode *ext2_get_inode(struct super_block *sb, ino_t ino,
                                         struct buffer_head **p)
{
	struct buffer_head *bh;
	unsigned long block_group;
	unsigned long block;
	unsigned long offset;
	struct ext2_group_desc *gdp;
	unsigned long inodes_pg = EXT2_INODES_PER_GROUP(sb);
	int inode_sz = EXT2_INODE_SIZE(sb);
	unsigned long blocksize = sb->s_blocksize;

	*p = NULL;
	/* Check the validity of the given inode number. */
	if ((ino != EXT2_ROOT_INO && ino < EXT2_FIRST_INO(sb)) ||
	    ino > le32_to_cpu(EXT2_SB(sb)->s_es->s_inodes_count))
		goto einval;

	/* Figure out in which block is the inode we are looking for and get
	 * its group block descriptor. */
	/* ? */
	block_group = (ino - 1) / inodes_pg;
	gdp = ext2_get_group_desc(sb, block_group, NULL);
	if (!gdp)
		goto einval;

	/* Figure out the offset within the block group inode table */
	/* ? */
	offset = ((ino - 1) % inodes_pg) * inode_sz;
	block = le32_to_cpu(gdp->bg_inode_table) + (offset / blocksize);
	offset = offset % blocksize;

	/* Read the block from disk [cite: 637] */
	bh = sb_bread(sb, block);
	if (!bh)
		goto eio;

	/* Return the pointer to the appropriate ext2_inode */
	/* ? */
	*p = bh;
	return (struct ext2_inode *)(bh->b_data + offset);

einval:
	ext2_error(sb, __func__, "bad inode number: %lu", (unsigned long)ino);
	return ERR_PTR(-EINVAL);
eio:
	ext2_error(sb, __func__, "unable to read inode block - inode=%lu, block=%lu",
	           (unsigned long)ino, block);
	return ERR_PTR(-EIO);
}

void ext2_set_inode_flags(struct inode *inode)
{
	inode->i_flags &= ~(S_SYNC|S_APPEND|S_IMMUTABLE|S_NOATIME|S_DIRSYNC);
}

struct inode *ext2_iget(struct super_block *sb, unsigned long ino)
{
	struct ext2_inode_info *ei;
	struct buffer_head *bh = NULL;
	struct ext2_inode *raw_inode;
	struct inode *inode;
	long ret = -EIO;
	int n;

	ext2_debug("request to get ino: %lu\n", ino);

	/*
	 * Allocate the VFS node.
	 * We know that the returned inode is part of a bigger ext2_inode_info
	 * inode since iget_locked() calls our ext2_sops->alloc_inode() function
	 * to perform the allocation of the inode.
	 */
	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	/*
	 * Read the EXT2 inode *from disk*
	 */
	raw_inode = ext2_get_inode(inode->i_sb, ino, &bh);
	if (IS_ERR(raw_inode)) {
		ret = PTR_ERR(raw_inode);
		brelse(bh);
		iget_failed(inode);
		return ERR_PTR(ret);
	}

	/*
	 * Fill the necessary fields of the VFS inode structure.
	 */
	inode->i_mode = le16_to_cpu(raw_inode->i_mode);
	i_uid_write(inode, (uid_t)le16_to_cpu(raw_inode->i_uid));
	i_gid_write(inode, (gid_t)le16_to_cpu(raw_inode->i_gid));
	set_nlink(inode, le16_to_cpu(raw_inode->i_links_count));
	inode_set_atime(inode, (signed)le32_to_cpu(raw_inode->i_atime), 0);
	inode_set_ctime(inode, (signed)le32_to_cpu(raw_inode->i_ctime), 0);
	inode_set_mtime(inode, (signed)le32_to_cpu(raw_inode->i_mtime), 0);
	ei->i_dtime = le32_to_cpu(raw_inode->i_dtime);
	inode->i_blocks = le32_to_cpu(raw_inode->i_blocks);
	inode->i_size = le32_to_cpu(raw_inode->i_size);
	if (i_size_read(inode) < 0) {
		ret = -EUCLEAN;
		brelse(bh);
		iget_failed(inode);
		return ERR_PTR(ret);
	}
	//> Setup the {inode,file}_operations structures depending on the type.
	if (S_ISREG(inode->i_mode)) {
		/* ? */
		inode->i_op = &ext2_file_inode_operations; // [cite: 435]
		inode->i_fop = &ext2_file_operations;      // [cite: 481]
		inode->i_mapping->a_ops = &ext2_aops;
	} else if (S_ISDIR(inode->i_mode)) {
		/* ? */
		inode->i_op = &ext2_dir_inode_operations;  // [cite: 380]
		inode->i_fop = &ext2_dir_operations;       // [cite: 481]
		inode->i_mapping->a_ops = &ext2_aops;
	} else if (S_ISLNK(inode->i_mode)) {
		if (ext2_inode_is_fast_symlink(inode)) {
			inode->i_op = &simple_symlink_inode_operations;
			inode->i_link = (char *)ei->i_data;
			nd_terminate_link(ei->i_data, inode->i_size, sizeof(ei->i_data) - 1);
		} else {
			inode->i_op = &page_symlink_inode_operations;
			inode_nohighmem(inode);
			inode->i_mapping->a_ops = &ext2_aops;
		}
	} else {
		inode->i_op = &ext2_special_inode_operations;
		if (raw_inode->i_block[0])
			init_special_inode(inode, inode->i_mode,
			   old_decode_dev(le32_to_cpu(raw_inode->i_block[0])));
		else 
			init_special_inode(inode, inode->i_mode,
			   new_decode_dev(le32_to_cpu(raw_inode->i_block[1])));
	}

	/*
	 * Fill the necessary fields of the ext2_inode_info structure.
	 */
	ei = EXT2_I(inode);
	ei->i_dtime = le32_to_cpu(raw_inode->i_dtime);
	ei->i_flags = le32_to_cpu(raw_inode->i_flags);
	ext2_set_inode_flags(inode);
	ei->i_dtime = 0;
	ei->i_state = 0;
	ei->i_block_group = (ino - 1) / EXT2_INODES_PER_GROUP(inode->i_sb);
	//> NOTE! The in-memory inode i_data array is in little-endian order
	//> even on big-endian machines: we do NOT byteswap the block numbers!
	for (n = 0; n < EXT2_N_BLOCKS; n++)
		ei->i_data[n] = raw_inode->i_block[n];

	brelse(bh);
	unlock_new_inode(inode);
	return inode;
}

static int ext2_do_write_inode(struct inode *inode, int do_sync)
{
	struct ext2_inode_info *ei = EXT2_I(inode);
	struct super_block *sb = inode->i_sb;
	ino_t ino = inode->i_ino;
	struct buffer_head *bh;
	struct ext2_inode *raw_inode = ext2_get_inode(sb, ino, &bh);
	int n;
	int err = 0;

	if (IS_ERR(raw_inode))
 		return -EIO;

	/* For fields not in the in-memory inode, initialise them to zero for new inodes. */
	if (ei->i_state & EXT2_STATE_NEW)
		memset(raw_inode, 0, EXT2_SB(sb)->s_inode_size);

	raw_inode->i_mode = cpu_to_le16(inode->i_mode);
	raw_inode->i_uid = cpu_to_le16(fs_high2lowuid(i_uid_read(inode)));
	raw_inode->i_gid = cpu_to_le16(fs_high2lowgid(i_gid_read(inode)));
	raw_inode->i_links_count = cpu_to_le16(inode->i_nlink);
	raw_inode->i_size = cpu_to_le32(inode->i_size);
	raw_inode->i_atime = cpu_to_le32(inode_get_atime_sec(inode));
	raw_inode->i_ctime = cpu_to_le32(inode_get_ctime_sec(inode));
	raw_inode->i_mtime = cpu_to_le32(inode_get_mtime_sec(inode));

	raw_inode->i_blocks = cpu_to_le32(inode->i_blocks);
	raw_inode->i_dtime = cpu_to_le32(ei->i_dtime);
	raw_inode->i_flags = cpu_to_le32(ei->i_flags);
	
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
		if (old_valid_dev(inode->i_rdev)) {
			raw_inode->i_block[0] = cpu_to_le32(old_encode_dev(inode->i_rdev));
			raw_inode->i_block[1] = 0;
		} else {
			raw_inode->i_block[0] = 0;
			raw_inode->i_block[1] = cpu_to_le32(new_encode_dev(inode->i_rdev));
			raw_inode->i_block[2] = 0;
		}
	} else for (n = 0; n < EXT2_N_BLOCKS; n++) {
		raw_inode->i_block[n] = ei->i_data[n];
	}

	mark_buffer_dirty(bh);
	if (do_sync) {
		sync_dirty_buffer(bh);
		if (buffer_req(bh) && !buffer_uptodate(bh)) {
			printk ("IO error syncing ext2 inode [%s:%08lx]\n",
			        sb->s_id, (unsigned long) ino);
			err = -EIO;
		}
	}
	ei->i_state &= ~EXT2_STATE_NEW;
	brelse(bh);
	return err;
}

/* Called at the last iput() if i_nlink is zero. */
void ext2_evict_inode(struct inode *inode)
{
	int want_delete = 0;
	struct ext2_inode_info *ei = EXT2_I(inode);

	if (inode->i_nlink == 0 && !is_bad_inode(inode))
		want_delete = 1;

	truncate_inode_pages_final(&inode->i_data);

	if (want_delete) {
		sb_start_intwrite(inode->i_sb);
		ei->i_dtime	= ktime_get_real_seconds();
		mark_inode_dirty(inode);
		ext2_do_write_inode(inode, inode_needs_sync(inode));
		inode->i_size = 0;
		if (inode->i_blocks)
			ext2_truncate_blocks(inode, 0);
	}

	invalidate_inode_buffers(inode);
	clear_inode(inode);

	if (want_delete) {
		ext2_free_inode(inode);
		sb_end_intwrite(inode->i_sb);
	}
}

int ext2_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	return ext2_do_write_inode(inode, wbc->sync_mode == WB_SYNC_ALL);
}

static int ext2_setsize(struct inode *inode, loff_t newsize)
{
	int error;

	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode)))
		return -EINVAL;
	if (ext2_inode_is_fast_symlink(inode))
		return -EINVAL;

	inode_dio_wait(inode);

	error = block_truncate_page(inode->i_mapping, newsize, ext2_get_block);
	if (error)
		return error;

	truncate_setsize(inode, newsize);
	ext2_truncate_blocks(inode, newsize);

	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	if (inode_needs_sync(inode)) {
		sync_mapping_buffers(inode->i_mapping);
		sync_inode_metadata(inode, 1);
	} else {
		mark_inode_dirty(inode);
	}

	return 0;
}

int ext2_getattr(struct mnt_idmap *idmap,
                 const struct path *path, struct kstat *stat,
                 u32 request_mask, unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct ext2_inode_info *ei = EXT2_I(inode);
	unsigned int flags;

	flags = ei->i_flags;
	stat->attributes_mask |= (STATX_ATTR_APPEND | STATX_ATTR_IMMUTABLE |
	                          STATX_ATTR_NODUMP);

	generic_fillattr(&nop_mnt_idmap, request_mask, inode, stat);
	return 0;
}

int ext2_setattr(struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *iattr)
{
	struct inode *inode = d_inode(dentry);
	int error;

	error = setattr_prepare(&nop_mnt_idmap, dentry, iattr);
	if (error)
		return error;

	if (iattr->ia_valid & ATTR_SIZE && iattr->ia_size != inode->i_size) {
		error = ext2_setsize(inode, iattr->ia_size);
		if (error)
			return error;
	}
	setattr_copy(&nop_mnt_idmap, inode, iattr);
	mark_inode_dirty(inode);

	return error;
}
