// SPDX-License-Identifier: GPL-2.0-only
/*
 * super.c
 *
 * This file contains the basic routines for ext2-lite.
 *
 * Dimitris Siakavaras <jimsiak@cslab.ece.ntua.gr>
 *
 */

#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/parser.h>
#include <linux/buffer_head.h>
#include <linux/vfs.h>
#include <linux/seq_file.h>
#include <linux/iversion.h>
#include "ext2.h"

// Necessary forward declarations
static void ext2_sync_super(struct super_block *, struct ext2_super_block *, int);
static int ext2_sync_fs(struct super_block *sb, int wait);

void ext2_error(struct super_block *sb, const char *function, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block *es = sbi->s_es;

	if (!sb_rdonly(sb)) {
		spin_lock(&sbi->s_lock);
		sbi->s_mount_state |= EXT2_ERROR_FS;
		es->s_state |= cpu_to_le16(EXT2_ERROR_FS);
		spin_unlock(&sbi->s_lock);
		ext2_sync_super(sb, es, 1);
	}

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	printk(KERN_CRIT "EXT2-fs (%s): error: %s: %pV\n", sb->s_id, function, &vaf);

	va_end(args);

	if (test_opt(sb, ERRORS_PANIC))
		panic("EXT2-fs: panic from previous error\n");
	if (!sb_rdonly(sb) && test_opt(sb, ERRORS_RO)) {
		ext2_msg(sb, KERN_CRIT, "error: remounting filesystem read-only");
		sb->s_flags |= SB_RDONLY;
	}
}

void ext2_msg(struct super_block *sb, const char *prefix, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	printk("%sEXT2-fs-lite (%s): %pV\n", prefix, sb->s_id, &vaf);
	va_end(args);
}

static struct kmem_cache *ext2_inode_cachep;

static void init_once(void *foo)
{
	struct ext2_inode_info *ei = (struct ext2_inode_info *)foo;
	inode_init_once(&ei->vfs_inode);
}

static int __init init_inodecache(void)
{
	ext2_inode_cachep = kmem_cache_create_usercopy("ext2_inode_cache",
	                          sizeof(struct ext2_inode_info), 0,
	                          SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
	                          offsetof(struct ext2_inode_info, i_data),
	                          sizeof_field(struct ext2_inode_info, i_data),
	                          init_once);
	return (ext2_inode_cachep == NULL) ? -ENOMEM : 0;
}

static void destroy_inodecache(void)
{
	/*
	 * Make sure all delayed rcu free inodes are flushed before we
	 * destroy cache.
	 */
	rcu_barrier();
	kmem_cache_destroy(ext2_inode_cachep);
}

enum {
	Opt_err_cont, Opt_err_panic, Opt_err_ro, Opt_debug
};

static const match_table_t tokens = {
	{Opt_err_cont, "errors=continue"},
	{Opt_err_panic, "errors=panic"},
	{Opt_err_ro, "errors=remount-ro"},
	{Opt_debug, "debug"},
};

static int parse_options(char *options, struct super_block *sb,
                         unsigned long *opt)
{
	char *p;
	substring_t args[MAX_OPT_ARGS];

	if (!options)
		return 1;

	while ((p = strsep(&options, ",")) != NULL) {
		int token;
		if (!*p)
			continue;

		token = match_token(p, tokens, args);
		switch (token) {
		case Opt_err_panic:
			clear_opt(*opt, ERRORS_CONT);
			clear_opt(*opt, ERRORS_RO);
			set_opt(*opt, ERRORS_PANIC);
			break;
		case Opt_err_ro:
			clear_opt(*opt, ERRORS_CONT);
			clear_opt(*opt, ERRORS_PANIC);
			set_opt(*opt, ERRORS_RO);
			break;
		case Opt_err_cont:
			clear_opt(*opt, ERRORS_RO);
			clear_opt(*opt, ERRORS_PANIC);
			set_opt(*opt, ERRORS_CONT);
			break;
		case Opt_debug:
			set_opt(*opt, DEBUG);
			break;
		default:
			return 0;
		}
	}
	return 1;
}

static int ext2_check_super(struct super_block *sb, struct ext2_super_block *es,
                            int read_only)
{
	int res = 0;
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	if (le32_to_cpu(es->s_rev_level) > EXT2_MAX_SUPP_REV) {
		ext2_msg(sb, KERN_ERR, "error: revision level too high, forcing read-only mode");
		res = SB_RDONLY;
	}
	if (read_only)
		return res;
	if (!(sbi->s_mount_state & EXT2_VALID_FS))
		ext2_msg(sb, KERN_WARNING, "warning: mounting unchecked fs, running e2fsck is recommended");
	else if ((sbi->s_mount_state & EXT2_ERROR_FS))
		ext2_msg(sb, KERN_WARNING, "warning: mounting fs with errors, running e2fsck is recommended");
	else if (le32_to_cpu(es->s_checkinterval) &&
	         (le32_to_cpu(es->s_lastcheck) + le32_to_cpu(es->s_checkinterval) <= ktime_get_real_seconds()))
		ext2_msg(sb, KERN_WARNING, "warning: checktime reached, running e2fsck is recommended");

	le16_add_cpu(&es->s_mnt_count, 1);
	if (test_opt(sb, DEBUG))
		ext2_msg(sb, KERN_INFO, "%s, %s, bs=%lu, gc=%lu, bpg=%lu, ipg=%lu, mo=%04lx]",
		         EXT2FS_VERSION, EXT2FS_DATE, sb->s_blocksize, sbi->s_groups_count,
		         EXT2_BLOCKS_PER_GROUP(sb), EXT2_INODES_PER_GROUP(sb), sbi->s_mount_opt);
	return res;
}

static int ext2_check_descriptors(struct super_block *sb)
{
	int i;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	__u32 bg_block_bitmap, bg_inode_bitmap, bg_inode_table, bg_inode_table_last;

	ext2_debug ("Checking group descriptors\n");

	for (i = 0; i < sbi->s_groups_count; i++) {
		struct ext2_group_desc *gdp = ext2_get_group_desc(sb, i, NULL);
		ext2_fsblk_t first_block = ext2_group_first_block_no(sb, i);
		ext2_fsblk_t last_block  = ext2_group_last_block_no(sb, i);

		bg_block_bitmap = le32_to_cpu(gdp->bg_block_bitmap);
		if (bg_block_bitmap < first_block || bg_block_bitmap > last_block)
		{
			ext2_error (sb, __func__, "Block bitmap for group %d not in group (block %lu)!",
				    i, (unsigned long)bg_block_bitmap);
			return 0;
		}

		bg_inode_bitmap = le32_to_cpu(gdp->bg_inode_bitmap);
		if (bg_inode_bitmap < first_block || bg_inode_bitmap > last_block)
		{
			ext2_error (sb, __func__, "Inode bitmap for group %d not in group (block %lu)!",
				    i, (unsigned long)bg_inode_bitmap);
			return 0;
		}

		bg_inode_table = le32_to_cpu(gdp->bg_inode_table);
		bg_inode_table_last = bg_inode_table + sbi->s_itb_per_group - 1;
		if (bg_inode_table < first_block || bg_inode_table_last > last_block)
		{
			ext2_error (sb, __func__,
				    "Inode table for group %d not in group (block %lu)!",
				    i, (unsigned long)bg_inode_table);
			return 0;
		}
	}
	return 1;
}

static unsigned long descriptor_loc(struct super_block *sb,
                                    unsigned long logic_sb_block, int nr)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	unsigned long bg, first_meta_bg;
	
	first_meta_bg = le32_to_cpu(sbi->s_es->s_first_meta_bg);

	if (nr < first_meta_bg)
		return (logic_sb_block + nr + 1);

	bg = sbi->s_desc_per_block * nr;
	return ext2_group_first_block_no(sb, bg) + ext2_bg_has_super(sb, bg);
}

static void ext2_sync_super(struct super_block *sb, struct ext2_super_block *es,
                            int wait)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	/*
	 * There seems to have been some error with a previous attempt to write the
	 * superblock. Maybe the underlying block device was violently unplugged
	 * (i.e., a USB device was yanked out). We can only retry the write and hope
	 * now it succeeds.
	 */
	if (buffer_write_io_error(sbi->s_sbh)) {
		ext2_msg(sb, KERN_ERR, "previous I/O error to superblock detected");
		clear_buffer_write_io_error(sbi->s_sbh);
		set_buffer_uptodate(sbi->s_sbh);
	}

	spin_lock(&sbi->s_lock);
	es->s_free_blocks_count = cpu_to_le32(ext2_count_free_blocks(sb));
	es->s_free_inodes_count = cpu_to_le32(ext2_count_free_inodes(sb));
	es->s_wtime = cpu_to_le32(ktime_get_real_seconds());
	spin_unlock(&sbi->s_lock); /* unlock before we do IO */

	mark_buffer_dirty(sbi->s_sbh);
	if (wait)
		sync_dirty_buffer(sbi->s_sbh);
}

static void ext2_write_super(struct super_block *sb)
{
	if (!sb_rdonly(sb))
		ext2_sync_fs(sb, 1);
}

static struct inode *ext2_alloc_inode(struct super_block *sb)
{
	struct ext2_inode_info *ei;
	ei = kmem_cache_alloc(ext2_inode_cachep, GFP_KERNEL);
	if (!ei)
		return NULL;

	inode_set_iversion(&ei->vfs_inode, 1);

	return &ei->vfs_inode;
}

static void ext2_free_inode_sb(struct inode *inode)
{
	kmem_cache_free(ext2_inode_cachep, EXT2_I(inode));
}

static void ext2_put_super(struct super_block *sb)
{
	int db_count;
	int i;
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	if (!sb_rdonly(sb)) {
		struct ext2_super_block *es = sbi->s_es;

		spin_lock(&sbi->s_lock);
		es->s_state = cpu_to_le16(sbi->s_mount_state);
		spin_unlock(&sbi->s_lock);
		ext2_sync_super(sb, es, 1);
	}
	db_count = sbi->s_gdb_count;
	for (i = 0; i < db_count; i++)
		brelse(sbi->s_group_desc[i]);
	kfree(sbi->s_group_desc);
	percpu_counter_destroy(&sbi->s_freeblocks_counter);
	percpu_counter_destroy(&sbi->s_freeinodes_counter);
	percpu_counter_destroy(&sbi->s_dirs_counter);
	brelse(sbi->s_sbh);
	sb->s_fs_info = NULL;
	kfree(sbi->s_blockgroup_lock);
	kfree(sbi);
}

/*
 * In the second extended file system, it is not necessary to
 * write the super block since we use a mapping of the
 * disk super block in a buffer.
 *
 * However, this function is still used to set the fs valid
 * flags to 0.  We need to set this flag to 0 since the fs
 * may have been checked while mounted and e2fsck may have
 * set s_state to EXT2_VALID_FS after some corrections.
 */
static int ext2_sync_fs(struct super_block *sb, int wait)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block *es = EXT2_SB(sb)->s_es;

	spin_lock(&sbi->s_lock);
	if (es->s_state & cpu_to_le16(EXT2_VALID_FS)) {
		ext2_debug("setting valid to 0\n");
		es->s_state &= cpu_to_le16(~EXT2_VALID_FS);
	}
	spin_unlock(&sbi->s_lock);
	ext2_sync_super(sb, es, wait);
	return 0;
}

static int ext2_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block *es = sbi->s_es;
	u64 fsid;

	spin_lock(&sbi->s_lock);

	if (sbi->s_blocks_last != le32_to_cpu(es->s_blocks_count)) {
		unsigned long i, overhead = 0;
		smp_rmb();

		/*
		 * Compute the overhead (FS structures). This is constant
		 * for a given filesystem unless the number of block groups
		 * changes so we cache the previous value until it does.
		 */

		/*
		 * All of the blocks before first_data_block are
		 * overhead
		 */
		overhead = le32_to_cpu(es->s_first_data_block);

		/*
		 * Add the overhead attributed to the superblock and
		 * block group descriptors.  If the sparse superblocks
		 * feature is turned on, then not all groups have this.
		 */
		for (i = 0; i < sbi->s_groups_count; i++)
			overhead += ext2_bg_has_super(sb, i) + ext2_bg_num_gdb(sb, i);

		/*
		 * Every block group has an inode bitmap, a block
		 * bitmap, and an inode table.
		 */
		overhead += (sbi->s_groups_count * (2 + sbi->s_itb_per_group));
		sbi->s_overhead_last = overhead;
		smp_wmb();
		sbi->s_blocks_last = le32_to_cpu(es->s_blocks_count);
	}

	buf->f_type = EXT2_SUPER_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = le32_to_cpu(es->s_blocks_count) - sbi->s_overhead_last;
	buf->f_bfree = ext2_count_free_blocks(sb);
	es->s_free_blocks_count = cpu_to_le32(buf->f_bfree);
	buf->f_bavail = buf->f_bfree;
	buf->f_files = le32_to_cpu(es->s_inodes_count);
	buf->f_ffree = ext2_count_free_inodes(sb);
	es->s_free_inodes_count = cpu_to_le32(buf->f_ffree);
	buf->f_namelen = EXT2_NAME_LEN;
	fsid = le64_to_cpup((void *)es->s_uuid) ^
	       le64_to_cpup((void *)es->s_uuid + sizeof(u64));
	buf->f_fsid = u64_to_fsid(fsid);
	spin_unlock(&sbi->s_lock);
	return 0;
}

static int ext2_remount(struct super_block *sb, int *flags, char *data)
{
	struct ext2_sb_info * sbi = EXT2_SB(sb);
	struct ext2_super_block * es;
	unsigned long new_opt;

	sync_filesystem(sb);

	spin_lock(&sbi->s_lock);
	new_opt = sbi->s_mount_opt;
	spin_unlock(&sbi->s_lock);

	if (!parse_options(data, sb, &new_opt))
		return -EINVAL;

	spin_lock(&sbi->s_lock);
	es = sbi->s_es;
	if ((bool)(*flags & SB_RDONLY) == sb_rdonly(sb))
		goto out_set;
	if (*flags & SB_RDONLY) {
		if (le16_to_cpu(es->s_state) & EXT2_VALID_FS ||
		    !(sbi->s_mount_state & EXT2_VALID_FS))
			goto out_set;

		/*
		 * OK, we are remounting a valid rw partition rdonly, so set
		 * the rdonly flag and then mark the partition as valid again.
		 */
		es->s_state = cpu_to_le16(sbi->s_mount_state);
		es->s_mtime = cpu_to_le32(ktime_get_real_seconds());
		spin_unlock(&sbi->s_lock);

		ext2_sync_super(sb, es, 1);
	} else {
		/*
		 * Mounting a RDONLY partition read-write, so reread and
		 * store the current valid flag.  (It may have been changed
		 * by e2fsck since we originally mounted the partition.)
		 */
		sbi->s_mount_state = le16_to_cpu(es->s_state);
		if (!ext2_check_super(sb, es, 0))
			sb->s_flags &= ~SB_RDONLY;
		spin_unlock(&sbi->s_lock);

		ext2_write_super(sb);
	}

	spin_lock(&sbi->s_lock);
out_set:
	sbi->s_mount_opt = new_opt;
	spin_unlock(&sbi->s_lock);

	return 0;
}

static int ext2_show_options(struct seq_file *seq, struct dentry *root)
{
	struct super_block *sb = root->d_sb;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block *es = sbi->s_es;

	spin_lock(&sbi->s_lock);

	if (test_opt(sb, ERRORS_RO)) {
		int def_errors = le16_to_cpu(es->s_errors);
		if (def_errors == EXT2_ERRORS_PANIC || def_errors == EXT2_ERRORS_CONTINUE)
			seq_puts(seq, ",errors=remount-ro");
	}
	if (test_opt(sb, ERRORS_CONT))
		seq_puts(seq, ",errors=continue");
	if (test_opt(sb, ERRORS_PANIC))
		seq_puts(seq, ",errors=panic");
	if (test_opt(sb, DEBUG))
		seq_puts(seq, ",debug");

	spin_unlock(&sbi->s_lock);
	return 0;
}

static const struct super_operations ext2_sops = {
	.alloc_inode  = ext2_alloc_inode,
	.free_inode   = ext2_free_inode_sb,
	.write_inode  = ext2_write_inode, // defined in inode.c
	.evict_inode  = ext2_evict_inode, // defined in inode.c
	.put_super    = ext2_put_super,
	.sync_fs      = ext2_sync_fs,
	.statfs       = ext2_statfs,
	.remount_fs   = ext2_remount,
	.show_options = ext2_show_options,
};

static int ext2_fill_super(struct super_block *sb, void *data, int silent)
{
	struct buffer_head *bh;
	struct ext2_sb_info *sbi;
	struct ext2_super_block *es;
	struct inode *root;
	unsigned long block, sb_block = 1, logic_sb_block, offset = 0, mount_opt = 0;
	long ret = -ENOMEM;
	int blocksize = BLOCK_SIZE;
	int db_count, i, j, err;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		goto failed;

	sbi->s_blockgroup_lock = kzalloc(sizeof(struct blockgroup_lock), GFP_KERNEL);
	if (!sbi->s_blockgroup_lock) {
		kfree(sbi);
		goto failed;
	}

	sb->s_fs_info = sbi;
	sbi->s_sb_block = sb_block;
	spin_lock_init(&sbi->s_lock);
	ret = -EINVAL;

	/* Set the blocksize to read the super block */
	blocksize = sb_min_blocksize(sb, BLOCK_SIZE);
	if (!blocksize) {
		ext2_msg(sb, KERN_ERR, "error: unable to set blocksize");
		goto failed_sbi;
	}

	/*
	 * If the superblock doesn't start on a hardware sector boundary,
	 * calculate the offset.  
	 */
	logic_sb_block = sb_block;
	offset = 0;
	if (blocksize != BLOCK_SIZE) {
		logic_sb_block = (sb_block * BLOCK_SIZE) / blocksize;
		offset = (sb_block * BLOCK_SIZE) % blocksize;
	}

	bh = sb_bread(sb, logic_sb_block);
	if (!bh) {
		ext2_msg(sb, KERN_ERR, "error: unable to read superblock");
		goto failed_sbi;
	}

	/*
	 * Note: s_es must be initialized as soon as possible because
	 * some ext2 macro-instructions depend on its value
	 */
	es = (struct ext2_super_block *)(((char *)bh->b_data) + offset);
	sbi->s_es = es;
	sb->s_magic = le16_to_cpu(es->s_magic);
	if (sb->s_magic != EXT2_SUPER_MAGIC)
		goto cantfind_ext2;

	/* We do not support reading default mount options */
	if (le32_to_cpu(es->s_default_mount_opts))
		ext2_msg(sb, KERN_WARNING, "warning: ignoring default mount options");

	if (le16_to_cpu(sbi->s_es->s_errors) == EXT2_ERRORS_PANIC)
		set_opt(mount_opt, ERRORS_PANIC);
	else if (le16_to_cpu(sbi->s_es->s_errors) == EXT2_ERRORS_CONTINUE)
		set_opt(mount_opt, ERRORS_CONT);
	else
		set_opt(mount_opt, ERRORS_RO);

	if (!parse_options((char *)data, sb, &mount_opt))
		goto failed_mount;

	sbi->s_mount_opt = mount_opt;

	/* In ext2-lite we do not support any set of features. */
	if (es->s_feature_ro_compat || es->s_feature_compat || es->s_feature_incompat) {
		ext2_msg(sb, KERN_ERR, "error: couldn't mount because of unsupported features");
		goto failed_mount;
	}

	blocksize = BLOCK_SIZE << le32_to_cpu(sbi->s_es->s_log_block_size);

	/* If the blocksize doesn't match, re-read the thing.. */
	if (sb->s_blocksize != blocksize) {
		brelse(bh);

		if (!sb_set_blocksize(sb, blocksize)) {
			ext2_msg(sb, KERN_ERR, "error: bad blocksize %d", blocksize);
			goto failed_sbi;
		}

		logic_sb_block = (sb_block*BLOCK_SIZE) / blocksize;
		offset = (sb_block*BLOCK_SIZE) % blocksize;
		bh = sb_bread(sb, logic_sb_block);
		if(!bh) {
			ext2_msg(sb, KERN_ERR, "error: couldn't read superblock on 2nd try");
			goto failed_sbi;
		}

		es = (struct ext2_super_block *)(((char *)bh->b_data) + offset);
		sbi->s_es = es;
		if (es->s_magic != cpu_to_le16(EXT2_SUPER_MAGIC)) {
			ext2_msg(sb, KERN_ERR, "error: magic mismatch");
			goto failed_mount;
		}
	}

 	//> In ext2-lite we only currently support direct blocks.
	sb->s_maxbytes = EXT2_NDIR_BLOCKS << sb->s_blocksize_bits;
	sb->s_max_links = EXT2_LINK_MAX;
	sb->s_time_min = S32_MIN;
	sb->s_time_max = S32_MAX;

	if (le32_to_cpu(es->s_rev_level) == EXT2_GOOD_OLD_REV) {
		sbi->s_inode_size = EXT2_GOOD_OLD_INODE_SIZE;
		sbi->s_first_ino = EXT2_GOOD_OLD_FIRST_INO;
	} else {
		sbi->s_inode_size = le16_to_cpu(es->s_inode_size);
		sbi->s_first_ino = le32_to_cpu(es->s_first_ino);
		if ((sbi->s_inode_size < EXT2_GOOD_OLD_INODE_SIZE) ||
		    !is_power_of_2(sbi->s_inode_size) ||
		    (sbi->s_inode_size > blocksize)) {
			ext2_msg(sb, KERN_ERR, "error: unsupported inode size: %d",
			                        sbi->s_inode_size);
			goto failed_mount;
		}
	}

	sbi->s_blocks_per_group = le32_to_cpu(es->s_blocks_per_group);
	sbi->s_inodes_per_group = le32_to_cpu(es->s_inodes_per_group);

	sbi->s_inodes_per_block = sb->s_blocksize / EXT2_INODE_SIZE(sb);
	if (sbi->s_inodes_per_block == 0 || sbi->s_inodes_per_group == 0)
		goto cantfind_ext2;
	sbi->s_itb_per_group = sbi->s_inodes_per_group / sbi->s_inodes_per_block;
	sbi->s_desc_per_block = sb->s_blocksize / sizeof(struct ext2_group_desc);
	sbi->s_sbh = bh;
	sbi->s_mount_state = le16_to_cpu(es->s_state);
	sbi->s_addr_per_block_bits = ilog2(EXT2_ADDR_PER_BLOCK(sb));
	sbi->s_desc_per_block_bits = ilog2(EXT2_DESC_PER_BLOCK(sb));

	if (sb->s_magic != EXT2_SUPER_MAGIC)
		goto cantfind_ext2;

	if (sb->s_blocksize != bh->b_size) {
		ext2_msg(sb, KERN_ERR, "error: unsupported blocksize");
		goto failed_mount;
	}

	if (sbi->s_blocks_per_group > sb->s_blocksize * 8) {
		ext2_msg(sb, KERN_ERR, "error: #blocks per group too big: %lu",
		         sbi->s_blocks_per_group);
		goto failed_mount;
	}
	if (sbi->s_inodes_per_group > sb->s_blocksize * 8) {
		ext2_msg(sb, KERN_ERR, "error: #inodes per group too big: %lu",
		         sbi->s_inodes_per_group);
		goto failed_mount;
	}

	if (EXT2_BLOCKS_PER_GROUP(sb) == 0)
		goto cantfind_ext2;

	sbi->s_groups_count = ((le32_to_cpu(es->s_blocks_count) -
	                        le32_to_cpu(es->s_first_data_block) - 1)
	                      / EXT2_BLOCKS_PER_GROUP(sb)) + 1;
	db_count = (sbi->s_groups_count + EXT2_DESC_PER_BLOCK(sb) - 1) /
	           EXT2_DESC_PER_BLOCK(sb);
	sbi->s_group_desc = kmalloc_array(db_count, sizeof(struct buffer_head *),
	                                  GFP_KERNEL);
	if (sbi->s_group_desc == NULL) {
		ret = -ENOMEM;
		ext2_msg(sb, KERN_ERR, "error: not enough memory");
		goto failed_mount;
	}
	bgl_lock_init(sbi->s_blockgroup_lock);
	for (i = 0; i < db_count; i++) {
		block = descriptor_loc(sb, logic_sb_block, i);
		sbi->s_group_desc[i] = sb_bread(sb, block);
		if (!sbi->s_group_desc[i]) {
			for (j = 0; j < i; j++)
				brelse (sbi->s_group_desc[j]);
			ext2_msg(sb, KERN_ERR, "error: unable to read group descriptors");
			goto failed_mount_group_desc;
		}
	}
	if (!ext2_check_descriptors(sb)) {
		ext2_msg(sb, KERN_ERR, "group descriptors corrupted");
		goto failed_mount2;
	}
	sbi->s_gdb_count = db_count;

	err = percpu_counter_init(&sbi->s_freeblocks_counter,
	                          ext2_count_free_blocks(sb), GFP_KERNEL);
	if (!err)
		err = percpu_counter_init(&sbi->s_freeinodes_counter,
		                          ext2_count_free_inodes(sb), GFP_KERNEL);
	if (!err)
		err = percpu_counter_init(&sbi->s_dirs_counter,
		                          ext2_count_dirs(sb), GFP_KERNEL);
	if (err) {
		ret = err;
		ext2_msg(sb, KERN_ERR, "error: insufficient memory");
		goto failed_mount3;
	}

	/* set up enough so that it can read an inode */
	sb->s_op = &ext2_sops;

	root = ext2_iget(sb, EXT2_ROOT_INO);
	if (IS_ERR(root)) {
		ret = PTR_ERR(root);
		goto failed_mount3;
	}
	if (!S_ISDIR(root->i_mode) || !root->i_blocks || !root->i_size) {
		iput(root);
		ext2_msg(sb, KERN_ERR, "error: corrupt root inode, run e2fsck");
		goto failed_mount3;
	}

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		ext2_msg(sb, KERN_ERR, "error: get root inode failed");
		ret = -ENOMEM;
		goto failed_mount3;
	}

	if (ext2_check_super(sb, es, sb_rdonly(sb)))
		sb->s_flags |= SB_RDONLY;
	ext2_write_super(sb);

	return 0;

cantfind_ext2:
	if (!silent)
		ext2_msg(sb, KERN_ERR, "error: can't find an ext2 filesystem on dev %s.", sb->s_id);
	goto failed_mount;
failed_mount3:
	percpu_counter_destroy(&sbi->s_freeblocks_counter);
	percpu_counter_destroy(&sbi->s_freeinodes_counter);
	percpu_counter_destroy(&sbi->s_dirs_counter);
failed_mount2:
	for (i = 0; i < db_count; i++)
		brelse(sbi->s_group_desc[i]);
failed_mount_group_desc:
	kfree(sbi->s_group_desc);
failed_mount:
	brelse(bh);
failed_sbi:
	sb->s_fs_info = NULL;
	kfree(sbi->s_blockgroup_lock);
	kfree(sbi);
failed:
	return ret;
}

static struct dentry *ext2_mount(struct file_system_type *fs_type,
                                 int flags, const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, ext2_fill_super);
}

static struct file_system_type ext2_fs_type = {
	.owner    = THIS_MODULE,
	.name     = "ext2-lite",
	.mount    = ext2_mount,
	.kill_sb  = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
};
MODULE_ALIAS_FS("ext2-lite");

static int __init init_ext2_fs(void)
{
	int err = init_inodecache();
	if (err)
		return err;

	/* Register ext2-lite filesystem in the kernel */
	/* If an error occurs remember to call destroy_inodecache() */
	/* ? */
	err = register_filesystem(&ext2_fs_type);
	if (err)
		destroy_inodecache();

	return err;
}

static void __exit exit_ext2_fs(void)
{
	/* Unregister ext2-lite filesystem from the kernel */
	/* ? */
	unregister_filesystem(&ext2_fs_type);
	destroy_inodecache();
}

MODULE_AUTHOR("ADD YOUR NAME HERE"); /* ? */
MODULE_DESCRIPTION("Second Extended Filesystem Lite Version from CSLab");
MODULE_LICENSE("GPL");
module_init(init_ext2_fs)
module_exit(exit_ext2_fs)
