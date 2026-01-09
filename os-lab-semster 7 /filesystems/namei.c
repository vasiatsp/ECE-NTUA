// SPDX-License-Identifier: GPL-2.0
/*
 * namei.c
 *
 * This file contains the routines responsible for pathname-to-inode translation.
 *
 * Dimitris Siakavaras <jimsiak@cslab.ece.ntua.gr>
 *
 */

#include <linux/pagemap.h>
#include "ext2.h"

static inline int ext2_add_nondir(struct dentry *dentry, struct inode *inode)
{
	int err = ext2_add_link(dentry, inode);
	if (err) {
		inode_dec_link_count(inode);
		discard_new_inode(inode);
		return err;
	}
	d_instantiate_new(dentry, inode);
	return 0;
}

/*
 * By the time this is called, we already have created
 * the directory cache entry for the new file, but it
 * is so far negative - it has no inode.
 *
 * If the create succeeds, we fill in the inode information
 * with d_instantiate(). 
 */
static int ext2_create(struct mnt_idmap *idmap,
                       struct inode *dir, struct dentry *dentry,
                       umode_t mode, bool excl)
{
	struct inode *inode;

	ext2_debug("hello\n");
	inode = ext2_new_inode(dir, mode);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	inode->i_op = &ext2_file_inode_operations;
	inode->i_fop = &ext2_file_operations;
	inode->i_mapping->a_ops = &ext2_aops;
	mark_inode_dirty(inode);
	ext2_debug("bye\n");
	return ext2_add_nondir(dentry, inode);
}

static struct dentry *ext2_lookup(struct inode *dir, struct dentry *dentry,
                                  unsigned int flags)
{
	struct inode *inode;
	ino_t ino;
	int res;
	
	ext2_debug("hello name: %s\n", dentry->d_name.name);
	if (dentry->d_name.len > EXT2_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	res = ext2_inode_by_name(dir, &dentry->d_name, &ino);
	if (res) {
		if (res != -ENOENT)
			return ERR_PTR(res);
		inode = NULL;
	} else {
		inode = ext2_iget(dir->i_sb, ino);
		if (inode == ERR_PTR(-ESTALE)) {
			ext2_error(dir->i_sb, __func__, "deleted inode referenced: %lu",
			           (unsigned long) ino);
			return ERR_PTR(-EIO);
		}
	}
	return d_splice_alias(inode, dentry);
}

static int ext2_link(struct dentry *old_dentry, struct inode *dir,
                     struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	int err;

	ext2_debug("old_dentry: %s dentry: %s\n", old_dentry->d_name.name, dentry->d_name.name);
	inode_set_ctime_current(inode);
	inode_inc_link_count(inode);
	ihold(inode);

	err = ext2_add_link(dentry, inode);
	if (err) {
		inode_dec_link_count(inode);
		iput(inode);
		return err;
	}

	d_instantiate(dentry, inode);
	return 0;
}

static int ext2_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	ext2_dirent *de;
	struct folio *folio;
	int err;

	de = ext2_find_entry(dir, &dentry->d_name, &folio);
	if (IS_ERR(de)) {
		err = PTR_ERR(de);
		goto out;
	}

	err = ext2_delete_entry(de, folio);
	if (err)
		goto out;

	inode_set_ctime_to_ts(inode, inode_get_ctime(dir));
	inode_dec_link_count(inode);
	err = 0;
out:
	return err;
}

static int ext2_symlink(struct mnt_idmap *idmap,
                        struct inode *dir, struct dentry *dentry,
                        const char *symname)
{
	struct super_block *sb = dir->i_sb;
	int err = -ENAMETOOLONG;
	unsigned l = strlen(symname)+1;
	struct inode *inode;

	ext2_debug("hello\n");
	if (l > sb->s_blocksize)
		goto out;

	inode = ext2_new_inode(dir, S_IFLNK | S_IRWXUGO);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out;

	if (l > sizeof (EXT2_I(inode)->i_data)) {
		/* slow symlink */
		inode->i_op = &page_symlink_inode_operations;
		inode_nohighmem(inode);
		inode->i_mapping->a_ops = &ext2_aops;
		err = page_symlink(inode, symname, l);
		if (err)
			goto out_fail;
	} else {
		/* fast symlink */
		inode->i_op = &simple_symlink_inode_operations;
		inode->i_link = (char*)EXT2_I(inode)->i_data;
		memcpy(inode->i_link, symname, l);
		inode->i_size = l-1;
	}
	mark_inode_dirty(inode);

	err = ext2_add_nondir(dentry, inode);
	goto out;

out_fail:
	inode_dec_link_count(inode);
	discard_new_inode(inode);
	ext2_debug("bye\n");
out:
	return err;
}

static int ext2_mkdir(struct mnt_idmap *idmap,
                      struct inode *dir, struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int err;

	inode_inc_link_count(dir);

	inode = ext2_new_inode(dir, S_IFDIR | mode);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		goto out_dir;

	inode->i_op = &ext2_dir_inode_operations;
	inode->i_fop = &ext2_dir_operations;
	inode->i_mapping->a_ops = &ext2_aops;

	inode_inc_link_count(inode);

	err = ext2_make_empty(inode, dir);
	if (err)
		goto out_fail;

	err = ext2_add_link(dentry, inode);
	if (err)
		goto out_fail;

	d_instantiate_new(dentry, inode);
	goto out;

out_fail:
	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	discard_new_inode(inode);
out_dir:
	inode_dec_link_count(dir);
out:
	return err;
}

static int ext2_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int err;

	if (!ext2_empty_dir(inode))
		return -ENOTEMPTY;

	err = ext2_unlink(dir, dentry);
	if (err)
		return err;

	inode->i_size = 0;
	inode_dec_link_count(inode);
	inode_dec_link_count(dir);
	return 0;
}

static int ext2_mknod(struct mnt_idmap *idmap,
                      struct inode *dir, struct dentry *dentry,
                      umode_t mode, dev_t rdev)
{
	struct inode * inode;
	int err;

	inode = ext2_new_inode(dir, mode);
	err = PTR_ERR(inode);
	if (IS_ERR(inode))
		return err;

	init_special_inode(inode, inode->i_mode, rdev);
	inode->i_op = &ext2_special_inode_operations;
	mark_inode_dirty(inode);
	err = ext2_add_nondir(dentry, inode);
	return err;
}

static int ext2_rename(struct mnt_idmap *idmap,
                       struct inode *old_dir, struct dentry *old_dentry,
                       struct inode *new_dir, struct dentry *new_dentry,
                       unsigned int flags)
{
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	struct folio *dir_folio = NULL;
	ext2_dirent *dir_de = NULL;
	struct folio *old_folio;
	ext2_dirent *old_de;
	int err;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	old_de = ext2_find_entry(old_dir, &old_dentry->d_name, &old_folio);
	if (IS_ERR(old_de))
		return PTR_ERR(old_de);

	if (S_ISDIR(old_inode->i_mode) && old_dir != new_dir) {
		err = -EIO;
		dir_de = ext2_dotdot(old_inode, &dir_folio);
		if (!dir_de)
			goto out_old;
	}

	if (new_inode) {
		struct folio *new_folio;
		ext2_dirent *new_de;

		err = -ENOTEMPTY;
		if (dir_de && !ext2_empty_dir(new_inode))
			goto out_dir;

		new_de = ext2_find_entry(new_dir, &new_dentry->d_name, &new_folio);
		if (IS_ERR(new_de)) {
			err = PTR_ERR(new_de);
			goto out_dir;
		}
		err = ext2_set_link(new_dir, new_de, new_folio, old_inode, true);
		folio_release_kmap(new_folio, new_de);
		if (err)
			goto out_dir;
		inode_set_ctime_current(new_inode);
		if (dir_de)
			drop_nlink(new_inode);
		inode_dec_link_count(new_inode);
	} else {
		err = ext2_add_link(new_dentry, old_inode);
		if (err)
			goto out_dir;
		if (dir_de)
			inode_inc_link_count(new_dir);
	}

	// Like most other Unix systems, set the ctime for inodes on a rename.
	inode_set_ctime_current(old_inode);
	mark_inode_dirty(old_inode);

	err = ext2_delete_entry(old_de, old_folio);
	if (!err && dir_de) {
		if (old_dir != new_dir)
			err = ext2_set_link(old_inode, dir_de, dir_folio, new_dir, false);
		inode_dec_link_count(old_dir);
	}

out_dir:
	if (dir_de)
		folio_release_kmap(dir_folio, dir_de);
out_old:
	folio_release_kmap(old_folio, old_de);
	return err;
}

const struct inode_operations ext2_dir_inode_operations = {
	.create  = ext2_create,
	.lookup  = ext2_lookup,
	.link    = ext2_link,
	.unlink  = ext2_unlink,
	.symlink = ext2_symlink,
	.mkdir   = ext2_mkdir,
	.rmdir   = ext2_rmdir,
	.mknod   = ext2_mknod,
	.rename  = ext2_rename,
	.getattr = ext2_getattr,
	.setattr = ext2_setattr,
};

const struct inode_operations ext2_special_inode_operations = {
	.getattr = ext2_getattr,
	.setattr = ext2_setattr,
};
