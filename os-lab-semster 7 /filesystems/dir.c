// SPDX-License-Identifier: GPL-2.0
/*
 * dir.c
 *
 * This file contains the routines that handle directory operations.
 *
 * Dimitris Siakavaras <jimsiak@cslab.ece.ntua.gr>
 *
 */

#include <linux/buffer_head.h>
#include <linux/iversion.h>
#include "ext2.h"

/*
 * ext2 uses block-sized chunks. Arguably, sector-sized ones would be
 * more robust, but we have what we have
 */
static inline unsigned ext2_chunk_size(struct inode *inode)
{
	return inode->i_sb->s_blocksize;
}

/*
 * Return the offset into page `page_nr' of the last valid
 * byte in that page, plus one.
 */
static unsigned ext2_last_byte(struct inode *inode, unsigned long page_nr)
{
	unsigned last_byte = inode->i_size;

	last_byte -= page_nr << PAGE_SHIFT;
	if (last_byte > PAGE_SIZE)
		last_byte = PAGE_SIZE;
	return last_byte;
}

static void ext2_commit_chunk(struct folio *folio, loff_t pos, unsigned len)
{
	struct address_space *mapping = folio->mapping;
	struct inode *dir = mapping->host;
	int err = 0;

	inode_inc_iversion(dir);
	block_write_end(NULL, mapping, pos, len, len, &folio->page, NULL);

	if (pos+len > dir->i_size) {
		i_size_write(dir, pos+len);
		mark_inode_dirty(dir);
	}
	folio_unlock(folio);
}

static bool ext2_check_folio(struct folio *folio, int quiet, char *kaddr)
{
	struct inode *dir = folio->mapping->host;
	struct super_block *sb = dir->i_sb;
	unsigned chunk_size = ext2_chunk_size(dir);
	u32 max_inumber = le32_to_cpu(EXT2_SB(sb)->s_es->s_inodes_count);
	unsigned offs, rec_len;
	unsigned limit = folio_size(folio);
	ext2_dirent *p;
	char *error;

	if (dir->i_size < folio_pos(folio) + limit) {
		limit = offset_in_folio(folio, dir->i_size);
		if (limit & (chunk_size - 1))
			goto Ebadsize;
		if (!limit)
			goto out;
	}
	for (offs = 0; offs <= limit - EXT2_DIR_REC_LEN(1); offs += rec_len) {
		p = (ext2_dirent *)(kaddr + offs);
		rec_len = le16_to_cpu(p->rec_len);

		if (unlikely(rec_len < EXT2_DIR_REC_LEN(1)))
			goto Eshort;
		if (unlikely(rec_len & 3))
			goto Ealign;
		if (unlikely(rec_len < EXT2_DIR_REC_LEN(p->name_len)))
			goto Enamelen;
		if (unlikely(((offs + rec_len - 1) ^ offs) & ~(chunk_size-1)))
			goto Espan;
		if (unlikely(le32_to_cpu(p->inode) > max_inumber))
			goto Einumber;
	}
	if (offs != limit)
		goto Eend;
out:
	folio_set_checked(folio);
	return true;

Ebadsize:
	if (!quiet)
		ext2_error(sb, __func__,
			"size of directory #%lu is not a multiple "
			"of chunk size", dir->i_ino);
	goto fail;
Eshort:
	error = "rec_len is smaller than minimal";
	goto bad_entry;
Ealign:
	error = "unaligned directory entry";
	goto bad_entry;
Enamelen:
	error = "rec_len is too small for name_len";
	goto bad_entry;
Espan:
	error = "directory entry across blocks";
	goto bad_entry;
Einumber:
	error = "inode out of bounds";
bad_entry:
	if (!quiet)
		ext2_error(sb, __func__, "bad entry in directory #%lu: : %s - "
			"offset=%llu, inode=%lu, rec_len=%d, name_len=%d",
			dir->i_ino, error, folio_pos(folio) + offs,
			(unsigned long) le32_to_cpu(p->inode),
			rec_len, p->name_len);
	goto fail;
Eend:
	if (!quiet) {
		p = (ext2_dirent *)(kaddr + offs);
		ext2_error(sb, __func__, "entry in directory #%lu spans the page boundary offset=%llu, inode=%lu",
			dir->i_ino, folio_pos(folio) + offs,
			(unsigned long) le32_to_cpu(p->inode));
	}
fail:
	return false;
}

static void *ext2_get_folio(struct inode *dir, unsigned long n,
                            int quiet, struct folio **foliop)
{
	struct address_space *mapping = dir->i_mapping;
	struct folio *folio = read_mapping_folio(mapping, n, NULL);
	void *kaddr;

	if (IS_ERR(folio))
		return ERR_CAST(folio);
	kaddr = kmap_local_folio(folio, 0);
	if (unlikely(!folio_test_checked(folio))) {
		if (!ext2_check_folio(folio, quiet, kaddr))
			goto fail;
	}
	*foliop = folio;
	return kaddr;
fail:
	folio_release_kmap(folio, kaddr);
	return ERR_PTR(-EIO);
}

/*
 * NOTE! unlike strncmp, ext2_match returns 1 for success, 0 for failure.
 *
 * len <= EXT2_NAME_LEN and de != NULL are guaranteed by caller.
 */
static inline int ext2_match(int len, const char * const name, ext2_dirent *de)
{
	if (len != de->name_len)
		return 0;
	if (!de->inode)
		return 0;
	return !memcmp(name, de->name, len);
}

static inline ext2_dirent *ext2_next_entry(ext2_dirent *p)
{
	return (ext2_dirent *)((char *)p + le16_to_cpu(p->rec_len));
}

static inline unsigned ext2_validate_entry(char *base, unsigned offset,
                                           unsigned mask)
{
	ext2_dirent *de = (ext2_dirent*)(base + offset);
	ext2_dirent *p = (ext2_dirent*)(base + (offset&mask));
	while ((char*)p < (char*)de) {
		if (p->rec_len == 0)
			break;
		p = ext2_next_entry(p);
	}
	return (char *)p - base;
}

static int ext2_readdir(struct file *file, struct dir_context *ctx)
{
	loff_t pos = ctx->pos;
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	unsigned int offset = pos & ~PAGE_MASK;
	unsigned long n = pos >> PAGE_SHIFT;
	unsigned long npages = dir_pages(inode);
	unsigned chunk_mask = ~(ext2_chunk_size(inode)-1);
	bool need_revalidate = !inode_eq_iversion(inode, file->f_version);

	if (pos > inode->i_size - EXT2_DIR_REC_LEN(1))
		return 0;

	for ( ; n < npages; n++, offset = 0) {
		ext2_dirent *de;
		struct folio *folio;
		char *kaddr = ext2_get_folio(inode, n, 0, &folio);
		char *limit;

		if (IS_ERR(kaddr)) {
			ext2_error(sb, __func__, "bad page in #%lu", inode->i_ino);
			ctx->pos += PAGE_SIZE - offset;
			return PTR_ERR(kaddr);
		}
		if (unlikely(need_revalidate)) {
			if (offset) {
				offset = ext2_validate_entry(kaddr, offset, chunk_mask);
				ctx->pos = (n<<PAGE_SHIFT) + offset;
			}
			file->f_version = inode_query_iversion(inode);
			need_revalidate = false;
		}
		de = (ext2_dirent *)(kaddr+offset);
		limit = kaddr + ext2_last_byte(inode, n) - EXT2_DIR_REC_LEN(1);
		for ( ; (char*)de <= limit; de = ext2_next_entry(de)) {
			if (de->rec_len == 0) {
				ext2_error(sb, __func__, "zero-length directory entry");
				folio_release_kmap(folio, de);
				return -EIO;
			}
			if (de->inode) {
				unsigned char d_type = DT_UNKNOWN;

				if (!dir_emit(ctx, de->name, de->name_len,
				              le32_to_cpu(de->inode), d_type))
				{
					folio_release_kmap(folio, de);
					return 0;
				}
			}
			ctx->pos += le16_to_cpu(de->rec_len);
		}
		folio_release_kmap(folio, kaddr);
	}
	return 0;
}

/*
 * finds an entry in the specified directory with the wanted name.
 * It returns a pointer to the folio in which the entry was found (as a
 * parameter - foliop), and the entry itself.
 * Folio is returned mapped and unlocked.
 * Entry is guaranteed to be valid.
 */
ext2_dirent *ext2_find_entry(struct inode *dir, const struct qstr *child,
                             struct folio **foliop)
{
	const char *name = child->name;
	int namelen = child->len;
	unsigned reclen = EXT2_DIR_REC_LEN(namelen);
	unsigned long npages = dir_pages(dir);
	unsigned long i;
	ext2_dirent *de;
	char *kaddr;

	if (npages == 0)
		return ERR_PTR(-ENOENT);

	/* Scan all the pages of the directory to find the requested name. */
	for (i=0; i < npages; i++) {
	
		/* ? */
		kaddr = ext2_get_folio(dir, i, 0, foliop);
		if (IS_ERR(kaddr))
			continue;

		ext2_dirent *de = (ext2_dirent *)kaddr;
		char *limit = kaddr + ext2_last_byte(dir, i) - EXT2_DIR_REC_LEN(1);

		while ((char *)de <= limit) {
			if (de->rec_len == 0) {
				folio_release_kmap(*foliop, kaddr);
				return ERR_PTR(-EIO);
			}
			if (ext2_match(namelen, name, de))
				return de;
			de = ext2_next_entry(de);
		}
		folio_release_kmap(*foliop, kaddr);
	}
	return ERR_PTR(-ENOENT);
}

ext2_dirent *ext2_dotdot(struct inode *dir, struct folio **foliop)
{
	ext2_dirent *de = ext2_get_folio(dir, 0, 0, foliop);

	if (!IS_ERR(de))
		return ext2_next_entry(de);
	return de;
}

int ext2_inode_by_name(struct inode *dir, const struct qstr *child, ino_t *ino)
{
	ext2_dirent *de;
	struct folio *folio;
	
	de = ext2_find_entry(dir, child, &folio);
	if (IS_ERR(de))
		return PTR_ERR(de);

	*ino = le32_to_cpu(de->inode);
	folio_release_kmap(folio, de);
	return 0;
}

static int ext2_prepare_chunk(struct folio *folio, loff_t pos, unsigned len)
{
	return __block_write_begin(&folio->page, pos, len, ext2_get_block);
}

int ext2_set_link(struct inode *dir, ext2_dirent *de,
		  struct folio *folio, struct inode *inode, bool update_times)
{
	loff_t pos = folio_pos(folio) + offset_in_folio(folio, de);
	unsigned len = le16_to_cpu(de->rec_len);
	int err;

	folio_lock(folio);
	err = ext2_prepare_chunk(folio, pos, len);
	if (err) {
		folio_unlock(folio);
		return err;
	}
	de->inode = cpu_to_le32(inode->i_ino);
	de->file_type = 0;
	ext2_commit_chunk(folio, pos, len);
	if (update_times)
		inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);

	err = filemap_write_and_wait(dir->i_mapping);
	if (!err)
		err = sync_inode_metadata(dir, 1);
	return err;
}

/*
 * dentry->d_parent inode is locked by the VFS code.
 */
int ext2_add_link(struct dentry *dentry, struct inode *inode)
{
	struct inode *dir = d_inode(dentry->d_parent);
	const char *name = dentry->d_name.name;
	int namelen = dentry->d_name.len;
	unsigned chunk_size = ext2_chunk_size(dir);
	unsigned reclen = EXT2_DIR_REC_LEN(namelen);
	unsigned short rec_len, name_len;
	struct folio *folio = NULL;
	ext2_dirent *de;
	unsigned long npages = dir_pages(dir);
	unsigned long n;
	loff_t pos;
	int err;

	/*
	 * We take care of directory expansion in the same loop.
	 * This code plays outside i_size, so it locks the page
	 * to protect that region.
	 */
	for (n = 0; n <= npages; n++) {
		char *kaddr = ext2_get_folio(dir, n, 0, &folio);
		char *dir_end;

		if (IS_ERR(kaddr))
			return PTR_ERR(kaddr);
		folio_lock(folio);
		dir_end = kaddr + ext2_last_byte(dir, n);
		de = (ext2_dirent *)kaddr;
		kaddr += folio_size(folio) - reclen;
		while ((char *)de <= kaddr) {
			if ((char *)de == dir_end) {
				/* We hit i_size */
				name_len = 0;
				rec_len = chunk_size;
				de->rec_len = cpu_to_le16(chunk_size);
				de->inode = 0;
				goto got_it;
			}
			if (de->rec_len == 0) {
				ext2_error(dir->i_sb, __func__,
					"zero-length directory entry");
				err = -EIO;
				goto out_unlock;
			}
			err = -EEXIST;
			if (ext2_match(namelen, name, de))
				goto out_unlock;
			name_len = EXT2_DIR_REC_LEN(de->name_len);
			rec_len = le16_to_cpu(de->rec_len);
			if (!de->inode && rec_len >= reclen)
				goto got_it;
			if (rec_len >= name_len + reclen)
				goto got_it;
			de = (ext2_dirent *)((char *) de + rec_len);
		}
		folio_unlock(folio);
		folio_release_kmap(folio, kaddr);
	}
	BUG();
	return -EINVAL;

got_it:
	pos = folio_pos(folio) + offset_in_folio(folio, de);
	err = ext2_prepare_chunk(folio, pos, rec_len);
	if (err)
		goto out_unlock;
	if (de->inode) {
		ext2_dirent *de1 = (ext2_dirent *)((char *) de + name_len);
		de1->rec_len = cpu_to_le16(rec_len - name_len);
		de->rec_len = cpu_to_le16(name_len);
		de = de1;
	}
	de->name_len = namelen;
	memcpy(de->name, name, namelen);
	de->inode = cpu_to_le32(inode->i_ino);
	de->file_type = 0;
	ext2_commit_chunk(folio, pos, rec_len);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	mark_inode_dirty(dir);
	err = filemap_write_and_wait(dir->i_mapping);
	if (!err)
		err = sync_inode_metadata(dir, 1);
out_put:
	folio_release_kmap(folio, de);
	return err;
out_unlock:
	folio_unlock(folio);
	goto out_put;
}

/*
 * ext2_delete_entry deletes a directory entry by merging it with the
 * previous entry. Page is up-to-date. Releases the page.
 */
int ext2_delete_entry(ext2_dirent *dir, struct folio *folio)
{
	struct inode *inode = folio->mapping->host;
	size_t from, to;
	char *kaddr;
	loff_t pos;
	ext2_dirent *de, *pde = NULL;
	int err;

	from = offset_in_folio(folio, dir);
	to = from + le16_to_cpu(dir->rec_len);
	kaddr = (char *)dir - from;
	from &= ~(ext2_chunk_size(inode)-1);
	de = (ext2_dirent *)(kaddr + from);

	while ((char*)de < (char*)dir) {
		if (de->rec_len == 0) {
			ext2_error(inode->i_sb, __func__, "zero-length directory entry");
			return -EIO;
		}
		pde = de;
		de = ext2_next_entry(de);
	}
	if (pde)
		from = offset_in_folio(folio, pde);
	pos = folio_pos(folio) + from;
	folio_lock(folio);
	err = ext2_prepare_chunk(folio, pos, to - from);
	if (err) {
		folio_unlock(folio);
		return err;
	}
	if (pde)
		pde->rec_len = cpu_to_le16(to - from);
	dir->inode = 0;
	ext2_commit_chunk(folio, pos, to - from);
	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	mark_inode_dirty(inode);

	err = filemap_write_and_wait(inode->i_mapping);
	if (!err)
		err = sync_inode_metadata(inode, 1);
	return err;
}

/*
 * Set the first fragment of directory.
 */
int ext2_make_empty(struct inode *inode, struct inode *parent)
{
	struct folio *folio= filemap_grab_folio(inode->i_mapping, 0);
	unsigned chunk_size = ext2_chunk_size(inode);
	ext2_dirent *de;
	int err;
	void *kaddr;

	if (IS_ERR(folio))
		return PTR_ERR(folio);

	err = ext2_prepare_chunk(folio, 0, chunk_size);
	if (err) {
		folio_unlock(folio);
		goto fail;
	}
	kaddr = kmap_local_folio(folio, 0);
	memset(kaddr, 0, chunk_size);
	de = (ext2_dirent *)kaddr;
	de->name_len = 1;
	de->rec_len = cpu_to_le16(EXT2_DIR_REC_LEN(1));
	memcpy(de->name, ".\0\0", 4);
	de->inode = cpu_to_le32(inode->i_ino);
	de->file_type = 0;

	de = (ext2_dirent*)(kaddr + EXT2_DIR_REC_LEN(1));
	de->name_len = 2;
	de->rec_len = cpu_to_le16(chunk_size - EXT2_DIR_REC_LEN(1));
	de->inode = cpu_to_le32(parent->i_ino);
	memcpy(de->name, "..\0", 4);
	de->file_type = 0;
	kunmap_local(kaddr);
	ext2_commit_chunk(folio, 0, chunk_size);
	err = filemap_write_and_wait(inode->i_mapping);
	if (!err)
		err = sync_inode_metadata(inode, 1);
fail:
	folio_put(folio);
	return err;
}

/*
 * routine to check that the specified directory is empty (for rmdir)
 */
int ext2_empty_dir(struct inode *inode)
{
	struct folio *folio;
	char *kaddr;
	unsigned long i, npages = dir_pages(inode);

	for (i = 0; i < npages; i++) {
		ext2_dirent *de;

		kaddr = ext2_get_folio(inode, i, 0, &folio);
		if (IS_ERR(kaddr))
			return 0;

		de = (ext2_dirent *)kaddr;
		kaddr += ext2_last_byte(inode, i) - EXT2_DIR_REC_LEN(1);
		while ((char *)de <= kaddr) {
			if (de->rec_len == 0) {
				ext2_error(inode->i_sb, __func__,
					"zero-length directory entry");
				printk("kaddr=%p, de=%p\n", kaddr, de);
				goto not_empty;
			}
			if (de->inode != 0) {
				/* check for . and .. */
				if (de->name[0] != '.')
					goto not_empty;
				if (de->name_len > 2)
					goto not_empty;
				if (de->name_len < 2) {
					if (de->inode != cpu_to_le32(inode->i_ino))
						goto not_empty;
				} else if (de->name[1] != '.')
					goto not_empty;
			}
			de = ext2_next_entry(de);
		}
		folio_release_kmap(folio, kaddr);
	}
	return 1;

not_empty:
	folio_release_kmap(folio, kaddr);
	return 0;
}

const struct file_operations ext2_dir_operations = {
	.llseek         = generic_file_llseek,
	.read           = generic_read_dir,
	.iterate_shared = ext2_readdir,
	.fsync          = generic_file_fsync,
};
