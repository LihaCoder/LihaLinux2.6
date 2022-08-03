/*
 *  linux/fs/ext2/fsync.c
 *
 *  Copyright (C) 1993  Stephen Tweedie (sct@dcs.ed.ac.uk)
 *  from
 *  Copyright (C) 1992  Remy Card (card@masi.ibp.fr)
 *                      Laboratoire MASI - Institut Blaise Pascal
 *                      Universite Pierre et Marie Curie (Paris VI)
 *  from
 *  linux/fs/minix/truncate.c   Copyright (C) 1991, 1992  Linus Torvalds
 * 
 *  ext2fs fsync primitive
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 * 
 *  Removed unnecessary code duplication for little endian machines
 *  and excessive __inline__s. 
 *        Andi Kleen, 1997
 *
 * Major simplications and cleanup - we only need to do the metadata, because
 * we can depend on generic_block_fdatasync() to sync the data blocks.
 */

#include "ext2.h"
#include <linux/smp_lock.h>
#include <linux/buffer_head.h>		/* for fsync_inode_buffers() */


/*
 *	File may be NULL when we are called. Perhaps we shouldn't
 *	even pass file to fsync ?
 */
 
 // fsync回调处
int ext2_sync_file(struct file * file, struct dentry *dentry, int datasync)
{
	struct inode *inode = dentry->d_inode;
	int err;
	
	err  = sync_mapping_buffers(inode->i_mapping);

	// 数据脏了就把元数据信息给落盘。
	// 反之不脏就直接返回.
	if (!(inode->i_state & I_DIRTY))
		return err;

	// 落盘元数据datasync为0，所以就进不去if中。
	// 当执行sys_fdatasync时datasync为1，并且当设置了I_DIRTY_DATASYNC这个标志位时，也把元数据落盘，不然直接返回。
	// 所以发生了重要的写I_DIRTY_DATASYNC这个标志位就会带上。
	if (datasync && !(inode->i_state & I_DIRTY_DATASYNC))
		return err;

	// 元数据信息落盘
	err |= ext2_sync_inode(inode);
	return err ? -EIO : 0;
}
