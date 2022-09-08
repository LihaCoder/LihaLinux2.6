/*
 * This file contains the procedures for the handling of select and poll
 *
 * Created for Linux based loosely upon Mathius Lattner's minix
 * patches by Peter MacDonald. Heavily edited by Linus.
 *
 *  4 February 1994
 *     COFF/ELF binary emulation. If the process has the STICKY_TIMEOUTS
 *     flag set in its personality we do *not* modify the given timeout
 *     parameter to reflect time remaining.
 *
 *  24 January 2000
 *     Changed sys_poll()/do_poll() to use PAGE_SIZE chunk-based allocation 
 *     of fds to overcome nfds < 16390 descriptors limit (Tigran Aivazian).
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/smp_lock.h>
#include <linux/poll.h>
#include <linux/personality.h> /* for STICKY_TIMEOUTS */
#include <linux/file.h>
#include <linux/fs.h>

#include <asm/uaccess.h>

#define ROUND_UP(x,y) (((x)+(y)-1)/(y))
#define DEFAULT_POLLMASK (POLLIN | POLLOUT | POLLRDNORM | POLLWRNORM)

struct poll_table_entry {

	// 当前节点的文件。
	struct file * filp;

	// 这个
	wait_queue_t wait;

	// 这是当前sock协议对应的一个队列。  注意这里是指针哦。  baby
	wait_queue_head_t * wait_address;
};

// 这里维护了
struct poll_table_page {
	struct poll_table_page * next;
	struct poll_table_entry * entry;
	struct poll_table_entry entries[0];
};

#define POLL_TABLE_FULL(table) \
	((unsigned long)((table)->entry+1) > PAGE_SIZE + (unsigned long)(table))

/*
 * Ok, Peter made a complicated, but straightforward multiple_wait() function.
 * I have rewritten this, taking some shortcuts: This code may not be easy to
 * follow, but it should be free of race-conditions, and it's practical. If you
 * understand what I'm doing here, then you understand how the linux
 * sleep/wakeup mechanism works.
 *
 * Two very simple procedures, poll_wait() and poll_freewait() make all the
 * work.  poll_wait() is an inline-function defined in <linux/poll.h>,
 * as all select/poll functions have to call it to add an entry to the
 * poll table.
 */
void __pollwait(struct file *filp, wait_queue_head_t *wait_address, poll_table *p);

void poll_initwait(struct poll_wqueues *pwq)
{
	init_poll_funcptr(&pwq->pt, __pollwait);
	pwq->error = 0;
	pwq->table = NULL;
}

EXPORT_SYMBOL(poll_initwait);

void poll_freewait(struct poll_wqueues *pwq)
{
	// 拿到等待队列
	struct poll_table_page * p = pwq->table;
	
	while (p) {
		struct poll_table_entry * entry;
		struct poll_table_page *old;

		entry = p->entry;
		
		do {
			entry--;

			// entry->wait_address这是队列
			// entry->wait这是队列的任务
			remove_wait_queue(entry->wait_address,&entry->wait);
		
			fput(entry->filp);
		} while (entry > p->entries);
		
		old = p;
		p = p->next;
		free_page((unsigned long) old);
	}
}

EXPORT_SYMBOL(poll_freewait);

// tcp_poll里面执行的钩子
// 这是把fd和当前进程封装成一个队列元素放入到队列中，并且绑定了回调的钩子。
void __pollwait(struct file *filp, wait_queue_head_t *wait_address, poll_table *_p)
{
	// 骚操作获取到结构体的基址。
	struct poll_wqueues *p = container_of(_p, struct poll_wqueues, pt);

	// 
	struct poll_table_page *table = p->table;

	// 队列不存在，或者内部元素为0.
	// 也就是初始化的操作
	if (!table || POLL_TABLE_FULL(table)) {
		struct poll_table_page *new_table;

		new_table = (struct poll_table_page *) __get_free_page(GFP_KERNEL);
		if (!new_table) {
			p->error = -ENOMEM;
			__set_current_state(TASK_RUNNING);
			return;
		}
		new_table->entry = new_table->entries;
		new_table->next = table;
		p->table = new_table;
		table = new_table;
	}

	/* Add a new entry */
	// 放入到队列。
	{
		struct poll_table_entry * entry = table->entry;

		// 意思是poll_table_entry是一个连续的空间，数组？
		// 下次使用entry，就是当前的下一个。
		table->entry = entry+1;

		// 原子性加引用。代表被使用了。
	 	get_file(filp);
		
	 	entry->filp = filp;
		
		entry->wait_address = wait_address;

		// 初始化wait_queue_t wait;
		// 把对应的进程结构体、回调钩子赋值
		init_waitqueue_entry(&entry->wait, current);

		// wait_address这个是sock维护的队列
		// &entry->wait这个是队列中的元素
		// 所以这是放入到队列中
		add_wait_queue(wait_address,&entry->wait);
	}
}


#define __IN(fds, n)		(fds->in + n)
#define __OUT(fds, n)		(fds->out + n)
#define __EX(fds, n)		(fds->ex + n)
#define __RES_IN(fds, n)	(fds->res_in + n)
#define __RES_OUT(fds, n)	(fds->res_out + n)
#define __RES_EX(fds, n)	(fds->res_ex + n)

#define BITS(fds, n)		(*__IN(fds, n)|*__OUT(fds, n)|*__EX(fds, n))

static int max_select_fd(unsigned long n, fd_set_bits *fds)
{
	unsigned long *open_fds;
	unsigned long set;
	int max;

	/* handle last in-complete long-word first */
	set = ~(~0UL << (n & (__NFDBITS-1)));
	n /= __NFDBITS;
	open_fds = current->files->open_fds->fds_bits+n;
	max = 0;
	if (set) {
		set &= BITS(fds, n);
		if (set) {
			if (!(set & ~*open_fds))
				goto get_max;
			return -EBADF;
		}
	}
	while (n) {
		open_fds--;
		n--;
		set = BITS(fds, n);
		if (!set)
			continue;
		if (set & ~*open_fds)
			return -EBADF;
		if (max)
			continue;
get_max:
		do {
			max++;
			set >>= 1;
		} while (set);
		max += n * __NFDBITS;
	}

	return max;
}

#define BIT(i)		(1UL << ((i)&(__NFDBITS-1)))
#define MEM(i,m)	((m)+(unsigned)(i)/__NFDBITS)
#define ISSET(i,m)	(((i)&*(m)) != 0)
#define SET(i,m)	(*(m) |= (i))

#define POLLIN_SET (POLLRDNORM | POLLRDBAND | POLLIN | POLLHUP | POLLERR)
#define POLLOUT_SET (POLLWRBAND | POLLWRNORM | POLLOUT | POLLERR)
#define POLLEX_SET (POLLPRI)

int do_select(int n, fd_set_bits *fds, long *timeout)
{
	struct poll_wqueues table;
	poll_table *wait;
	int retval, i;
	long __timeout = *timeout;

 	spin_lock(&current->files->file_lock);
	retval = max_select_fd(n, fds);
	spin_unlock(&current->files->file_lock);

	if (retval < 0)
		return retval;
	n = retval;

	poll_initwait(&table);
	wait = &table.pt;
	if (!__timeout)
		wait = NULL;
	retval = 0;
	for (;;) {
		unsigned long *rinp, *routp, *rexp, *inp, *outp, *exp;

		set_current_state(TASK_INTERRUPTIBLE);

		inp = fds->in; outp = fds->out; exp = fds->ex;
		rinp = fds->res_in; routp = fds->res_out; rexp = fds->res_ex;

		for (i = 0; i < n; ++rinp, ++routp, ++rexp) {
			unsigned long in, out, ex, all_bits, bit = 1, mask, j;
			unsigned long res_in = 0, res_out = 0, res_ex = 0;
			struct file_operations *f_op = NULL;
			struct file *file = NULL;

			// 这里是先++ 再*
			// 取数组的下一位。
			in = *inp++; out = *outp++; ex = *exp++;

			// 二进制组合，最后还是为0，就代表这三个值都为0
			all_bits = in | out | ex;
			if (all_bits == 0) {
				i += __NFDBITS;
				continue;
			}

			for (j = 0; j < __NFDBITS; ++j, ++i, bit <<= 1) {

				// 这个退出条件，代表所有的n都已经遍历完了。
				if (i >= n)
					break;
				if (!(bit & all_bits))
					continue;

				// 获取到文件
				file = fget(i);
				
				if (file) {
					f_op = file->f_op;
					mask = DEFAULT_POLLMASK;
					if (f_op && f_op->poll)
						//  这里面会把当前fd和当前current做绑定放入到一个队列中等待
						mask = (*f_op->poll)(file, retval ? NULL : wait);
					fput(file);

					// in 必须有bit这一位？
					if ((mask & POLLIN_SET) && (in & bit)) {
						res_in |= bit;
						retval++;
					}

					// out 必须有bit这一位？
					if ((mask & POLLOUT_SET) && (out & bit)) {
						res_out |= bit;
						retval++;
					}

					// ex 必须有bit这一位？
					if ((mask & POLLEX_SET) && (ex & bit)) {
						res_ex |= bit;
						retval++;
					}
				}
			}
			if (res_in)
				*rinp = res_in;
			if (res_out)
				*routp = res_out;
			if (res_ex)
				*rexp = res_ex;
		}
		// shit
		wait = NULL;
		if (retval || !__timeout || signal_pending(current))
			break;
		if(table.error) {
			retval = table.error;
			break;
		}
		__timeout = schedule_timeout(__timeout);
	}
	__set_current_state(TASK_RUNNING);


	poll_freewait(&table);

	/*
	 * Up-to-date the caller timeout.
	 */
	*timeout = __timeout;
	return retval;
}

EXPORT_SYMBOL(do_select);

static void *select_bits_alloc(int size)
{
	return kmalloc(6 * size, GFP_KERNEL);
}

static void select_bits_free(void *bits, int size)
{
	kfree(bits);
}

/*
 * We can actually return ERESTARTSYS instead of EINTR, but I'd
 * like to be certain this leads to no problems. So I return
 * EINTR just for safety.
 *
 * Update: ERESTARTSYS breaks at least the xview clock binary, so
 * I'm trying ERESTARTNOHAND which restart only when you want to.
 */
#define MAX_SELECT_SECONDS \
	((unsigned long) (MAX_SCHEDULE_TIMEOUT / HZ)-1)

// 先要明白select的作用。
// 是什么  ，能干什么，怎么玩。理论，实操，总结。三板斧
asmlinkage long
sys_select(int n, fd_set __user *inp, fd_set __user *outp, fd_set __user *exp, struct timeval __user *tvp)
{
	fd_set_bits fds;
	char *bits;
	long timeout;
	int ret, size, max_fdset;

	timeout = MAX_SCHEDULE_TIMEOUT;
	if (tvp) {
		time_t sec, usec;

		if ((ret = verify_area(VERIFY_READ, tvp, sizeof(*tvp)))
		    || (ret = __get_user(sec, &tvp->tv_sec))
		    || (ret = __get_user(usec, &tvp->tv_usec)))
			goto out_nofds;

		ret = -EINVAL;
		if (sec < 0 || usec < 0)
			goto out_nofds;

		if ((unsigned long) sec < MAX_SELECT_SECONDS) {
			timeout = ROUND_UP(usec, 1000000/HZ);
			timeout += sec * (unsigned long) HZ;
		}
	}

	ret = -EINVAL;
	if (n < 0)
		goto out_nofds;

	/* max_fdset can increase, so grab it once to avoid race */
	max_fdset = current->·files->max_fdset;
	if (n > max_fdset)
		n = max_fdset;

	/*
	 * We need 6 bitmaps (in/out/ex for both incoming and outgoing),
	 * since we used fdset we need to allocate memory in units of
	 * long-words. 
	 */
	ret = -ENOMEM;

	
	size = FDS_BYTES(n);

	// 开辟4 * 6的大小，用来存放fd_set_bits fds;
	bits = select_bits_alloc(size);
	
	if (!bits)
		goto out_nofds;

	// 推指针
	fds.in      = (unsigned long *)  bits;
	fds.out     = (unsigned long *) (bits +   size);
	fds.ex      = (unsigned long *) (bits + 2*size);
	fds.res_in  = (unsigned long *) (bits + 3*size);
	fds.res_out = (unsigned long *) (bits + 4*size);
	fds.res_ex  = (unsigned long *) (bits + 5*size);

	// 把用户传进来的数据拷贝到上面初始化的fd_set_bits fds中。
	if ((ret = get_fd_set(n, inp, fds.in)) ||
	    (ret = get_fd_set(n, outp, fds.out)) ||
	    (ret = get_fd_set(n, exp, fds.ex)))
		goto out;
	zero_fd_set(n, fds.res_in);
	zero_fd_set(n, fds.res_out);
	zero_fd_set(n, fds.res_ex);

	ret = do_select(n, &fds, &timeout);

	if (tvp && !(current->personality & STICKY_TIMEOUTS)) {
		time_t sec = 0, usec = 0;
		if (timeout) {
			sec = timeout / HZ;
			usec = timeout % HZ;
			usec *= (1000000/HZ);
		}
		put_user(sec, &tvp->tv_sec);
		put_user(usec, &tvp->tv_usec);
	}

	if (ret < 0)
		goto out;
	if (!ret) {
		ret = -ERESTARTNOHAND;
		if (signal_pending(current))
			goto out;
		ret = 0;
	}

	set_fd_set(n, inp, fds.res_in);
	set_fd_set(n, outp, fds.res_out);
	set_fd_set(n, exp, fds.res_ex);

out:
	select_bits_free(bits, size);
out_nofds:
	return ret;
}

struct poll_list {
	struct poll_list *next;
	int len;
	struct pollfd entries[0];
};


#define POLLFD_PER_PAGE  ((PAGE_SIZE-sizeof(struct poll_list)) / sizeof(struct pollfd))

static void do_pollfd(unsigned int num, struct pollfd * fdpage,
	poll_table ** pwait, int *count)
{
	int i;

	// num为数量
	for (i = 0; i < num; i++) {
		
		int fd;
		
		unsigned int mask;
	
		struct pollfd *fdp;

		mask = 0;

		// 得到数组的下标元素
		fdp = fdpage+i;

		// 得到fd
		fd = fdp->fd;
		
		if (fd >= 0) {
			// 得到fd对应的file
			struct file * file = fget(fd);
			
			mask = POLLNVAL;

			
			if (file != NULL) {
				mask = DEFAULT_POLLMASK;
				
				if (file->f_op && file->f_op->poll)
					// 老样子，执行poll，内部执行回调，把当前file和current放入到对应的等待队列中
					mask = file->f_op->poll(file, *pwait);
				
				mask &= fdp->events | POLLERR | POLLHUP;
				fput(file);
			}

			// 为什么没找到file
			// count也++？
			if (mask) {
				*pwait = NULL;
				(*count)++;
			}
		}
		// 为0就代表
		fdp->revents = mask;
	}
}

// nfds数量
// list poll_list链表头部
// wait 等待队列和回调事件
// timeout超时时间
static int do_poll(unsigned int nfds,  struct poll_list *list,
			struct poll_wqueues *wait, long timeout)
{
	int count = 0;
	poll_table* pt = &wait->pt;	 	// 回调事件

	if (!timeout)
		pt = NULL;
 
	for (;;) {
		
		struct poll_list *walk;
		
		set_current_state(TASK_INTERRUPTIBLE);
	
		walk = list;
		
		while(walk != NULL) {
			
			do_pollfd( walk->len, walk->entries, &pt, &count);
			
			walk = walk->next;
		
		}
		
		pt = NULL;
		if (count || !timeout || signal_pending(current))
			break;
		count = wait->error;	
		if (count)
			break;

		// 目前没事件，醒来了再试。
		timeout = schedule_timeout(timeout);
		
	}
	__set_current_state(TASK_RUNNING);
	return count;
}

// ufds nfds 指针+数量 = 数组
asmlinkage long sys_poll(struct pollfd __user * ufds, unsigned int nfds, long timeout)
{
	struct poll_wqueues table;
 	int fdcount, err;
 	unsigned int i;
	struct poll_list *head;
 	struct poll_list *walk;

	/* Do a sanity check on nfds ... */
	// 一次的范围限制
	if (nfds > current->files->max_fdset && nfds > OPEN_MAX)
		return -EINVAL;

	if (timeout) {
		/* Careful about overflow in the intermediate values */
		if ((unsigned long) timeout < MAX_SCHEDULE_TIMEOUT / HZ)
			timeout = (unsigned long)(timeout*HZ+999)/1000+1;
		else /* Negative or overflow */
			timeout = MAX_SCHEDULE_TIMEOUT;
	}

	// 跟select一样，挂回调钩子
	poll_initwait(&table);


	head = NULL;		// 链表头部
	walk = NULL;		// 工作链表。
	i = nfds;
	err = -ENOMEM;

	// 这个while的目的是把用户态的数据拷贝到内核态中（因为用户态的数据不可信）。
	while(i!=0) {
		
		struct poll_list *pp;
		
		pp = kmalloc(sizeof(struct poll_list)+
				sizeof(struct pollfd)*
				(i>POLLFD_PER_PAGE?POLLFD_PER_PAGE:i),
					GFP_KERNEL);
	
		if(pp==NULL)
			goto out_fds;
		
		pp->next=NULL;

		// 超过一个页？
		pp->len = (i>POLLFD_PER_PAGE?POLLFD_PER_PAGE:i);

		// 第一次走if初始化，后续链在链表后面。
		if (head == NULL)
			head = pp;
		else
			walk->next = pp;

		// 赋好值，下次while。
		walk = pp;

		// 把用户态的内容，拷贝到内核态，除非超过一页，要不然就是一次性拷贝结束。
		if (copy_from_user(pp->entries, ufds + nfds-i, 
				sizeof(struct pollfd)*pp->len)) {
			err = -EFAULT;
			goto out_fds;
		}
		i -= pp->len;
	}

	// 
	fdcount = do_poll(nfds, head, &table, timeout);

	/* OK, now copy the revents fields back to user space. */
	walk = head;
	err = -EFAULT;

	// 把内容（全部，准备好的和没准备好的）再拷贝到用户态。
	while(walk != NULL) {
		struct pollfd *fds = walk->entries;
		int j;

		for (j=0; j < walk->len; j++, ufds++) {
			if(__put_user(fds[j].revents, &ufds->revents))
				goto out_fds;
		}
		walk = walk->next;
  	}
	err = fdcount;
	if (!fdcount && signal_pending(current))
		err = -EINTR;
out_fds:
	walk = head;

	// 返回前要把开辟的内存空间给释放
	while(walk!=NULL) {
		struct poll_list *pp = walk->next;
		kfree(walk);
		walk = pp;
	}

	// 并且要把链表中的数据给清空
	poll_freewait(&table);
	return err;
}
