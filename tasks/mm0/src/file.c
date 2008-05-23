/*
 * Copyright (C) 2008 Bahadir Balban
 */
#include <init.h>
#include <vm_area.h>
#include <kmalloc/kmalloc.h>
#include <l4/macros.h>
#include <l4/api/errno.h>
#include <l4lib/types.h>
#include <l4lib/arch/syscalls.h>
#include <l4lib/arch/syslib.h>
#include <l4lib/ipcdefs.h>
#include <l4/api/kip.h>
#include <posix/sys/types.h>
#include <string.h>
#include <file.h>

/* List of all generic files */
LIST_HEAD(vm_file_list);


/* Copy from one page's buffer into another page */
int page_copy(struct page *dst, struct page *src,
	      unsigned long dst_offset, unsigned long src_offset,
	      unsigned long size)
{
	void *dstvaddr, *srcvaddr;

	BUG_ON(dst_offset + size > PAGE_SIZE);
	BUG_ON(src_offset + size > PAGE_SIZE);

	dstvaddr = l4_map_helper((void *)page_to_phys(dst), 1);
	srcvaddr = l4_map_helper((void *)page_to_phys(src), 1);

#if 0
	printf("%s: Copying from page with offset %d to page with offset %d\n"
	       "src copy offset: 0x%x, dst copy offset: 0x%x, copy size: %d\n",
	       __FUNCTION__, src->offset, dst->offset, src_offset, dst_offset,
	       size);

	printf("%s: Copying string: %s\n", __FUNCTION__,
	       srcvaddr + src_offset);
#endif

	memcpy(dstvaddr + dst_offset, srcvaddr + src_offset, size);

	l4_unmap_helper(dstvaddr, 1);
	l4_unmap_helper(srcvaddr, 1);

	return 0;
}

int vfs_read(unsigned long vnum, unsigned long file_offset,
	     unsigned long npages, void *pagebuf)
{
	int err;

	l4_save_ipcregs();

	write_mr(L4SYS_ARG0, vnum);
	write_mr(L4SYS_ARG1, file_offset);
	write_mr(L4SYS_ARG2, npages);
	write_mr(L4SYS_ARG3, (u32)pagebuf);

	if ((err = l4_sendrecv(VFS_TID, VFS_TID, L4_IPC_TAG_PAGER_READ)) < 0) {
		printf("%s: L4 IPC Error: %d.\n", __FUNCTION__, err);
		return err;
	}

	/* Check if syscall was successful */
	if ((err = l4_get_retval()) < 0) {
		printf("%s: Pager from VFS read error: %d.\n",
		       __FUNCTION__, err);
		return err;
	}
	l4_restore_ipcregs();

	return err;
}


/*
 * When a new file is opened by the vfs this receives the information
 * about that file so that it can serve that file's content (via
 * read/write/mmap) later to that task.
 */
int vfs_receive_sys_open(l4id_t sender, l4id_t opener, int fd,
			 unsigned long vnum, unsigned long length)
{
	struct vm_file *vmfile;
	struct tcb *t;

	/* Check argument validity */
	if (sender != VFS_TID) {
		l4_ipc_return(-EPERM);
		return 0;
	}

	if (!(t = find_task(opener))) {
		l4_ipc_return(-EINVAL);
		return 0;
	}

	if (fd < 0 || fd > TASK_FILES_MAX) {
		l4_ipc_return(-EINVAL);
		return 0;
	}

	/* Assign vnum to given fd on the task */
	t->fd[fd].vnum = vnum;
	t->fd[fd].cursor = 0;

	/* Check if that vm_file is already in the list */
	list_for_each_entry(vmfile, &vm_file_list, list) {
		/* Check it is a vfs file and if so vnums match. */
		if ((vmfile->type & VM_FILE_VFS) &&
		    vm_file_to_vnum(vmfile) == vnum) {
			/* Add a reference to it from the task */
			t->fd[fd].vmfile = vmfile;
			vmfile->vm_obj.refcnt++;
			l4_ipc_return(0);
			return 0;
		}
	}

	/* Otherwise allocate a new one for this vnode */
	if (IS_ERR(vmfile = vfs_file_create())) {
		l4_ipc_return((int)vmfile);
		return 0;
	}

	/* Initialise and add a reference to it from the task */
	vm_file_to_vnum(vmfile) = vnum;
	vmfile->length = length;
	vmfile->vm_obj.pager = &file_pager;
	t->fd[fd].vmfile = vmfile;
	vmfile->vm_obj.refcnt++;

	/* Add to global list */
	list_add(&vmfile->vm_obj.list, &vm_file_list);

	l4_ipc_return(0);
	return 0;
}


/*
 * Inserts the page to vmfile's list in order of page frame offset.
 * We use an ordered list instead of a radix or btree for now.
 */
int insert_page_olist(struct page *this, struct vm_object *vmo)
{
	struct page *before, *after;

	/* Add if list is empty */
	if (list_empty(&vmo->page_cache)) {
		list_add_tail(&this->list, &vmo->page_cache);
		return 0;
	}
	/* Else find the right interval */
	list_for_each_entry(before, &vmo->page_cache, list) {
		after = list_entry(before->list.next, struct page, list);

		/* If there's only one in list */
		if (before->list.next == &vmo->page_cache) {
			/* Add to end if greater */
			if (this->offset > before->offset)
				list_add_tail(&this->list, &before->list);
			/* Add to beginning if smaller */
			else if (this->offset < before->offset)
				list_add(&this->list, &before->list);
			else
				BUG();
			return 0;
		}

		/* If this page is in-between two other, insert it there */
		if (before->offset < this->offset &&
		    after->offset > this->offset) {
			list_add_tail(&this->list, &before->list);
			return 0;
		}
		BUG_ON(this->offset == before->offset);
		BUG_ON(this->offset == after->offset);
	}
	BUG();
}

/*
 * This reads-in a range of pages from a file and populates the page cache
 * just like a page fault, but its not in the page fault path.
 */
int read_file_pages(struct vm_file *vmfile, unsigned long pfn_start,
		    unsigned long pfn_end)
{
	struct page *page;

	for (int f_offset = pfn_start; f_offset < pfn_end; f_offset++) {
		page = vmfile->vm_obj.pager->ops.page_in(&vmfile->vm_obj,
							 f_offset);
		if (IS_ERR(page)) {
			printf("%s: %s:Could not read page %d "
			       "from file with vnum: 0x%x\n", __TASKNAME__,
			       __FUNCTION__, f_offset, vm_file_to_vnum(vmfile));
			return (int)page;
		}
	}

	return 0;
}

int vfs_write(unsigned long vnum, unsigned long file_offset,
	      unsigned long npages, void *pagebuf)
{
	int err;

	l4_save_ipcregs();

	write_mr(L4SYS_ARG0, vnum);
	write_mr(L4SYS_ARG1, file_offset);
	write_mr(L4SYS_ARG2, npages);
	write_mr(L4SYS_ARG3, (u32)pagebuf);

	if ((err = l4_sendrecv(VFS_TID, VFS_TID, L4_IPC_TAG_PAGER_WRITE)) < 0) {
		printf("%s: L4 IPC Error: %d.\n", __FUNCTION__, err);
		return err;
	}

	/* Check if syscall was successful */
	if ((err = l4_get_retval()) < 0) {
		printf("%s: Pager to VFS write error: %d.\n", __FUNCTION__, err);
		return err;
	}

	l4_restore_ipcregs();

	return err;
}

int vfs_close(l4id_t sender, int fd)
{
	int err;

	l4_save_ipcregs();

	write_mr(L4SYS_ARG0, sender);
	write_mr(L4SYS_ARG1, fd);

	if ((err = l4_sendrecv(VFS_TID, VFS_TID, L4_IPC_TAG_PAGER_CLOSE)) < 0) {
		printf("%s: L4 IPC Error: %d.\n", __FUNCTION__, err);
		return err;
	}

	/* Check if syscall was successful */
	if ((err = l4_get_retval()) < 0) {
		printf("%s: Pager to VFS write error: %d.\n", __FUNCTION__, err);
		return err;
	}

	l4_restore_ipcregs();

	return err;
}

/* Writes updated file stats back to vfs. (e.g. new file size) */
int vfs_update_file_stats(struct vm_file *f)
{
	int err;

	l4_save_ipcregs();

	write_mr(L4SYS_ARG0, vm_file_to_vnum(f));
	write_mr(L4SYS_ARG1, f->length);

	if ((err = l4_sendrecv(VFS_TID, VFS_TID,
			       L4_IPC_TAG_PAGER_UPDATE_STATS)) < 0) {
		printf("%s: L4 IPC Error: %d.\n", __FUNCTION__, err);
		return err;
	}

	/* Check if syscall was successful */
	if ((err = l4_get_retval()) < 0) {
		printf("%s: Pager to VFS write error: %d.\n",
		       __FUNCTION__, err);
		return err;
	}

	l4_restore_ipcregs();

	return err;

}

/* Writes pages in cache back to their file */
int write_file_pages(struct vm_file *f, unsigned long pfn_start,
		     unsigned long pfn_end)
{
	int err;

	BUG_ON(pfn_end != __pfn(page_align_up(f->length)));
	for (int f_offset = pfn_start; f_offset < pfn_end; f_offset++) {
		err = f->vm_obj.pager->ops.page_out(&f->vm_obj, f_offset);
		if (err < 0) {
			printf("%s: %s:Could not write page %d "
			       "to file with vnum: 0x%x\n", __TASKNAME__,
			       __FUNCTION__, f_offset, vm_file_to_vnum(f));
			return err;
		}
	}

	return 0;
}

/* Flush all dirty file pages and update file stats */
int flush_file_pages(struct vm_file *f)
{
	write_file_pages(f, 0, __pfn(page_align_up(f->length)));

	vfs_update_file_stats(f);

	return 0;
}

/* Given a task and fd syncs all IO on it */
int fsync_common(l4id_t sender, int fd)
{
	struct vm_file *f;
	struct tcb *task;
	int err;

	/* Get the task */
	BUG_ON(!(task = find_task(sender)));

	/* Check fd validity */
	if (fd < 0 || fd > TASK_FILES_MAX || !task->fd[fd].vmfile)
		return -EBADF;

	/* Finish I/O on file */
	f = task->fd[fd].vmfile;
	if ((err = flush_file_pages(f)) < 0)
		return err;

	return 0;
}

/* Closes the file descriptor and notifies vfs */
int fd_close(l4id_t sender, int fd)
{
	struct tcb *task;
	int err;

	/* Get the task */
	BUG_ON(!(task = find_task(sender)));

	if ((err = vfs_close(task->tid, fd)) < 0)
		return err;

	task->fd[fd].vnum = 0;
	task->fd[fd].cursor = 0;
	task->fd[fd].vmfile = 0;

	return 0;
}

int sys_close(l4id_t sender, int fd)
{
	int err;

	/* Sync the file and update stats */
	if ((err = fsync_common(sender, fd)) < 0) {
		l4_ipc_return(err);
		return 0;
	}
	
	/* Close the file descriptor. */
	return	fd_close(sender, fd);
}

int sys_fsync(l4id_t sender, int fd)
{
	/* Sync the file and update stats */
	return fsync_common(sender, fd);
}

/* FIXME: Add error handling to this */
/* Extends a file's size by adding it new pages */
int new_file_pages(struct vm_file *f, unsigned long start, unsigned long end)
{
	unsigned long npages = end - start;
	struct page *page;
	void *paddr;

	/* Allocate the memory for new pages */
	if (!(paddr = alloc_page(npages)))
		return -ENOMEM;

	/* Process each page */
	for (unsigned long i = 0; i < npages; i++) {
		page = phys_to_page(paddr + PAGE_SIZE * i);
		page_init(page);
		page->refcnt++;
		page->owner = &f->vm_obj;
		page->offset = start + i;
		page->virtual = 0;

		/* Add the page to file's vm object */
		BUG_ON(!list_empty(&page->list));
		insert_page_olist(page, &f->vm_obj);
	}

	/* Update vm object */
	f->vm_obj.npages += npages;

	return 0;
}

/* TODO:
 * Re-evaluate. Possibly merge with read_cache_pages.
 */

/* Writes user data in buffer into pages in cache */
int write_cache_pages(struct vm_file *vmfile, struct tcb *task, void *buf,
		      unsigned long pfn_start, unsigned long pfn_end,
		      unsigned long cursor_offset, int count)
{
	struct page *head, *this;
	unsigned long last_pgoff;	/* Last copied page's offset */
	unsigned long copy_offset;	/* Current copy offset on the buffer */
	int copysize, left;

	/* Find the head of consecutive pages */
	list_for_each_entry(head, &vmfile->vm_obj.page_cache, list)
		if (head->offset == pfn_start)
			goto copy;

	/*
	 * Page not found, this is a bug. The writeable page range
	 * must have been readied by read_file_pages()/new_file_pages().
	 */
	BUG();

copy:
	left = count;

	/* Copy the first page and unmap. */
	copysize = (left < PAGE_SIZE) ? left : PAGE_SIZE;
	copy_offset = (unsigned long)buf;
	page_copy(head, task_virt_to_page(task, copy_offset),
		  cursor_offset, copy_offset & PAGE_MASK, copysize);
	left -= copysize;
	last_pgoff = head->offset;

	/* Map the rest, copy and unmap. */
	list_for_each_entry(this, &head->list, list) {
		if (left == 0 || this->offset == pfn_end)
			break;

		/* Make sure we're advancing on pages consecutively */
		BUG_ON(this->offset != last_pgoff + 1);

		copysize = (left < PAGE_SIZE) ? left : PAGE_SIZE;
		copy_offset = (unsigned long)buf + count - left;

		/* Must be page aligned */
		BUG_ON(!is_page_aligned(copy_offset));

		page_copy(this, task_virt_to_page(task, copy_offset),
			  0, 0, copysize);
		left -= copysize;
		last_pgoff = this->offset;
	}
	BUG_ON(left != 0);

	return count - left;
}

/*
 * Reads a page range from an ordered list of pages into buffer.
 *
 * NOTE:
 * This assumes the page range is consecutively available in the cache
 * and count bytes are available. To ensure this, read_file_pages must
 * be called first and count must be checked. Since it has read-checking
 * assumptions, count must be satisfied.
 */
int read_cache_pages(struct vm_file *vmfile, struct tcb *task, void *buf,
		     unsigned long pfn_start, unsigned long pfn_end,
		     unsigned long cursor_offset, int count)
{
	struct page *head, *this;
	int copysize, left;
	unsigned long last_pgoff;	/* Last copied page's offset */
	unsigned long copy_offset;	/* Current copy offset on the buffer */

	/* Find the head of consecutive pages */
	list_for_each_entry(head, &vmfile->vm_obj.page_cache, list)
		if (head->offset == pfn_start)
			goto copy;

	/* Page not found, nothing read */
	return 0;

copy:
	left = count;

	/* Copy the first page and unmap. */
	copysize = (left < PAGE_SIZE) ? left : PAGE_SIZE;
	copy_offset = (unsigned long)buf;
	page_copy(task_virt_to_page(task, copy_offset), head,
		  PAGE_MASK & copy_offset, cursor_offset, copysize);
	left -= copysize;
	last_pgoff = head->offset;

	/* Map the rest, copy and unmap. */
	list_for_each_entry(this, &head->list, list) {
		if (left == 0 || this->offset == pfn_end)
			break;

		/* Make sure we're advancing on pages consecutively */
		BUG_ON(this->offset != last_pgoff + 1);

		/* Get copying size and start offset */
		copysize = (left < PAGE_SIZE) ? left : PAGE_SIZE;
		copy_offset = (unsigned long)buf + count - left;

		/* MUST be page aligned */
		BUG_ON(!is_page_aligned(copy_offset));

		page_copy(task_virt_to_page(task, copy_offset),
			  this, 0, 0, copysize);
		left -= copysize;
		last_pgoff = this->offset;
	}
	BUG_ON(left != 0);

	return count - left;
}

int sys_read(l4id_t sender, int fd, void *buf, int count)
{
	unsigned long pfn_start, pfn_end;
	unsigned long cursor, byte_offset;
	struct vm_file *vmfile;
	int err, retval = 0;
	struct tcb *task;

	BUG_ON(!(task = find_task(sender)));

	/* Check fd validity */
	if (fd < 0 || fd > TASK_FILES_MAX || !task->fd[fd].vmfile) {
		retval = -EBADF;
		goto out;
	}

	/* Check count validity */
	if (count < 0) {
		retval = -EINVAL;
		goto out;
	} else if (!count) {
		retval = 0;
		goto out;
	}

	/* Check user buffer validity. */
	if ((err = validate_task_range(task, (unsigned long)buf,
				       (unsigned long)(buf + count),
				       VM_READ)) < 0) {
		retval = err;
		goto out;
	}

	vmfile = task->fd[fd].vmfile;
	cursor = task->fd[fd].cursor;

	/* If cursor is beyond file end, simply return 0 */
	if (cursor >= vmfile->length) {
		retval = 0;
		goto out;
	}

	/* Start and end pages expected to be read by user */
	pfn_start = __pfn(cursor);
	pfn_end = __pfn(page_align_up(cursor + count));

	/* But we can read up to maximum file size */
	pfn_end = __pfn(page_align_up(vmfile->length)) < pfn_end ?
		  __pfn(page_align_up(vmfile->length)) : pfn_end;

	/* If trying to read more than end of file, reduce it to max possible */
	if (cursor + count > vmfile->length)
		count = vmfile->length - cursor;

	/* Read the page range into the cache from file */
	if ((err = read_file_pages(vmfile, pfn_start, pfn_end)) < 0) {
		retval = err;
		goto out;
	}

	/* The offset of cursor on first page */
	byte_offset = PAGE_MASK & cursor;

	/* Read it into the user buffer from the cache */
	if ((count = read_cache_pages(vmfile, task, buf, pfn_start, pfn_end,
				    byte_offset, count)) < 0) {
		retval = count;
		goto out;
	}

	/* Update cursor on success */
	task->fd[fd].cursor += count;
	retval = count;

out:
	l4_ipc_return(retval);
	return 0;

}

/* FIXME:
 *
 * Error:
 * We find the page buffer is in, and then copy from the *start* of the page
 * rather than buffer's offset in that page.
 */
int sys_write(l4id_t sender, int fd, void *buf, int count)
{
	unsigned long pfn_wstart, pfn_wend;	/* Write start/end */
	unsigned long pfn_fstart, pfn_fend;	/* File start/end */
	unsigned long pfn_nstart, pfn_nend;	/* New pages start/end */
	unsigned long cursor, byte_offset;
	struct vm_file *vmfile;
	int err = 0, retval = 0;
	struct tcb *task;

	BUG_ON(!(task = find_task(sender)));

	/* Check fd validity */
	if (fd < 0 || fd > TASK_FILES_MAX || !task->fd[fd].vmfile) {
		retval = -EBADF;
		goto out;
	}

	/* Check count validity */
	if (count < 0) {
		retval = -EINVAL;
		goto out;
	} else if (!count) {
		retval = 0;
		goto out;
	}

	/* Check user buffer validity. */
	if ((err = validate_task_range(task, (unsigned long)buf,
				       (unsigned long)(buf + count),
				       VM_WRITE | VM_READ)) < 0) {
		retval = err;
		goto out;
	}

	vmfile = task->fd[fd].vmfile;
	cursor = task->fd[fd].cursor;

	/* See what pages user wants to write */
	pfn_wstart = __pfn(cursor);
	pfn_wend = __pfn(page_align_up(cursor + count));

	/* Get file start and end pages */
	pfn_fstart = 0;
	pfn_fend = __pfn(page_align_up(vmfile->length));

	/*
	 * Find the intersection to determine which pages are
	 * already part of the file, and which ones are new.
	 */
	if (pfn_wstart < pfn_fend) {
		pfn_fstart = pfn_wstart;

		/*
		 * Shorten the end if end page is
		 * less than file size
		 */
		if (pfn_wend < pfn_fend) {
			pfn_fend = pfn_wend;

			/* This also means no new pages in file */
			pfn_nstart = 0;
			pfn_nend = 0;
		} else {

			/* The new pages start from file end,
			 * and end by write end. */
			pfn_nstart = pfn_fend;
			pfn_nend = pfn_wend;
		}

	} else {
		/* No intersection, its all new pages */
		pfn_fstart = 0;
		pfn_fend = 0;
		pfn_nstart = pfn_wstart;
		pfn_nend = pfn_wend;
	}

	/*
	 * Read in the portion that's already part of the file.
	 */
	if ((err = read_file_pages(vmfile, pfn_fstart, pfn_fend)) < 0) {
		retval = err;
		goto out;
	}

	/* Create new pages for the part that's new in the file */
	if ((err = new_file_pages(vmfile, pfn_nstart, pfn_nend)) < 0) {
		retval = err;
		goto out;
	}

	/*
	 * At this point be it new or existing file pages, all pages
	 * to be written are expected to be in the page cache. Write.
	 */
	byte_offset = PAGE_MASK & cursor;
	if ((err = write_cache_pages(vmfile, task, buf, pfn_wstart,
				     pfn_wend, byte_offset, count)) < 0) {
		retval = err;
		goto out;
	}

	/*
	 * Update the file size, and cursor. vfs will be notified
	 * of this change when the file is flushed (e.g. via fflush()
	 * or close())
	 */
	if (task->fd[fd].cursor + count > vmfile->length)
		vmfile->length = task->fd[fd].cursor + count;

	task->fd[fd].cursor += count;
	retval = count;

out:
	l4_ipc_return(retval);
	return 0;
}

/* FIXME: Check for invalid cursor values. Check for total, sometimes negative. */
int sys_lseek(l4id_t sender, int fd, off_t offset, int whence)
{
	struct tcb *task;
	int retval = 0;
	unsigned long long total, cursor;

	BUG_ON(!(task = find_task(sender)));

	/* Check fd validity */
	if (fd < 0 || fd > TASK_FILES_MAX || !task->fd[fd].vmfile) {
		retval = -EBADF;
		goto out;
	}

	/* Offset validity */
	if (offset < 0) {
		retval = -EINVAL;
		goto out;
	}

	switch (whence) {
	case SEEK_SET:
		retval = task->fd[fd].cursor = offset;
		break;
	case SEEK_CUR:
		cursor = (unsigned long long)task->fd[fd].cursor;
		if (cursor + offset > 0xFFFFFFFF)
			retval = -EINVAL;
		else
			retval = task->fd[fd].cursor += offset;
		break;
	case SEEK_END:
		cursor = (unsigned long long)task->fd[fd].cursor;
		total = (unsigned long long)task->fd[fd].vmfile->length;
		if (cursor + total > 0xFFFFFFFF)
			retval = -EINVAL;
		else {
			retval = task->fd[fd].cursor =
				task->fd[fd].vmfile->length + offset;
		}
	default:
		retval = -EINVAL;
		break;
	}

out:
	l4_ipc_return(retval);
	return 0;
}

