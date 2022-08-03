/*
 *	linux/mm/msync.c
 *
 * Copyright (C) 1994-1999  Linus Torvalds
 */

/*
 * The msync() system call.
 */
#include <linux/slab.h>
#include <linux/pagemap.h>
#include <linux/mm.h>
#include <linux/mman.h>

#include <asm/pgtable.h>
#include <asm/pgalloc.h>
#include <asm/tlbflush.h>

/*
 * Called with mm->page_table_lock held to protect against other
 * threads/the swapper from ripping pte's out from under us.
 */
static int filemap_sync_pte(pte_t *ptep, struct vm_area_struct *vma,
	unsigned long address, unsigned int flags)
{
	pte_t pte = *ptep;

	if (pte_present(pte) && pte_dirty(pte)) {
		struct page *page;

		// 根据pte的描述符+虚拟地址的拆分 得到页帧的基址
		unsigned long pfn = pte_pfn(pte);
		if (pfn_valid(pfn)) {

			page = pfn_to_page(pfn);
			if (!PageReserved(page) && ptep_test_and_clear_dirty(ptep)) {

				// 清空tlb的原因很简单
				// 因为当前页后续会被写到磁盘中，也就是当前页已经不归当前进程所持有
				flush_tlb_page(vma, address);

				// 如果当前页有数据，就标为脏页。
				set_page_dirty(page);
			}
		}
	}
	return 0;
}

static int filemap_sync_pte_range(pmd_t * pmd,
	unsigned long address, unsigned long end, 
	struct vm_area_struct *vma, unsigned int flags)
{
	pte_t *pte;
	int error;

	if (pmd_none(*pmd))
		return 0;
	if (pmd_bad(*pmd)) {
		pmd_ERROR(*pmd);
		pmd_clear(pmd);
		return 0;
	}

	// 如果只找到基址，那么就只需要取到低20位就行。
	// 但是虚拟地址需要切割，因为要找到具体的4byte描述符
	pte = pte_offset_map(pmd, address);
	if ((address & PMD_MASK) != (end & PMD_MASK))
		end = (address & PMD_MASK) + PMD_SIZE;
	error = 0;
	do {
		error |= filemap_sync_pte(pte, vma, address, flags);

		// 因为找完一个页以后，要加1024，顺带把连续的页玩一玩。
		address += PAGE_SIZE;
		pte++;
	} while (address && (address < end));

	pte_unmap(pte - 1);

	return error;
}

static inline int filemap_sync_pmd_range(pgd_t * pgd,
	unsigned long address, unsigned long end, 
	struct vm_area_struct *vma, unsigned int flags)
{
	pmd_t * pmd;
	int error;

	if (pgd_none(*pgd))
		return 0;
	if (pgd_bad(*pgd)) {
		pgd_ERROR(*pgd);
		pgd_clear(pgd);
		return 0;
	}

	// 但是虚拟地址的需要切割
	// 如果只找到pmd基址，那么就只需要取到低20位就行。
	pmd = pmd_offset(pgd, address);
	if ((address & PGDIR_MASK) != (end & PGDIR_MASK))
		end = (address & PGDIR_MASK) + PGDIR_SIZE;
	error = 0;
	do {
		error |= filemap_sync_pte_range(pmd, address, end, vma, flags);
		address = (address + PMD_SIZE) & PMD_MASK;
		pmd++;
	} while (address && (address < end));
	return error;
}

// vma
// address 起始地址
// size 大小
// flags标志位
static int filemap_sync(struct vm_area_struct * vma, unsigned long address,
	size_t size, unsigned int flags)
{
	// 
	pgd_t * dir;
	unsigned long end = address + size;
	int error = 0;

	/* Aquire the lock early; it may be possible to avoid dropping
	 * and reaquiring it repeatedly.
	 */
	spin_lock(&vma->vm_mm->page_table_lock);

	dir = pgd_offset(vma->vm_mm, address);
	flush_cache_range(vma, address, end);
	if (address >= end)
		BUG();
	do {
		// 通过pgd找pmd，
		// 内部再通过pmd找到pte
		error |= filemap_sync_pmd_range(dir, address, end, vma, flags);
		address = (address + PGDIR_SIZE) & PGDIR_MASK;
		dir++;
	} while (address && (address < end));
	flush_tlb_range(vma, end - size, end);

	spin_unlock(&vma->vm_mm->page_table_lock);

	return error;
}

/*
 * MS_SYNC syncs the entire file - including mappings.
 *
 * MS_ASYNC does not start I/O (it used to, up to 2.5.67).  Instead, it just
 * marks the relevant pages dirty.  The application may now run fsync() to
 * write out the dirty pages and wait on the writeout and check the result.
 * Or the application may run fadvise(FADV_DONTNEED) against the fd to start
 * async writeout immediately.
 * So my _not_ starting I/O in MS_ASYNC we provide complete flexibility to
 * applications.
 */

// vma
// start 起始地址
// end 结束地址
// flags 标志位

// 这个方法通过vma获取到真实的页，然后写回？
static int msync_interval(struct vm_area_struct * vma,
	unsigned long start, unsigned long end, int flags)
{
	int ret = 0;
	struct file * file = vma->vm_file;

	if ((flags & MS_INVALIDATE) && (vma->vm_flags & VM_LOCKED))
		return -EBUSY;

	if (file && (vma->vm_flags & VM_SHARED)) {

		// 这个方法来找寻脏页。
		ret = filemap_sync(vma, start, end-start, flags);

		if (!ret && (flags & MS_SYNC)) {
			
			struct inode *inode = file->f_dentry->d_inode;
			int err;

			down(&inode->i_sem);

			// 这里写脏页里面的数据。
			ret = filemap_fdatawrite(inode->i_mapping);
			if (file->f_op && file->f_op->fsync) {

				// fsync写元数据
				err = file->f_op->fsync(file,file->f_dentry,1);
				if (err && !ret)
					ret = err;
			}

			// 等待。
			err = filemap_fdatawait(inode->i_mapping);
			if (!ret)
				ret = err;
			up(&inode->i_sem);
		}
	}
	return ret;
}

// 首先我们先要明白，sys_msync是把当前文件通过mmap映射的页给落盘。
// 所以肯定要找到文件的页。
asmlinkage long sys_msync(unsigned long start, size_t len, int flags)
{
	unsigned long end;
	struct vm_area_struct * vma;
	int unmapped_error, error = -EINVAL;

	down_read(&current->mm->mmap_sem);

	// 三个模式一个没选 直接G
	if (flags & ~(MS_ASYNC | MS_INVALIDATE | MS_SYNC))
		goto out;

	// ~PAGE_MASK = 4095
	// start 等于4095就G。
	if (start & ~PAGE_MASK)
		goto out;

	// 如果同时这两个标志位直接G
	if ((flags & MS_ASYNC) && (flags & MS_SYNC))
		goto out;
	error = -ENOMEM;

	// 保留4095的余数。 
	// 还有一种写法呀....
	// len & ~PAGE_MASK 不就行了？？？？？
	len = (len + ~PAGE_MASK) & PAGE_MASK;
	end = start + len;
	if (end < start)
		goto out;
	error = 0;
	if (end == start)
		goto out;
	/*
	 * If the interval [start,end) covers some unmapped address ranges,
	 * just ignore them, but return -ENOMEM at the end.
	 */
	 // 找到当前文件对应的vma(虚拟地址)。
	vma = find_vma(current->mm, start);
	unmapped_error = 0;
	for (;;) {
		/* Still start < end. */
		error = -ENOMEM;
		if (!vma)
			goto out;
		/* Here start < vma->vm_end. */
		if (start < vma->vm_start) {
			unmapped_error = -ENOMEM;
			start = vma->vm_start;
		}
		/* Here vma->vm_start <= start < vma->vm_end. */
		if (end <= vma->vm_end) {
			if (start < end) {

				// end没超过vma->vm_end就直接使用end
				error = msync_interval(vma, start, end, flags);
				if (error)
					goto out;
			}
			error = unmapped_error;
			goto out;
		}
		/* Here vma->vm_start <= start < vma->vm_end < end. */
		// end超过了vma->vm_end就直接使用vma->vm_end
		error = msync_interval(vma, start, vma->vm_end, flags);
		if (error)
			goto out;
		start = vma->vm_end;
		vma = vma->vm_next;
	}
out:
	up_read(&current->mm->mmap_sem);
	return error;
}
