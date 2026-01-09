// SPDX-License-Identifier: GPL-2.0
/*
 * file.c
 *
 * This file contains everything related to regular files.
 *
 * Dimitris Siakavaras <jimsiak@cslab.ece.ntua.gr>
 *
 */

#include <linux/iomap.h>
#include "ext2.h"

const struct file_operations ext2_file_operations = {
	.llseek            = generic_file_llseek,
	.read_iter         = generic_file_read_iter,
	.write_iter        = generic_file_write_iter,
	.mmap              = generic_file_mmap,
	.fsync             = generic_file_fsync,
	.get_unmapped_area = thp_get_unmapped_area,
	.splice_read       = filemap_splice_read,
	.splice_write      = iter_file_splice_write,
};

const struct inode_operations ext2_file_inode_operations = {
	.getattr = ext2_getattr,
	.setattr = ext2_setattr,
};
