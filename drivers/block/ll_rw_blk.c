/*
 *  linux/drivers/block/ll_rw_blk.c
 *
 * Copyright (C) 1991, 1992 Linus Torvalds
 * Copyright (C) 1994,      Karl Keyte: Added support for disk statistics
 * Elevator latency, (C) 2000  Andrea Arcangeli <andrea@suse.de> SuSE
 * Queue request tables / lock, selectable elevator, Jens Axboe <axboe@suse.de>
 * kernel-doc documentation started by NeilBrown <neilb@cse.unsw.edu.au> -  July2000
 * bio rewrite, highmem i/o, etc, Jens Axboe <axboe@suse.de> - may 2001
 */

/*
 * This handles all read/write requests to block devices
 */
#include <linux/config.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/backing-dev.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/highmem.h>
#include <linux/mm.h>
#include <linux/kernel_stat.h>
#include <linux/string.h>
#include <linux/init.h>
#include <linux/bootmem.h>	/* for max_pfn/max_low_pfn */
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/swap.h>

static void blk_unplug_work(void *data);
static void blk_unplug_timeout(unsigned long data);

/*
 * For the allocated request tables
 */
static kmem_cache_t *request_cachep;

/*
 * plug management
 */
static LIST_HEAD(blk_plug_list);
static spinlock_t blk_plug_lock __cacheline_aligned_in_smp = SPIN_LOCK_UNLOCKED;

static wait_queue_head_t congestion_wqh[2];

/*
 * Controlling structure to kblockd
 */
static struct workqueue_struct *kblockd_workqueue; 

unsigned long blk_max_low_pfn, blk_max_pfn;

EXPORT_SYMBOL(blk_max_low_pfn);
EXPORT_SYMBOL(blk_max_pfn);

/* Amount of time in which a process may batch requests */
#define BLK_BATCH_TIME	(HZ/50UL)

/* Number of requests a "batching" process may submit */
#define BLK_BATCH_REQ	32

/*
 * Return the threshold (number of used requests) at which the queue is
 * considered to be congested.  It include a little hysteresis to keep the
 * context switch rate down.
 */
static inline int queue_congestion_on_threshold(struct request_queue *q)
{
	int ret;

	ret = q->nr_requests - (q->nr_requests / 8) + 1;

	if (ret > q->nr_requests)
		ret = q->nr_requests;

	return ret;
}

/*
 * The threshold at which a queue is considered to be uncongested
 */
static inline int queue_congestion_off_threshold(struct request_queue *q)
{
	int ret;

	ret = q->nr_requests - (q->nr_requests / 8) - 1;

	if (ret < 1)
		ret = 1;

	return ret;
}

/*
 * A queue has just exitted congestion.  Note this in the global counter of
 * congested queues, and wake up anyone who was waiting for requests to be
 * put back.
 */
static void clear_queue_congested(request_queue_t *q, int rw)
{
	enum bdi_state bit;
	wait_queue_head_t *wqh = &congestion_wqh[rw];

	bit = (rw == WRITE) ? BDI_write_congested : BDI_read_congested;
	clear_bit(bit, &q->backing_dev_info.state);
	if (waitqueue_active(wqh))
		wake_up(wqh);
}

/*
 * A queue has just entered congestion.  Flag that in the queue's VM-visible
 * state flags and increment the global gounter of congested queues.
 */
static void set_queue_congested(request_queue_t *q, int rw)
{
	enum bdi_state bit;

	bit = (rw == WRITE) ? BDI_write_congested : BDI_read_congested;
	set_bit(bit, &q->backing_dev_info.state);
}

/**
 * blk_get_backing_dev_info - get the address of a queue's backing_dev_info
 * @bdev:	device
 *
 * Locates the passed device's request queue and returns the address of its
 * backing_dev_info
 *
 * Will return NULL if the request queue cannot be located.
 */
struct backing_dev_info *blk_get_backing_dev_info(struct block_device *bdev)
{
	struct backing_dev_info *ret = NULL;
	request_queue_t *q = bdev_get_queue(bdev);

	if (q)
		ret = &q->backing_dev_info;
	return ret;
}

void blk_queue_activity_fn(request_queue_t *q, activity_fn *fn, void *data)
{
	q->activity_fn = fn;
	q->activity_data = data;
}

/**
 * blk_queue_prep_rq - set a prepare_request function for queue
 * @q:		queue
 * @pfn:	prepare_request function
 *
 * It's possible for a queue to register a prepare_request callback which
 * is invoked before the request is handed to the request_fn. The goal of
 * the function is to prepare a request for I/O, it can be used to build a
 * cdb from the request data for instance.
 *
 */
void blk_queue_prep_rq(request_queue_t *q, prep_rq_fn *pfn)
{
	q->prep_rq_fn = pfn;
}

EXPORT_SYMBOL(blk_queue_prep_rq);

/**
 * blk_queue_merge_bvec - set a merge_bvec function for queue
 * @q:		queue
 * @mbfn:	merge_bvec_fn
 *
 * Usually queues have static limitations on the max sectors or segments that
 * we can put in a request. Stacking drivers may have some settings that
 * are dynamic, and thus we have to query the queue whether it is ok to
 * add a new bio_vec to a bio at a given offset or not. If the block device
 * has such limitations, it needs to register a merge_bvec_fn to control
 * the size of bio's sent to it. Per default now merge_bvec_fn is defined for
 * a queue, and only the fixed limits are honored.
 *
 */
void blk_queue_merge_bvec(request_queue_t *q, merge_bvec_fn *mbfn)
{
	q->merge_bvec_fn = mbfn;
}

EXPORT_SYMBOL(blk_queue_merge_bvec);

/**
 * blk_queue_make_request - define an alternate make_request function for a device
 * @q:  the request queue for the device to be affected
 * @mfn: the alternate make_request function
 *
 * Description:
 *    The normal way for &struct bios to be passed to a device
 *    driver is for them to be collected into requests on a request
 *    queue, and then to allow the device driver to select requests
 *    off that queue when it is ready.  This works well for many block
 *    devices. However some block devices (typically virtual devices
 *    such as md or lvm) do not benefit from the processing on the
 *    request queue, and are served best by having the requests passed
 *    directly to them.  This can be achieved by providing a function
 *    to blk_queue_make_request().
 *
 * Caveat:
 *    The driver that does this *must* be able to deal appropriately
 *    with buffers in "highmemory". This can be accomplished by either calling
 *    __bio_kmap_atomic() to get a temporary kernel mapping, or by calling
 *    blk_queue_bounce() to create a buffer in normal memory.
 **/
void blk_queue_make_request(request_queue_t * q, make_request_fn * mfn)
{
	/*
	 * set defaults
	 */
	q->nr_requests = BLKDEV_MAX_RQ;
	q->max_phys_segments = MAX_PHYS_SEGMENTS;
	q->max_hw_segments = MAX_HW_SEGMENTS;
	q->make_request_fn = mfn;
	q->backing_dev_info.ra_pages = (VM_MAX_READAHEAD * 1024) / PAGE_CACHE_SIZE;
	q->backing_dev_info.state = 0;
	q->backing_dev_info.memory_backed = 0;
	blk_queue_max_sectors(q, MAX_SECTORS);
	blk_queue_hardsect_size(q, 512);
	blk_queue_dma_alignment(q, 511);

	q->unplug_thresh = 4;		/* hmm */
	q->unplug_delay = (3 * HZ) / 1000;	/* 3 milliseconds */
	if (q->unplug_delay == 0)
		q->unplug_delay = 1;

	INIT_WORK(&q->unplug_work, blk_unplug_work, q);

	q->unplug_timer.function = blk_unplug_timeout;
	q->unplug_timer.data = (unsigned long)q;

	/*
	 * by default assume old behaviour and bounce for any highmem page
	 */
	blk_queue_bounce_limit(q, BLK_BOUNCE_HIGH);

	INIT_LIST_HEAD(&q->plug_list);

	blk_queue_activity_fn(q, NULL, NULL);
}

EXPORT_SYMBOL(blk_queue_make_request);

/**
 * blk_queue_bounce_limit - set bounce buffer limit for queue
 * @q:  the request queue for the device
 * @dma_addr:   bus address limit
 *
 * Description:
 *    Different hardware can have different requirements as to what pages
 *    it can do I/O directly to. A low level driver can call
 *    blk_queue_bounce_limit to have lower memory pages allocated as bounce
 *    buffers for doing I/O to pages residing above @page. By default
 *    the block layer sets this to the highest numbered "low" memory page.
 **/
void blk_queue_bounce_limit(request_queue_t *q, u64 dma_addr)
{
	unsigned long bounce_pfn = dma_addr >> PAGE_SHIFT;
	unsigned long mb = dma_addr >> 20;
	static request_queue_t *last_q;

	/*
	 * set appropriate bounce gfp mask -- unfortunately we don't have a
	 * full 4GB zone, so we have to resort to low memory for any bounces.
	 * ISA has its own < 16MB zone.
	 */
	if (bounce_pfn < blk_max_low_pfn) {
		BUG_ON(dma_addr < BLK_BOUNCE_ISA);
		init_emergency_isa_pool();
		q->bounce_gfp = GFP_NOIO | GFP_DMA;
	} else
		q->bounce_gfp = GFP_NOIO;

	/*
	 * keep this for debugging for now...
	 */
	if (dma_addr != BLK_BOUNCE_HIGH && q != last_q) {
		printk("blk: queue %p, ", q);
		if (dma_addr == BLK_BOUNCE_ANY)
			printk("no I/O memory limit\n");
		else
			printk("I/O limit %luMb (mask 0x%Lx)\n", mb, (long long) dma_addr);
	}

	q->bounce_pfn = bounce_pfn;
	last_q = q;
}

EXPORT_SYMBOL(blk_queue_bounce_limit);

/**
 * blk_queue_max_sectors - set max sectors for a request for this queue
 * @q:  the request queue for the device
 * @max_sectors:  max sectors in the usual 512b unit
 *
 * Description:
 *    Enables a low level driver to set an upper limit on the size of
 *    received requests.
 **/
void blk_queue_max_sectors(request_queue_t *q, unsigned short max_sectors)
{
	if ((max_sectors << 9) < PAGE_CACHE_SIZE) {
		max_sectors = 1 << (PAGE_CACHE_SHIFT - 9);
		printk("%s: set to minimum %d\n", __FUNCTION__, max_sectors);
	}

	q->max_sectors = max_sectors;
}

EXPORT_SYMBOL(blk_queue_max_sectors);

/**
 * blk_queue_max_phys_segments - set max phys segments for a request for this queue
 * @q:  the request queue for the device
 * @max_segments:  max number of segments
 *
 * Description:
 *    Enables a low level driver to set an upper limit on the number of
 *    physical data segments in a request.  This would be the largest sized
 *    scatter list the driver could handle.
 **/
void blk_queue_max_phys_segments(request_queue_t *q, unsigned short max_segments)
{
	if (!max_segments) {
		max_segments = 1;
		printk("%s: set to minimum %d\n", __FUNCTION__, max_segments);
	}

	q->max_phys_segments = max_segments;
}

EXPORT_SYMBOL(blk_queue_max_phys_segments);

/**
 * blk_queue_max_hw_segments - set max hw segments for a request for this queue
 * @q:  the request queue for the device
 * @max_segments:  max number of segments
 *
 * Description:
 *    Enables a low level driver to set an upper limit on the number of
 *    hw data segments in a request.  This would be the largest number of
 *    address/length pairs the host adapter can actually give as once
 *    to the device.
 **/
void blk_queue_max_hw_segments(request_queue_t *q, unsigned short max_segments)
{
	if (!max_segments) {
		max_segments = 1;
		printk("%s: set to minimum %d\n", __FUNCTION__, max_segments);
	}

	q->max_hw_segments = max_segments;
}

EXPORT_SYMBOL(blk_queue_max_hw_segments);

/**
 * blk_queue_max_segment_size - set max segment size for blk_rq_map_sg
 * @q:  the request queue for the device
 * @max_size:  max size of segment in bytes
 *
 * Description:
 *    Enables a low level driver to set an upper limit on the size of a
 *    coalesced segment
 **/
void blk_queue_max_segment_size(request_queue_t *q, unsigned int max_size)
{
	if (max_size < PAGE_CACHE_SIZE) {
		max_size = PAGE_CACHE_SIZE;
		printk("%s: set to minimum %d\n", __FUNCTION__, max_size);
	}

	q->max_segment_size = max_size;
}

EXPORT_SYMBOL(blk_queue_max_segment_size);

/**
 * blk_queue_hardsect_size - set hardware sector size for the queue
 * @q:  the request queue for the device
 * @size:  the hardware sector size, in bytes
 *
 * Description:
 *   This should typically be set to the lowest possible sector size
 *   that the hardware can operate on (possible without reverting to
 *   even internal read-modify-write operations). Usually the default
 *   of 512 covers most hardware.
 **/
void blk_queue_hardsect_size(request_queue_t *q, unsigned short size)
{
	q->hardsect_size = size;
}

EXPORT_SYMBOL(blk_queue_hardsect_size);

/*
 * Returns the minimum that is _not_ zero, unless both are zero.
 */
#define min_not_zero(l, r) (l == 0) ? r : ((r == 0) ? l : min(l, r))

/**
 * blk_queue_stack_limits - inherit underlying queue limits for stacked drivers
 * @t:	the stacking driver (top)
 * @b:  the underlying device (bottom)
 **/
void blk_queue_stack_limits(request_queue_t *t, request_queue_t *b)
{
	/* zero is "infinity" */
	t->max_sectors = min_not_zero(t->max_sectors,b->max_sectors);

	t->max_phys_segments = min(t->max_phys_segments,b->max_phys_segments);
	t->max_hw_segments = min(t->max_hw_segments,b->max_hw_segments);
	t->max_segment_size = min(t->max_segment_size,b->max_segment_size);
	t->hardsect_size = max(t->hardsect_size,b->hardsect_size);
}

EXPORT_SYMBOL(blk_queue_stack_limits);

/**
 * blk_queue_segment_boundary - set boundary rules for segment merging
 * @q:  the request queue for the device
 * @mask:  the memory boundary mask
 **/
void blk_queue_segment_boundary(request_queue_t *q, unsigned long mask)
{
	if (mask < PAGE_CACHE_SIZE - 1) {
		mask = PAGE_CACHE_SIZE - 1;
		printk("%s: set to minimum %lx\n", __FUNCTION__, mask);
	}

	q->seg_boundary_mask = mask;
}

EXPORT_SYMBOL(blk_queue_segment_boundary);

/**
 * blk_queue_dma_alignment - set dma length and memory alignment
 * @q:     the request queue for the device
 * @mask:  alignment mask
 *
 * description:
 *    set required memory and length aligment for direct dma transactions.
 *    this is used when buiding direct io requests for the queue.
 *
 **/
void blk_queue_dma_alignment(request_queue_t *q, int mask)
{
	q->dma_alignment = mask;
}

EXPORT_SYMBOL(blk_queue_dma_alignment);

/**
 * blk_queue_find_tag - find a request by its tag and queue
 *
 * @q:	 The request queue for the device
 * @tag: The tag of the request
 *
 * Notes:
 *    Should be used when a device returns a tag and you want to match
 *    it with a request.
 *
 *    no locks need be held.
 **/
struct request *blk_queue_find_tag(request_queue_t *q, int tag)
{
	struct blk_queue_tag *bqt = q->queue_tags;

	if (unlikely(bqt == NULL || tag >= bqt->real_max_depth))
		return NULL;

	return bqt->tag_index[tag];
}

EXPORT_SYMBOL(blk_queue_find_tag);

/**
 * blk_queue_free_tags - release tag maintenance info
 * @q:  the request queue for the device
 *
 *  Notes:
 *    blk_cleanup_queue() will take care of calling this function, if tagging
 *    has been used. So there's usually no need to call this directly, unless
 *    tagging is just being disabled but the queue remains in function.
 **/
void blk_queue_free_tags(request_queue_t *q)
{
	struct blk_queue_tag *bqt = q->queue_tags;

	if (!bqt)
		return;

	if (atomic_dec_and_test(&bqt->refcnt)) {
		BUG_ON(bqt->busy);
		BUG_ON(!list_empty(&bqt->busy_list));

		kfree(bqt->tag_index);
		bqt->tag_index = NULL;

		kfree(bqt->tag_map);
		bqt->tag_map = NULL;

		kfree(bqt);
	}

	q->queue_tags = NULL;
	q->queue_flags &= ~(1 << QUEUE_FLAG_QUEUED);
}

EXPORT_SYMBOL(blk_queue_free_tags);

static int
init_tag_map(request_queue_t *q, struct blk_queue_tag *tags, int depth)
{
	int bits, i;

	if (depth > q->nr_requests * 2) {
		depth = q->nr_requests * 2;
		printk(KERN_ERR "%s: adjusted depth to %d\n",
				__FUNCTION__, depth);
	}

	tags->tag_index = kmalloc(depth * sizeof(struct request *), GFP_ATOMIC);
	if (!tags->tag_index)
		goto fail;

	bits = (depth / BLK_TAGS_PER_LONG) + 1;
	tags->tag_map = kmalloc(bits * sizeof(unsigned long), GFP_ATOMIC);
	if (!tags->tag_map)
		goto fail;

	memset(tags->tag_index, 0, depth * sizeof(struct request *));
	memset(tags->tag_map, 0, bits * sizeof(unsigned long));
	tags->max_depth = depth;
	tags->real_max_depth = bits * BITS_PER_LONG;

	/*
	 * set the upper bits if the depth isn't a multiple of the word size
	 */
	for (i = depth; i < bits * BLK_TAGS_PER_LONG; i++)
		__set_bit(i, tags->tag_map);

	INIT_LIST_HEAD(&tags->busy_list);
	tags->busy = 0;
	atomic_set(&tags->refcnt, 1);
	return 0;
fail:
	kfree(tags->tag_index);
	return -ENOMEM;
}

/**
 * blk_queue_init_tags - initialize the queue tag info
 * @q:  the request queue for the device
 * @depth:  the maximum queue depth supported
 **/
int blk_queue_init_tags(request_queue_t *q, int depth,
			struct blk_queue_tag *tags)
{
	if (!tags) {
		tags = kmalloc(sizeof(struct blk_queue_tag), GFP_ATOMIC);
		if (!tags)
			goto fail;

		if (init_tag_map(q, tags, depth))
			goto fail;
	} else
		atomic_inc(&tags->refcnt);

	/*
	 * assign it, all done
	 */
	q->queue_tags = tags;
	q->queue_flags |= (1 << QUEUE_FLAG_QUEUED);
	return 0;
fail:
	kfree(tags);
	return -ENOMEM;
}

EXPORT_SYMBOL(blk_queue_init_tags);

/**
 * blk_queue_resize_tags - change the queueing depth
 * @q:  the request queue for the device
 * @new_depth: the new max command queueing depth
 *
 *  Notes:
 *    Must be called with the queue lock held.
 **/
int blk_queue_resize_tags(request_queue_t *q, int new_depth)
{
	struct blk_queue_tag *bqt = q->queue_tags;
	struct request **tag_index;
	unsigned long *tag_map;
	int bits, max_depth;

	if (!bqt)
		return -ENXIO;

	/*
	 * don't bother sizing down
	 */
	if (new_depth <= bqt->real_max_depth) {
		bqt->max_depth = new_depth;
		return 0;
	}

	/*
	 * save the old state info, so we can copy it back
	 */
	tag_index = bqt->tag_index;
	tag_map = bqt->tag_map;
	max_depth = bqt->real_max_depth;

	if (init_tag_map(q, bqt, new_depth))
		return -ENOMEM;

	memcpy(bqt->tag_index, tag_index, max_depth * sizeof(struct request *));
	bits = max_depth / BLK_TAGS_PER_LONG;
	memcpy(bqt->tag_map, tag_map, bits * sizeof(unsigned long));

	kfree(tag_index);
	kfree(tag_map);
	return 0;
}

/**
 * blk_queue_end_tag - end tag operations for a request
 * @q:  the request queue for the device
 * @rq: the request that has completed
 *
 *  Description:
 *    Typically called when end_that_request_first() returns 0, meaning
 *    all transfers have been done for a request. It's important to call
 *    this function before end_that_request_last(), as that will put the
 *    request back on the free list thus corrupting the internal tag list.
 *
 *  Notes:
 *   queue lock must be held.
 **/
void blk_queue_end_tag(request_queue_t *q, struct request *rq)
{
	struct blk_queue_tag *bqt = q->queue_tags;
	int tag = rq->tag;

	BUG_ON(tag == -1);

	if (unlikely(tag >= bqt->real_max_depth))
		return;

	if (unlikely(!__test_and_clear_bit(tag, bqt->tag_map))) {
		printk("attempt to clear non-busy tag (%d)\n", tag);
		return;
	}

	list_del_init(&rq->queuelist);
	rq->flags &= ~REQ_QUEUED;
	rq->tag = -1;

	if (unlikely(bqt->tag_index[tag] == NULL))
		printk("tag %d is missing\n", tag);

	bqt->tag_index[tag] = NULL;
	bqt->busy--;
}

EXPORT_SYMBOL(blk_queue_end_tag);

/**
 * blk_queue_start_tag - find a free tag and assign it
 * @q:  the request queue for the device
 * @rq:  the block request that needs tagging
 *
 *  Description:
 *    This can either be used as a stand-alone helper, or possibly be
 *    assigned as the queue &prep_rq_fn (in which case &struct request
 *    automagically gets a tag assigned). Note that this function
 *    assumes that any type of request can be queued! if this is not
 *    true for your device, you must check the request type before
 *    calling this function.  The request will also be removed from
 *    the request queue, so it's the drivers responsibility to readd
 *    it if it should need to be restarted for some reason.
 *
 *  Notes:
 *   queue lock must be held.
 **/
int blk_queue_start_tag(request_queue_t *q, struct request *rq)
{
	struct blk_queue_tag *bqt = q->queue_tags;
	unsigned long *map = bqt->tag_map;
	int tag = 0;

	if (unlikely((rq->flags & REQ_QUEUED))) {
		printk(KERN_ERR 
		       "request %p for device [%s] already tagged %d",
		       rq, rq->rq_disk ? rq->rq_disk->disk_name : "?", rq->tag);
		BUG();
	}

	for (map = bqt->tag_map; *map == -1UL; map++) {
		tag += BLK_TAGS_PER_LONG;

		if (tag >= bqt->max_depth)
			return 1;
	}

	tag += ffz(*map);
	__set_bit(tag, bqt->tag_map);

	rq->flags |= REQ_QUEUED;
	rq->tag = tag;
	bqt->tag_index[tag] = rq;
	blkdev_dequeue_request(rq);
	list_add(&rq->queuelist, &bqt->busy_list);
	bqt->busy++;
	return 0;
}

EXPORT_SYMBOL(blk_queue_start_tag);

/**
 * blk_queue_invalidate_tags - invalidate all pending tags
 * @q:  the request queue for the device
 *
 *  Description:
 *   Hardware conditions may dictate a need to stop all pending requests.
 *   In this case, we will safely clear the block side of the tag queue and
 *   readd all requests to the request queue in the right order.
 *
 *  Notes:
 *   queue lock must be held.
 **/
void blk_queue_invalidate_tags(request_queue_t *q)
{
	struct blk_queue_tag *bqt = q->queue_tags;
	struct list_head *tmp, *n;
	struct request *rq;

	list_for_each_safe(tmp, n, &bqt->busy_list) {
		rq = list_entry_rq(tmp);

		if (rq->tag == -1) {
			printk("bad tag found on list\n");
			list_del_init(&rq->queuelist);
			rq->flags &= ~REQ_QUEUED;
		} else
			blk_queue_end_tag(q, rq);

		rq->flags &= ~REQ_STARTED;
		__elv_add_request(q, rq, ELEVATOR_INSERT_BACK, 0);
	}
}

EXPORT_SYMBOL(blk_queue_invalidate_tags);

static char *rq_flags[] = {
	"REQ_RW",
	"REQ_FAILFAST",
	"REQ_SOFTBARRIER",
	"REQ_HARDBARRIER",
	"REQ_CMD",
	"REQ_NOMERGE",
	"REQ_STARTED",
	"REQ_DONTPREP",
	"REQ_QUEUED",
	"REQ_PC",
	"REQ_BLOCK_PC",
	"REQ_SENSE",
	"REQ_FAILED",
	"REQ_QUIET",
	"REQ_SPECIAL",
	"REQ_DRIVE_CMD",
	"REQ_DRIVE_TASK",
	"REQ_DRIVE_TASKFILE",
	"REQ_PREEMPT",
	"REQ_PM_SUSPEND",
	"REQ_PM_RESUME",
	"REQ_PM_SHUTDOWN",
	"REQ_IDETAPE_PC1",
	"REQ_IDETAPE_PC2",
	"REQ_IDETAPE_READ",
	"REQ_IDETAPE_WRITE",
	"REQ_IDETAPE_READ_BUFFER",
};

void blk_dump_rq_flags(struct request *rq, char *msg)
{
	int bit;

	printk("%s: dev %s: flags = ", msg,
		rq->rq_disk ? rq->rq_disk->disk_name : "?");
	bit = 0;
	do {
		if (rq->flags & (1 << bit))
			printk("%s ", rq_flags[bit]);
		bit++;
	} while (bit < __REQ_NR_BITS);

	printk("\nsector %llu, nr/cnr %lu/%u\n", (unsigned long long)rq->sector,
						       rq->nr_sectors,
						       rq->current_nr_sectors);
	printk("bio %p, biotail %p, buffer %p, data %p, len %u\n", rq->bio, rq->biotail, rq->buffer, rq->data, rq->data_len);

	if (rq->flags & (REQ_BLOCK_PC | REQ_PC)) {
		printk("cdb: ");
		for (bit = 0; bit < sizeof(rq->cmd); bit++)
			printk("%02x ", rq->cmd[bit]);
		printk("\n");
	}
}

EXPORT_SYMBOL(blk_dump_rq_flags);

void blk_recount_segments(request_queue_t *q, struct bio *bio)
{
	struct bio_vec *bv, *bvprv = NULL;
	int i, nr_phys_segs, nr_hw_segs, seg_size, cluster;
	int high, highprv = 1;

	if (unlikely(!bio->bi_io_vec))
		return;

	cluster = q->queue_flags & (1 << QUEUE_FLAG_CLUSTER);
	seg_size = nr_phys_segs = nr_hw_segs = 0;
	bio_for_each_segment(bv, bio, i) {
		/*
		 * the trick here is making sure that a high page is never
		 * considered part of another segment, since that might
		 * change with the bounce page.
		 */
		high = page_to_pfn(bv->bv_page) >= q->bounce_pfn;
		if (high || highprv)
			goto new_hw_segment;
		if (cluster) {
			if (seg_size + bv->bv_len > q->max_segment_size)
				goto new_segment;
			if (!BIOVEC_PHYS_MERGEABLE(bvprv, bv))
				goto new_segment;
			if (!BIOVEC_SEG_BOUNDARY(q, bvprv, bv))
				goto new_segment;

			seg_size += bv->bv_len;
			bvprv = bv;
			continue;
		}
new_segment:
		if (!BIOVEC_VIRT_MERGEABLE(bvprv, bv))
new_hw_segment:
			nr_hw_segs++;

		nr_phys_segs++;
		bvprv = bv;
		seg_size = bv->bv_len;
		highprv = high;
	}

	bio->bi_phys_segments = nr_phys_segs;
	bio->bi_hw_segments = nr_hw_segs;
	bio->bi_flags |= (1 << BIO_SEG_VALID);
}


int blk_phys_contig_segment(request_queue_t *q, struct bio *bio,
				   struct bio *nxt)
{
	if (!(q->queue_flags & (1 << QUEUE_FLAG_CLUSTER)))
		return 0;

	if (!BIOVEC_PHYS_MERGEABLE(__BVEC_END(bio), __BVEC_START(nxt)))
		return 0;
	if (bio->bi_size + nxt->bi_size > q->max_segment_size)
		return 0;

	/*
	 * bio and nxt are contigous in memory, check if the queue allows
	 * these two to be merged into one
	 */
	if (BIO_SEG_BOUNDARY(q, bio, nxt))
		return 1;

	return 0;
}

EXPORT_SYMBOL(blk_phys_contig_segment);

int blk_hw_contig_segment(request_queue_t *q, struct bio *bio,
				 struct bio *nxt)
{
	if (!(q->queue_flags & (1 << QUEUE_FLAG_CLUSTER)))
		return 0;

	if (!BIOVEC_VIRT_MERGEABLE(__BVEC_END(bio), __BVEC_START(nxt)))
		return 0;
	if (bio->bi_size + nxt->bi_size > q->max_segment_size)
		return 0;

	/*
	 * bio and nxt are contigous in memory, check if the queue allows
	 * these two to be merged into one
	 */
	if (BIO_SEG_BOUNDARY(q, bio, nxt))
		return 1;

	return 0;
}

EXPORT_SYMBOL(blk_hw_contig_segment);

/*
 * map a request to scatterlist, return number of sg entries setup. Caller
 * must make sure sg can hold rq->nr_phys_segments entries
 */
int blk_rq_map_sg(request_queue_t *q, struct request *rq, struct scatterlist *sg)
{
	struct bio_vec *bvec, *bvprv;
	struct bio *bio;
	int nsegs, i, cluster;

	nsegs = 0;
	cluster = q->queue_flags & (1 << QUEUE_FLAG_CLUSTER);

	/*
	 * for each bio in rq
	 */
	bvprv = NULL;
	rq_for_each_bio(bio, rq) {
		/*
		 * for each segment in bio
		 */
		bio_for_each_segment(bvec, bio, i) {
			int nbytes = bvec->bv_len;

			if (bvprv && cluster) {
				if (sg[nsegs - 1].length + nbytes > q->max_segment_size)
					goto new_segment;

				if (!BIOVEC_PHYS_MERGEABLE(bvprv, bvec))
					goto new_segment;
				if (!BIOVEC_SEG_BOUNDARY(q, bvprv, bvec))
					goto new_segment;

				sg[nsegs - 1].length += nbytes;
			} else {
new_segment:
				memset(&sg[nsegs],0,sizeof(struct scatterlist));
				sg[nsegs].page = bvec->bv_page;
				sg[nsegs].length = nbytes;
				sg[nsegs].offset = bvec->bv_offset;

				nsegs++;
			}
			bvprv = bvec;
		} /* segments in bio */
	} /* bios in rq */

	return nsegs;
}

EXPORT_SYMBOL(blk_rq_map_sg);

/*
 * the standard queue merge functions, can be overridden with device
 * specific ones if so desired
 */

static inline int ll_new_mergeable(request_queue_t *q,
				   struct request *req,
				   struct bio *bio)
{
	int nr_phys_segs = bio_phys_segments(q, bio);

	if (req->nr_phys_segments + nr_phys_segs > q->max_phys_segments) {
		req->flags |= REQ_NOMERGE;
		q->last_merge = NULL;
		return 0;
	}

	/*
	 * A hw segment is just getting larger, bump just the phys
	 * counter.
	 */
	req->nr_phys_segments += nr_phys_segs;
	return 1;
}

static inline int ll_new_hw_segment(request_queue_t *q,
				    struct request *req,
				    struct bio *bio)
{
	int nr_hw_segs = bio_hw_segments(q, bio);
	int nr_phys_segs = bio_phys_segments(q, bio);

	if (req->nr_hw_segments + nr_hw_segs > q->max_hw_segments
	    || req->nr_phys_segments + nr_phys_segs > q->max_phys_segments) {
		req->flags |= REQ_NOMERGE;
		q->last_merge = NULL;
		return 0;
	}

	/*
	 * This will form the start of a new hw segment.  Bump both
	 * counters.
	 */
	req->nr_hw_segments += nr_hw_segs;
	req->nr_phys_segments += nr_phys_segs;
	return 1;
}

static int ll_back_merge_fn(request_queue_t *q, struct request *req, 
			    struct bio *bio)
{
	if (req->nr_sectors + bio_sectors(bio) > q->max_sectors) {
		req->flags |= REQ_NOMERGE;
		q->last_merge = NULL;
		return 0;
	}

	if (BIOVEC_VIRT_MERGEABLE(__BVEC_END(req->biotail), __BVEC_START(bio)))
		return ll_new_mergeable(q, req, bio);

	return ll_new_hw_segment(q, req, bio);
}

static int ll_front_merge_fn(request_queue_t *q, struct request *req, 
			     struct bio *bio)
{
	if (req->nr_sectors + bio_sectors(bio) > q->max_sectors) {
		req->flags |= REQ_NOMERGE;
		q->last_merge = NULL;
		return 0;
	}

	if (BIOVEC_VIRT_MERGEABLE(__BVEC_END(bio), __BVEC_START(req->bio)))
		return ll_new_mergeable(q, req, bio);

	return ll_new_hw_segment(q, req, bio);
}

static int ll_merge_requests_fn(request_queue_t *q, struct request *req,
				struct request *next)
{
	int total_phys_segments = req->nr_phys_segments +next->nr_phys_segments;
	int total_hw_segments = req->nr_hw_segments + next->nr_hw_segments;

	/*
	 * First check if the either of the requests are re-queued
	 * requests.  Can't merge them if they are.
	 */
	if (req->special || next->special)
		return 0;

	/*
	 * Will it become to large?
	 */
	if ((req->nr_sectors + next->nr_sectors) > q->max_sectors)
		return 0;

	total_phys_segments = req->nr_phys_segments + next->nr_phys_segments;
	if (blk_phys_contig_segment(q, req->biotail, next->bio))
		total_phys_segments--;

	if (total_phys_segments > q->max_phys_segments)
		return 0;

	total_hw_segments = req->nr_hw_segments + next->nr_hw_segments;
	if (blk_hw_contig_segment(q, req->biotail, next->bio))
		total_hw_segments--;

	if (total_hw_segments > q->max_hw_segments)
		return 0;

	/* Merge is OK... */
	req->nr_phys_segments = total_phys_segments;
	req->nr_hw_segments = total_hw_segments;
	return 1;
}

/*
 * "plug" the device if there are no outstanding requests: this will
 * force the transfer to start only after we have put all the requests
 * on the list.
 *
 * This is called with interrupts off and no requests on the queue and
 * with the queue lock held.
 */
void blk_plug_device(request_queue_t *q)
{
	WARN_ON(!irqs_disabled());

	/*
	 * don't plug a stopped queue, it must be paired with blk_start_queue()
	 * which will restart the queueing
	 */
	if (!blk_queue_plugged(q)
	    && !test_bit(QUEUE_FLAG_STOPPED, &q->queue_flags)) {
		spin_lock(&blk_plug_lock);
		list_add_tail(&q->plug_list, &blk_plug_list);
		mod_timer(&q->unplug_timer, jiffies + q->unplug_delay);
		spin_unlock(&blk_plug_lock);
	}
}

EXPORT_SYMBOL(blk_plug_device);

/*
 * remove the queue from the plugged list, if present. called with
 * queue lock held and interrupts disabled.
 */
int blk_remove_plug(request_queue_t *q)
{
	WARN_ON(!irqs_disabled());
	if (blk_queue_plugged(q)) {
		spin_lock(&blk_plug_lock);
		list_del_init(&q->plug_list);
		del_timer(&q->unplug_timer);
		spin_unlock(&blk_plug_lock);
		return 1;
	}

	return 0;
}

EXPORT_SYMBOL(blk_remove_plug);

/*
 * remove the plug and let it rip..
 */
static inline void __generic_unplug_device(request_queue_t *q)
{
	if (test_bit(QUEUE_FLAG_STOPPED, &q->queue_flags))
		return;

	if (!blk_remove_plug(q))
		return;

	del_timer(&q->unplug_timer);

	/*
	 * was plugged, fire request_fn if queue has stuff to do
	 */
	if (elv_next_request(q))
		q->request_fn(q);
}

/**
 * generic_unplug_device - fire a request queue
 * @data:    The &request_queue_t in question
 *
 * Description:
 *   Linux uses plugging to build bigger requests queues before letting
 *   the device have at them. If a queue is plugged, the I/O scheduler
 *   is still adding and merging requests on the queue. Once the queue
 *   gets unplugged (either by manually calling this function, or by
 *   calling blk_run_queues()), the request_fn defined for the
 *   queue is invoked and transfers started.
 **/
void generic_unplug_device(void *data)
{
	request_queue_t *q = data;

	spin_lock_irq(q->queue_lock);
	__generic_unplug_device(q);
	spin_unlock_irq(q->queue_lock);
}

EXPORT_SYMBOL(generic_unplug_device);

static void blk_unplug_work(void *data)
{
	request_queue_t *q = data;
	q->unplug_fn(q);
}

static void blk_unplug_timeout(unsigned long data)
{
	request_queue_t *q = (request_queue_t *)data;

	kblockd_schedule_work(&q->unplug_work);
}

/**
 * blk_start_queue - restart a previously stopped queue
 * @q:    The &request_queue_t in question
 *
 * Description:
 *   blk_start_queue() will clear the stop flag on the queue, and call
 *   the request_fn for the queue if it was in a stopped state when
 *   entered. Also see blk_stop_queue(). Must not be called from driver
 *   request function due to recursion issues. Queue lock must be held.
 **/
void blk_start_queue(request_queue_t *q)
{
	clear_bit(QUEUE_FLAG_STOPPED, &q->queue_flags);
	schedule_work(&q->unplug_work);
}

EXPORT_SYMBOL(blk_start_queue);

/**
 * blk_stop_queue - stop a queue
 * @q:    The &request_queue_t in question
 *
 * Description:
 *   The Linux block layer assumes that a block driver will consume all
 *   entries on the request queue when the request_fn strategy is called.
 *   Often this will not happen, because of hardware limitations (queue
 *   depth settings). If a device driver gets a 'queue full' response,
 *   or if it simply chooses not to queue more I/O at one point, it can
 *   call this function to prevent the request_fn from being called until
 *   the driver has signalled it's ready to go again. This happens by calling
 *   blk_start_queue() to restart queue operations. Queue lock must be held.
 **/
void blk_stop_queue(request_queue_t *q)
{
	blk_remove_plug(q);
	set_bit(QUEUE_FLAG_STOPPED, &q->queue_flags);
}

EXPORT_SYMBOL(blk_stop_queue);

/**
 * blk_run_queue - run a single device queue
 * @q:	The queue to run
 */
void blk_run_queue(struct request_queue *q)
{
	unsigned long flags;

	spin_lock_irqsave(q->queue_lock, flags);
	blk_remove_plug(q);
	q->request_fn(q);
	spin_unlock_irqrestore(q->queue_lock, flags);
}

EXPORT_SYMBOL(blk_run_queue);

/**
 * blk_run_queues - fire all plugged queues
 *
 * Description:
 *   Start I/O on all plugged queues known to the block layer. Queues that
 *   are currently stopped are ignored. This is equivalent to the older
 *   tq_disk task queue run.
 **/
#define blk_plug_entry(entry) list_entry((entry), request_queue_t, plug_list)
void blk_run_queues(void)
{
	LIST_HEAD(local_plug_list);

	spin_lock_irq(&blk_plug_lock);

	/*
	 * this will happen fairly often
	 */
	if (list_empty(&blk_plug_list))
		goto out;

	list_splice_init(&blk_plug_list, &local_plug_list);
	
	while (!list_empty(&local_plug_list)) {
		request_queue_t *q = blk_plug_entry(local_plug_list.next);

		spin_unlock_irq(&blk_plug_lock);
		q->unplug_fn(q);
		spin_lock_irq(&blk_plug_lock);
	}
out:
	spin_unlock_irq(&blk_plug_lock);
}

EXPORT_SYMBOL(blk_run_queues);

/**
 * blk_cleanup_queue: - release a &request_queue_t when it is no longer needed
 * @q:    the request queue to be released
 *
 * Description:
 *     blk_cleanup_queue is the pair to blk_init_queue() or
 *     blk_queue_make_request().  It should be called when a request queue is
 *     being released; typically when a block device is being de-registered.
 *     Currently, its primary task it to free all the &struct request
 *     structures that were allocated to the queue and the queue itself.
 *
 * Caveat:
 *     Hopefully the low level driver will have finished any
 *     outstanding requests first...
 **/
void blk_cleanup_queue(request_queue_t * q)
{
	struct request_list *rl = &q->rq;

	if (!atomic_dec_and_test(&q->refcnt))
		return;

	elevator_exit(q);

	del_timer_sync(&q->unplug_timer);
	kblockd_flush();

	if (rl->rq_pool)
		mempool_destroy(rl->rq_pool);

	if (blk_queue_tagged(q))
		blk_queue_free_tags(q);

	kfree(q);
}

EXPORT_SYMBOL(blk_cleanup_queue);

static int blk_init_free_list(request_queue_t *q)
{
	struct request_list *rl = &q->rq;

	rl->count[READ] = rl->count[WRITE] = 0;
	init_waitqueue_head(&rl->wait[READ]);
	init_waitqueue_head(&rl->wait[WRITE]);

	rl->rq_pool = mempool_create(BLKDEV_MIN_RQ, mempool_alloc_slab, mempool_free_slab, request_cachep);

	if (!rl->rq_pool)
		return -ENOMEM;

	return 0;
}

static int __make_request(request_queue_t *, struct bio *);

// 老子就知道CONFIG_IOSCHED_AS、CONFIG_IOSCHED_DEADLINE、CONFIG_IOSCHED_NOOP这三个玩意是make打包的时候规定的。
// makefile好像是xml配置文件一样，给出很多开关选项，让用户通过配置文件开启。
static elevator_t *chosen_elevator =
#if defined(CONFIG_IOSCHED_AS)
	&iosched_as;
#elif defined(CONFIG_IOSCHED_DEADLINE)
	&iosched_deadline;
#elif defined(CONFIG_IOSCHED_NOOP)
	&elevator_noop;
#else
	NULL;
#error "You must have at least 1 I/O scheduler selected"
#endif

#if defined(CONFIG_IOSCHED_AS) || defined(CONFIG_IOSCHED_DEADLINE) || defined (CONFIG_IOSCHED_NOOP)
static int __init elevator_setup(char *str)
{
#ifdef CONFIG_IOSCHED_DEADLINE
	if (!strcmp(str, "deadline"))
		chosen_elevator = &iosched_deadline;
#endif
#ifdef CONFIG_IOSCHED_AS
	if (!strcmp(str, "as"))
		chosen_elevator = &iosched_as;
#endif
#ifdef CONFIG_IOSCHED_NOOP
	if (!strcmp(str, "noop"))
		chosen_elevator = &elevator_noop;
#endif
	return 1;
}

__setup("elevator=", elevator_setup);
#endif /* CONFIG_IOSCHED_AS || CONFIG_IOSCHED_DEADLINE || CONFIG_IOSCHED_NOOP */

request_queue_t *blk_alloc_queue(int gfp_mask)
{
	request_queue_t *q = kmalloc(sizeof(*q), gfp_mask);

	if (!q)
		return NULL;

	memset(q, 0, sizeof(*q));
	init_timer(&q->unplug_timer);
	atomic_set(&q->refcnt, 1);
	return q;
}

EXPORT_SYMBOL(blk_alloc_queue);

/**
 * blk_init_queue  - prepare a request queue for use with a block device
 * @rfn:  The function to be called to process requests that have been
 *        placed on the queue.
 * @lock: Request queue spin lock
 *
 * Description:
 *    If a block device wishes to use the standard request handling procedures,
 *    which sorts requests and coalesces adjacent requests, then it must
 *    call blk_init_queue().  The function @rfn will be called when there
 *    are requests on the queue that need to be processed.  If the device
 *    supports plugging, then @rfn may not be called immediately when requests
 *    are available on the queue, but may be called at some time later instead.
 *    Plugged queues are generally unplugged when a buffer belonging to one
 *    of the requests on the queue is needed, or due to memory pressure.
 *
 *    @rfn is not required, or even expected, to remove all requests off the
 *    queue, but only as many as it can handle at a time.  If it does leave
 *    requests on the queue, it is responsible for arranging that the requests
 *    get dealt with eventually.
 *
 *    The queue spin lock must be held while manipulating the requests on the
 *    request queue.
 *
 *    Function returns a pointer to the initialized request queue, or NULL if
 *    it didn't succeed.
 *
 * Note:
 *    blk_init_queue() must be paired with a blk_cleanup_queue() call
 *    when the block device is deactivated (such as at module unload).
 **/
 // 在hd.c中hd_init方法中调用。也就是初始化硬盘的队列以及硬盘的处理函数。
 // 也就是说request_queue_t是一个抽象，具体的设备来实现。比如这里就是使用hd的。
 // 然后一个request_queue_t中有电梯算法，而这个电梯算法是实现了几种，具体用户使用那种通过makefile让用户选择
request_queue_t *blk_init_queue(request_fn_proc *rfn, spinlock_t *lock)
{
	request_queue_t *q;
	static int printed;

	// 内存中开一个request_queue_t
	q = blk_alloc_queue(GFP_KERNEL);
	
	if (!q)
		return NULL;

	if (blk_init_free_list(q))
		goto out_init;

	if (!printed) {
		printed = 1;
		printk("Using %s io scheduler\n", chosen_elevator->elevator_name);
	}

	// 对电梯做初始化。  chosen_elevator 这个是选择的电梯算法。
	if (elevator_init(q, chosen_elevator))
		goto out_elv;

	q->request_fn		= rfn;
	q->back_merge_fn       	= ll_back_merge_fn;
	q->front_merge_fn      	= ll_front_merge_fn;
	q->merge_requests_fn	= ll_merge_requests_fn;
	q->prep_rq_fn		= NULL;
	q->unplug_fn		= generic_unplug_device;
	q->queue_flags		= (1 << QUEUE_FLAG_CLUSTER);
	q->queue_lock		= lock;

	blk_queue_segment_boundary(q, 0xffffffff);

	// make_request_fn函数指针的具体实现__make_request
	blk_queue_make_request(q, __make_request);
	
	blk_queue_max_segment_size(q, MAX_SEGMENT_SIZE);

	blk_queue_max_hw_segments(q, MAX_HW_SEGMENTS);
	blk_queue_max_phys_segments(q, MAX_PHYS_SEGMENTS);

	return q;
out_elv:
	blk_cleanup_queue(q);
out_init:
	kfree(q);
	return NULL;
}

EXPORT_SYMBOL(blk_init_queue);

int blk_get_queue(request_queue_t *q)
{
	if (!test_bit(QUEUE_FLAG_DEAD, &q->queue_flags)) {
		atomic_inc(&q->refcnt);
		return 0;
	}

	return 1;
}

EXPORT_SYMBOL(blk_get_queue);

static inline void blk_free_request(request_queue_t *q, struct request *rq)
{
	elv_put_request(q, rq);
	mempool_free(rq, q->rq.rq_pool);
}

static inline struct request *blk_alloc_request(request_queue_t *q,int gfp_mask)
{
	struct request *rq = mempool_alloc(q->rq.rq_pool, gfp_mask);

	if (!rq)
		return NULL;

	if (!elv_set_request(q, rq, gfp_mask))
		return rq;

	mempool_free(rq, q->rq.rq_pool);
	return NULL;
}

/*
 * ioc_batching returns true if the ioc is a valid batching request and
 * should be given priority access to a request.
 */
static inline int ioc_batching(struct io_context *ioc)
{
	if (!ioc)
		return 0;

	/*
	 * Make sure the process is able to allocate at least 1 request
	 * even if the batch times out, otherwise we could theoretically
	 * lose wakeups.
	 */
	return ioc->nr_batch_requests == BLK_BATCH_REQ ||
		(ioc->nr_batch_requests > 0
		&& time_before(jiffies, ioc->last_waited + BLK_BATCH_TIME));
}

/*
 * ioc_set_batching sets ioc to be a new "batcher" if it is not one. This
 * will cause the process to be a "batcher" on all queues in the system. This
 * is the behaviour we want though - once it gets a wakeup it should be given
 * a nice run.
 */
void ioc_set_batching(struct io_context *ioc)
{
	if (!ioc || ioc_batching(ioc))
		return;

	ioc->nr_batch_requests = BLK_BATCH_REQ;
	ioc->last_waited = jiffies;
}

/*
 * A request has just been released.  Account for it, update the full and
 * congestion status, wake up any waiters.   Called under q->queue_lock.
 */
static void freed_request(request_queue_t *q, int rw)
{
	struct request_list *rl = &q->rq;

	rl->count[rw]--;
	if (rl->count[rw] < queue_congestion_off_threshold(q))
		clear_queue_congested(q, rw);
	if (rl->count[rw]+1 <= q->nr_requests) {
		smp_mb();
		if (waitqueue_active(&rl->wait[rw]))
			wake_up(&rl->wait[rw]);
		if (!waitqueue_active(&rl->wait[rw]))
			blk_clear_queue_full(q, rw);
	}
}

#define blkdev_free_rq(list) list_entry((list)->next, struct request, queuelist)
/*
 * Get a free request, queue_lock must not be held
 */
static struct request *get_request(request_queue_t *q, int rw, int gfp_mask)
{
	struct request *rq = NULL;
	struct request_list *rl = &q->rq;
	struct io_context *ioc = get_io_context(gfp_mask);

	spin_lock_irq(q->queue_lock);
	if (rl->count[rw]+1 >= q->nr_requests) {
		/*
		 * The queue will fill after this allocation, so set it as
		 * full, and mark this process as "batching". This process
		 * will be allowed to complete a batch of requests, others
		 * will be blocked.
		 */
		if (!blk_queue_full(q, rw)) {
			ioc_set_batching(ioc);
			blk_set_queue_full(q, rw);
		}
	}

	if (blk_queue_full(q, rw)
			&& !ioc_batching(ioc) && !elv_may_queue(q, rw)) {
		/*
		 * The queue is full and the allocating process is not a
		 * "batcher", and not exempted by the IO scheduler
		 */
		spin_unlock_irq(q->queue_lock);
		goto out;
	}

	rl->count[rw]++;
	if (rl->count[rw] >= queue_congestion_on_threshold(q))
		set_queue_congested(q, rw);
	spin_unlock_irq(q->queue_lock);

	rq = blk_alloc_request(q, gfp_mask);
	if (!rq) {
		/*
		 * Allocation failed presumably due to memory. Undo anything
		 * we might have messed up.
		 *
		 * Allocating task should really be put onto the front of the
		 * wait queue, but this is pretty rare.
		 */
		spin_lock_irq(q->queue_lock);
		freed_request(q, rw);
		spin_unlock_irq(q->queue_lock);
		goto out;
	}

	if (ioc_batching(ioc))
		ioc->nr_batch_requests--;
	
	INIT_LIST_HEAD(&rq->queuelist);

	/*
	 * first three bits are identical in rq->flags and bio->bi_rw,
	 * see bio.h and blkdev.h
	 */
	rq->flags = rw;

	rq->errors = 0;
	rq->rq_status = RQ_ACTIVE;
	rq->bio = rq->biotail = NULL;
	rq->buffer = NULL;
	rq->ref_count = 1;
	rq->q = q;
	rq->rl = rl;
	rq->waiting = NULL;
	rq->special = NULL;
	rq->data = NULL;
	rq->sense = NULL;

out:
	put_io_context(ioc);
	return rq;
}

/*
 * No available requests for this queue, unplug the device and wait for some
 * requests to become available.
 */
static struct request *get_request_wait(request_queue_t *q, int rw)
{
	DEFINE_WAIT(wait);
	struct request *rq;

	generic_unplug_device(q);
	do {
		struct request_list *rl = &q->rq;

		prepare_to_wait_exclusive(&rl->wait[rw], &wait,
				TASK_UNINTERRUPTIBLE);

		rq = get_request(q, rw, GFP_NOIO);

		if (!rq) {
			struct io_context *ioc;

			io_schedule();

			/*
			 * After sleeping, we become a "batching" process and
			 * will be able to allocate at least one request, and
			 * up to a big batch of them for a small period time.
			 * See ioc_batching, ioc_set_batching
			 */
			ioc = get_io_context(GFP_NOIO);
			ioc_set_batching(ioc);
			put_io_context(ioc);
		}
		finish_wait(&rl->wait[rw], &wait);
	} while (!rq);

	return rq;
}

struct request *blk_get_request(request_queue_t *q, int rw, int gfp_mask)
{
	struct request *rq;

	BUG_ON(rw != READ && rw != WRITE);

	if (gfp_mask & __GFP_WAIT)
		rq = get_request_wait(q, rw);
	else
		rq = get_request(q, rw, gfp_mask);

	return rq;
}

EXPORT_SYMBOL(blk_get_request);

/**
 * blk_requeue_request - put a request back on queue
 * @q:		request queue where request should be inserted
 * @rq:		request to be inserted
 *
 * Description:
 *    Drivers often keep queueing requests until the hardware cannot accept
 *    more, when that condition happens we need to put the request back
 *    on the queue. Must be called with queue lock held.
 */
void blk_requeue_request(request_queue_t *q, struct request *rq)
{
	if (blk_rq_tagged(rq))
		blk_queue_end_tag(q, rq);

	elv_requeue_request(q, rq);
}

EXPORT_SYMBOL(blk_requeue_request);

/**
 * blk_insert_request - insert a special request in to a request queue
 * @q:		request queue where request should be inserted
 * @rq:		request to be inserted
 * @at_head:	insert request at head or tail of queue
 * @data:	private data
 * @reinsert:	true if request it a reinsertion of previously processed one
 *
 * Description:
 *    Many block devices need to execute commands asynchronously, so they don't
 *    block the whole kernel from preemption during request execution.  This is
 *    accomplished normally by inserting aritficial requests tagged as
 *    REQ_SPECIAL in to the corresponding request queue, and letting them be
 *    scheduled for actual execution by the request queue.
 *
 *    We have the option of inserting the head or the tail of the queue.
 *    Typically we use the tail for new ioctls and so forth.  We use the head
 *    of the queue for things like a QUEUE_FULL message from a device, or a
 *    host that is unable to accept a particular command.
 */
void blk_insert_request(request_queue_t *q, struct request *rq,
			int at_head, void *data, int reinsert)
{
	unsigned long flags;

	/*
	 * tell I/O scheduler that this isn't a regular read/write (ie it
	 * must not attempt merges on this) and that it acts as a soft
	 * barrier
	 */
	rq->flags |= REQ_SPECIAL | REQ_SOFTBARRIER;

	rq->special = data;

	spin_lock_irqsave(q->queue_lock, flags);

	/*
	 * If command is tagged, release the tag
	 */
	if(reinsert) {
		blk_requeue_request(q, rq);
	} else {
		int where = ELEVATOR_INSERT_BACK;

		if (at_head)
			where = ELEVATOR_INSERT_FRONT;

		if (blk_rq_tagged(rq))
			blk_queue_end_tag(q, rq);

		drive_stat_acct(rq, rq->nr_sectors, 1);
		__elv_add_request(q, rq, where, 0);
	}
	q->request_fn(q);
	spin_unlock_irqrestore(q->queue_lock, flags);
}

EXPORT_SYMBOL(blk_insert_request);

void drive_stat_acct(struct request *rq, int nr_sectors, int new_io)
{
	int rw = rq_data_dir(rq);

	if (!blk_fs_request(rq) || !rq->rq_disk)
		return;

	if (rw == READ) {
		disk_stat_add(rq->rq_disk, read_sectors, nr_sectors);
		if (!new_io)
			disk_stat_inc(rq->rq_disk, read_merges);
	} else if (rw == WRITE) {
		disk_stat_add(rq->rq_disk, write_sectors, nr_sectors);
		if (!new_io)
			disk_stat_inc(rq->rq_disk, write_merges);
	}
	if (new_io) {
		disk_round_stats(rq->rq_disk);
		rq->rq_disk->in_flight++;
	}
}

/*
 * add-request adds a request to the linked list.
 * queue lock is held and interrupts disabled, as we muck with the
 * request queue list.
 */
static inline void add_request(request_queue_t * q, struct request * req)
{
	drive_stat_acct(req, req->nr_sectors, 1);

	// q->activity_fn这个操作并不是调用这个函数指针的实现体。而是判断是否存在这个函数指针
	if (q->activity_fn)

		// 调用activity_fn函数指针对应的实现体。
		q->activity_fn(q->activity_data, rq_data_dir(req));

	/*
	 * elevator indicated where it wants this request to be
	 * inserted at elevator_merge time
	 */
	 // 走这里，因为对于hd的q->activity_fn初始化是null
	__elv_add_request(q, req, ELEVATOR_INSERT_SORT, 0);
}
 
/*
 * disk_round_stats()	- Round off the performance stats on a struct
 * disk_stats.
 *
 * The average IO queue length and utilisation statistics are maintained
 * by observing the current state of the queue length and the amount of
 * time it has been in this state for.
 *
 * Normally, that accounting is done on IO completion, but that can result
 * in more than a second's worth of IO being accounted for within any one
 * second, leading to >100% utilisation.  To deal with that, we call this
 * function to do a round-off before returning the results when reading
 * /proc/diskstats.  This accounts immediately for all queue usage up to
 * the current jiffies and restarts the counters again.
 */
void disk_round_stats(struct gendisk *disk)
{
	unsigned long now = jiffies;

	disk_stat_add(disk, time_in_queue, 
			disk->in_flight * (now - disk->stamp));
	disk->stamp = now;

	if (disk->in_flight)
		disk_stat_add(disk, io_ticks, (now - disk->stamp_idle));
	disk->stamp_idle = now;
}

/*
 * queue lock must be held
 */
void __blk_put_request(request_queue_t *q, struct request *req)
{
	struct request_list *rl = req->rl;

	if (unlikely(!q))
		return;
	if (unlikely(--req->ref_count))
		return;

	elv_completed_request(req->q, req);

	req->rq_status = RQ_INACTIVE;
	req->q = NULL;
	req->rl = NULL;

	/*
	 * Request may not have originated from ll_rw_blk. if not,
	 * it didn't come out of our reserved rq pools
	 */
	if (rl) {
		int rw = rq_data_dir(req);

		BUG_ON(!list_empty(&req->queuelist));

		blk_free_request(q, req);
		freed_request(q, rw);
	}
}

void blk_put_request(struct request *req)
{
	request_queue_t *q = req->q;
	
	/*
	 * if req->q isn't set, this request didnt originate from the
	 * block layer, so it's safe to just disregard it
	 */
	if (q) {
		unsigned long flags;

		spin_lock_irqsave(q->queue_lock, flags);
		__blk_put_request(q, req);
		spin_unlock_irqrestore(q->queue_lock, flags);
	}
}

EXPORT_SYMBOL(blk_put_request);

/**
 * blk_congestion_wait - wait for a queue to become uncongested
 * @rw: READ or WRITE
 * @timeout: timeout in jiffies
 *
 * Waits for up to @timeout jiffies for a queue (any queue) to exit congestion.
 * If no queues are congested then just wait for the next request to be
 * returned.
 */
void blk_congestion_wait(int rw, long timeout)
{
	DEFINE_WAIT(wait);
	wait_queue_head_t *wqh = &congestion_wqh[rw];

	blk_run_queues();
	prepare_to_wait(wqh, &wait, TASK_UNINTERRUPTIBLE);
	io_schedule_timeout(timeout);
	finish_wait(wqh, &wait);
}

EXPORT_SYMBOL(blk_congestion_wait);

/*
 * Has to be called with the request spinlock acquired
 */
static int attempt_merge(request_queue_t *q, struct request *req,
			  struct request *next)
{
	if (!rq_mergeable(req) || !rq_mergeable(next))
		return 0;

	/*
	 * not contigious
	 */
	if (req->sector + req->nr_sectors != next->sector)
		return 0;

	if (rq_data_dir(req) != rq_data_dir(next)
	    || req->rq_disk != next->rq_disk
	    || next->waiting || next->special)
		return 0;

	/*
	 * If we are allowed to merge, then append bio list
	 * from next to rq and release next. merge_requests_fn
	 * will have updated segment counts, update sector
	 * counts here.
	 */
	if (!q->merge_requests_fn(q, req, next))
		return 0;

	req->biotail->bi_next = next->bio;
	req->biotail = next->biotail;

	req->nr_sectors = req->hard_nr_sectors += next->hard_nr_sectors;

	elv_merge_requests(q, req, next);

	if (req->rq_disk) {
		disk_round_stats(req->rq_disk);
		req->rq_disk->in_flight--;
	}

	__blk_put_request(q, next);
	return 1;
}

static inline int attempt_back_merge(request_queue_t *q, struct request *rq)
{
	struct request *next = elv_latter_request(q, rq);

	if (next)
		return attempt_merge(q, rq, next);

	return 0;
}

static inline int attempt_front_merge(request_queue_t *q, struct request *rq)
{
	struct request *prev = elv_former_request(q, rq);

	if (prev)
		return attempt_merge(q, prev, rq);

	return 0;
}

/**
 * blk_attempt_remerge  - attempt to remerge active head with next request
 * @q:    The &request_queue_t belonging to the device
 * @rq:   The head request (usually)
 *
 * Description:
 *    For head-active devices, the queue can easily be unplugged so quickly
 *    that proper merging is not done on the front request. This may hurt
 *    performance greatly for some devices. The block layer cannot safely
 *    do merging on that first request for these queues, but the driver can
 *    call this function and make it happen any way. Only the driver knows
 *    when it is safe to do so.
 **/
void blk_attempt_remerge(request_queue_t *q, struct request *rq)
{
	unsigned long flags;

	spin_lock_irqsave(q->queue_lock, flags);
	attempt_back_merge(q, rq);
	spin_unlock_irqrestore(q->queue_lock, flags);
}

EXPORT_SYMBOL(blk_attempt_remerge);

/*
 * Non-locking blk_attempt_remerge variant.
 */
void __blk_attempt_remerge(request_queue_t *q, struct request *rq)
{
	attempt_back_merge(q, rq);
}

EXPORT_SYMBOL(__blk_attempt_remerge);


// make_request 硬盘的处理函数实现体。
// 这里是创建request的地方，而bio是他需要的参数。
static int __make_request(request_queue_t *q, struct bio *bio)
{
	struct request *req, *freereq = NULL;
	int el_ret, rw, nr_sectors, cur_nr_sectors, barrier, ra;
	sector_t sector;

	sector = bio->bi_sector;
	nr_sectors = bio_sectors(bio);
	cur_nr_sectors = bio_cur_sectors(bio);

	rw = bio_data_dir(bio);

	/*
	 * low level driver can indicate that it wants pages above a
	 * certain limit bounced to low memory (ie for highmem, or even
	 * ISA dma in theory)
	 */
	blk_queue_bounce(q, &bio);

	spin_lock_prefetch(q->queue_lock);

	barrier = test_bit(BIO_RW_BARRIER, &bio->bi_rw);

	ra = bio->bi_rw & (1 << BIO_RW_AHEAD);

again:
	spin_lock_irq(q->queue_lock);

	if (elv_queue_empty(q)) {
		blk_plug_device(q);
		goto get_rq;
	}
	if (barrier)
		goto get_rq;

	el_ret = elv_merge(q, &req, bio);
	
	switch (el_ret) {
		case ELEVATOR_BACK_MERGE:
			BUG_ON(!rq_mergeable(req));

			if (!q->back_merge_fn(q, req, bio))
				break;

			req->biotail->bi_next = bio;
			req->biotail = bio;
			req->nr_sectors = req->hard_nr_sectors += nr_sectors;
			drive_stat_acct(req, nr_sectors, 0);
			if (!attempt_back_merge(q, req))
				elv_merged_request(q, req);
			goto out;

		case ELEVATOR_FRONT_MERGE:
			BUG_ON(!rq_mergeable(req));

			if (!q->front_merge_fn(q, req, bio))
				break;

			bio->bi_next = req->bio;
			req->cbio = req->bio = bio;
			req->nr_cbio_segments = bio_segments(bio);
			req->nr_cbio_sectors = bio_sectors(bio);

			/*
			 * may not be valid. if the low level driver said
			 * it didn't need a bounce buffer then it better
			 * not touch req->buffer either...
			 */
			req->buffer = bio_data(bio);
			req->current_nr_sectors = cur_nr_sectors;
			req->hard_cur_sectors = cur_nr_sectors;
			req->sector = req->hard_sector = sector;
			req->nr_sectors = req->hard_nr_sectors += nr_sectors;
			drive_stat_acct(req, nr_sectors, 0);
			if (!attempt_front_merge(q, req))
				elv_merged_request(q, req);
			goto out;

		/*
		 * elevator says don't/can't merge. get new request
		 */
		case ELEVATOR_NO_MERGE:
			break;

		default:
			printk("elevator returned crap (%d)\n", el_ret);
			BUG();
	}

	/*
	 * Grab a free request from the freelist - if that is empty, check
	 * if we are doing read ahead and abort instead of blocking for
	 * a free slot.
	 */
get_rq:
	if (freereq) {
		req = freereq;
		freereq = NULL;
	} else {
		spin_unlock_irq(q->queue_lock);
		if ((freereq = get_request(q, rw, GFP_ATOMIC)) == NULL) {
			/*
			 * READA bit set
			 */
			if (ra)
				goto end_io;
	
			freereq = get_request_wait(q, rw);
		}
		goto again;
	}

	/*
	 * first three bits are identical in rq->flags and bio->bi_rw,
	 * see bio.h and blkdev.h
	 */
	req->flags = (bio->bi_rw & 7) | REQ_CMD;

	/*
	 * REQ_BARRIER implies no merging, but lets make it explicit
	 */
	if (barrier)
		req->flags |= (REQ_HARDBARRIER | REQ_NOMERGE);

	/*
	 * don't stack up retries for read ahead
	 */
	if (ra)
		req->flags |= REQ_FAILFAST;

	req->errors = 0;
	req->hard_sector = req->sector = sector;
	req->hard_nr_sectors = req->nr_sectors = nr_sectors;
	req->current_nr_sectors = req->hard_cur_sectors = cur_nr_sectors;
	req->nr_phys_segments = bio_phys_segments(q, bio);
	req->nr_hw_segments = bio_hw_segments(q, bio);
	req->nr_cbio_segments = bio_segments(bio);
	req->nr_cbio_sectors = bio_sectors(bio);
	req->buffer = bio_data(bio);	/* see ->buffer comment above */
	req->waiting = NULL;
	req->cbio = req->bio = req->biotail = bio;
	req->rq_disk = bio->bi_bdev->bd_disk;
	req->start_time = jiffies;

	// 依旧熟悉的add_request。
	// 这里是吧当前请求放入到对应的队列中。
	add_request(q, req);
out:
	if (freereq)
		__blk_put_request(q, freereq);

	// 盲猜，对于这个判断，就是用来判断是不是第一个倒霉蛋，不是的话就可以返回了，因为上面add_request已经放入到队列中了。
	// bingo 猜对了
	if (blk_queue_plugged(q)) {
		// 下面的操作是倒霉蛋完成的。
	
		int nr_queued = q->rq.count[READ] + q->rq.count[WRITE];
		
		if (nr_queued == q->unplug_thresh)

			// 这里是调用do_hd_request来处理请求了。
			__generic_unplug_device(q);
	}

	// 不是倒霉蛋就直接退出了。
	spin_unlock_irq(q->queue_lock);
	return 0;

end_io:
	bio_endio(bio, nr_sectors << 9, -EWOULDBLOCK);
	return 0;
}

/*
 * If bio->bi_dev is a partition, remap the location
 */
static inline void blk_partition_remap(struct bio *bio)
{
	struct block_device *bdev = bio->bi_bdev;

	if (bdev != bdev->bd_contains) {
		struct hd_struct *p = bdev->bd_part;

		switch (bio->bi_rw) {
		case READ:
			p->read_sectors += bio_sectors(bio);
			p->reads++;
			break;
		case WRITE:
			p->write_sectors += bio_sectors(bio);
			p->writes++;
			break;
		}
		bio->bi_sector += p->start_sect;
		bio->bi_bdev = bdev->bd_contains;
	}
}

/**
 * generic_make_request: hand a buffer to its device driver for I/O
 * @bio:  The bio describing the location in memory and on the device.
 *
 * generic_make_request() is used to make I/O requests of block
 * devices. It is passed a &struct bio, which describes the I/O that needs
 * to be done.
 *
 * generic_make_request() does not return any status.  The
 * success/failure status of the request, along with notification of
 * completion, is delivered asynchronously through the bio->bi_end_io
 * function described (one day) else where.
 *
 * The caller of generic_make_request must make sure that bi_io_vec
 * are set to describe the memory buffer, and that bi_dev and bi_sector are
 * set to describe the device address, and the
 * bi_end_io and optionally bi_private are set to describe how
 * completion notification should be signaled.
 *
 * generic_make_request and the drivers it calls may use bi_next if this
 * bio happens to be merged with someone else, and may change bi_dev and
 * bi_sector for remaps as it sees fit.  So the values of these fields
 * should NOT be depended on after the call to generic_make_request.
 */
void generic_make_request(struct bio *bio)
{
	request_queue_t *q;
	sector_t maxsector;
	int ret, nr_sectors = bio_sectors(bio);

	/* Test device or partition size, when known. */
	maxsector = bio->bi_bdev->bd_inode->i_size >> 9;
	if (maxsector) {
		sector_t sector = bio->bi_sector;

		if (maxsector < nr_sectors ||
		    maxsector - nr_sectors < sector) {
			char b[BDEVNAME_SIZE];
			/* This may well happen - the kernel calls
			 * bread() without checking the size of the
			 * device, e.g., when mounting a device. */
			printk(KERN_INFO
			       "attempt to access beyond end of device\n");
			printk(KERN_INFO "%s: rw=%ld, want=%Lu, limit=%Lu\n",
			       bdevname(bio->bi_bdev, b),
			       bio->bi_rw,
			       (unsigned long long) sector + nr_sectors,
			       (long long) maxsector);

			set_bit(BIO_EOF, &bio->bi_flags);
			goto end_io;
		}
	}

	/*
	 * Resolve the mapping until finished. (drivers are
	 * still free to implement/resolve their own stacking
	 * by explicitly returning 0)
	 *
	 * NOTE: we don't repeat the blk_size check for each new device.
	 * Stacking drivers are expected to know what they are doing.
	 */
	do {
		char b[BDEVNAME_SIZE];

		q = bdev_get_queue(bio->bi_bdev);
		if (!q) {
			printk(KERN_ERR
			       "generic_make_request: Trying to access "
				"nonexistent block-device %s (%Lu)\n",
				bdevname(bio->bi_bdev, b),
				(long long) bio->bi_sector);
end_io:
			bio_endio(bio, bio->bi_size, -EIO);
			break;
		}

		if (unlikely(bio_sectors(bio) > q->max_sectors)) {
			printk("bio too big device %s (%u > %u)\n", 
				bdevname(bio->bi_bdev, b),
				bio_sectors(bio),
				q->max_sectors);
			goto end_io;
		}

		if (test_bit(QUEUE_FLAG_DEAD, &q->queue_flags))
			goto end_io;

		/*
		 * If this device has partitions, remap block n
		 * of partition p to block n+start(p) of the disk.
		 */
		blk_partition_remap(bio);

		// 任何关于队列的操作，肯定都是request_queue来定义接口，文件系统来实现。
		ret = q->make_request_fn(q, bio);
	} while (ret);
}

EXPORT_SYMBOL(generic_make_request);

/**
 * submit_bio: submit a bio to the block device layer for I/O
 * @rw: whether to %READ or %WRITE, or maybe to %READA (read ahead)
 * @bio: The &struct bio which describes the I/O
 *
 * submit_bio() is very similar in purpose to generic_make_request(), and
 * uses that function to do most of the work. Both are fairly rough
 * interfaces, @bio must be presetup and ready for I/O.
 *
 */
int submit_bio(int rw, struct bio *bio)
{
	int count = bio_sectors(bio);

	BIO_BUG_ON(!bio->bi_size);
	BIO_BUG_ON(!bio->bi_io_vec);
	bio->bi_rw = rw;
	if (rw & WRITE)
		mod_page_state(pgpgout, count);
	else
		mod_page_state(pgpgin, count);

	// make_request  dddd.
	// 封装request请求对象。然后add_request进队列。
	generic_make_request(bio);
	return 1;
}

EXPORT_SYMBOL(submit_bio);

/**
 * blk_rq_next_segment
 * @rq:		the request being processed
 *
 * Description:
 *	Points to the next segment in the request if the current segment
 *	is complete. Leaves things unchanged if this segment is not over
 *	or if no more segments are left in this request.
 *
 *	Meant to be used for bio traversal during I/O submission
 *	Does not affect any I/O completions or update completion state
 *	in the request, and does not modify any bio fields.
 *
 *	Decrementing rq->nr_sectors, rq->current_nr_sectors and
 *	rq->nr_cbio_sectors as data is transferred is the caller's
 *	responsibility and should be done before calling this routine.
 **/
void blk_rq_next_segment(struct request *rq)
{
	if (rq->current_nr_sectors > 0)
		return;

	if (rq->nr_cbio_sectors > 0) {
		--rq->nr_cbio_segments;
		rq->current_nr_sectors = blk_rq_vec(rq)->bv_len >> 9;
	} else {
		if ((rq->cbio = rq->cbio->bi_next)) {
			rq->nr_cbio_segments = bio_segments(rq->cbio);
			rq->nr_cbio_sectors = bio_sectors(rq->cbio);
 			rq->current_nr_sectors = bio_cur_sectors(rq->cbio);
		}
 	}

	/* remember the size of this segment before we start I/O */
	rq->hard_cur_sectors = rq->current_nr_sectors;
}

/**
 * process_that_request_first	-	process partial request submission
 * @req:	the request being processed
 * @nr_sectors:	number of sectors I/O has been submitted on
 *
 * Description:
 *	May be used for processing bio's while submitting I/O without
 *	signalling completion. Fails if more data is requested than is
 *	available in the request in which case it doesn't advance any
 *	pointers.
 *
 *	Assumes a request is correctly set up. No sanity checks.
 *
 * Return:
 *	0 - no more data left to submit (not processed)
 *	1 - data available to submit for this request (processed)
 **/
int process_that_request_first(struct request *req, unsigned int nr_sectors)
{
	unsigned int nsect;

	if (req->nr_sectors < nr_sectors)
		return 0;

	req->nr_sectors -= nr_sectors;
	req->sector += nr_sectors;
	while (nr_sectors) {
		nsect = min_t(unsigned, req->current_nr_sectors, nr_sectors);
		req->current_nr_sectors -= nsect;
		nr_sectors -= nsect;
		if (req->cbio) {
			req->nr_cbio_sectors -= nsect;
			blk_rq_next_segment(req);
		}
	}
	return 1;
}

EXPORT_SYMBOL(process_that_request_first);

void blk_recalc_rq_segments(struct request *rq)
{
	struct bio *bio;
	int nr_phys_segs, nr_hw_segs;

	if (!rq->bio)
		return;

	nr_phys_segs = nr_hw_segs = 0;
	rq_for_each_bio(bio, rq) {
		/* Force bio hw/phys segs to be recalculated. */
		bio->bi_flags &= ~(1 << BIO_SEG_VALID);

		nr_phys_segs += bio_phys_segments(rq->q, bio);
		nr_hw_segs += bio_hw_segments(rq->q, bio);
	}

	rq->nr_phys_segments = nr_phys_segs;
	rq->nr_hw_segments = nr_hw_segs;
}

void blk_recalc_rq_sectors(struct request *rq, int nsect)
{
	if (blk_fs_request(rq)) {
		rq->hard_sector += nsect;
		rq->hard_nr_sectors -= nsect;

		/*
		 * Move the I/O submission pointers ahead if required,
		 * i.e. for drivers not aware of rq->cbio.
		 */
		if ((rq->nr_sectors >= rq->hard_nr_sectors) &&
		    (rq->sector <= rq->hard_sector)) {
			rq->sector = rq->hard_sector;
			rq->nr_sectors = rq->hard_nr_sectors;
			rq->hard_cur_sectors = bio_cur_sectors(rq->bio);
			rq->current_nr_sectors = rq->hard_cur_sectors;
			rq->nr_cbio_segments = bio_segments(rq->bio);
			rq->nr_cbio_sectors = bio_sectors(rq->bio);
			rq->buffer = bio_data(rq->bio);

			rq->cbio = rq->bio;
		}

		/*
		 * if total number of sectors is less than the first segment
		 * size, something has gone terribly wrong
		 */
		if (rq->nr_sectors < rq->current_nr_sectors) {
			printk("blk: request botched\n");
			rq->nr_sectors = rq->current_nr_sectors;
		}
	}
}

static int __end_that_request_first(struct request *req, int uptodate,
				    int nr_bytes)
{
	int total_bytes, bio_nbytes, error = 0, next_idx = 0;
	struct bio *bio;

	/*
	 * for a REQ_BLOCK_PC request, we want to carry any eventual
	 * sense key with us all the way through
	 */
	if (!blk_pc_request(req))
		req->errors = 0;

	if (!uptodate) {
		error = -EIO;
		if (!(req->flags & REQ_QUIET))
			printk("end_request: I/O error, dev %s, sector %llu\n",
				req->rq_disk ? req->rq_disk->disk_name : "?",
				(unsigned long long)req->sector);
	}

	total_bytes = bio_nbytes = 0;
	while ((bio = req->bio)) {
		int nbytes;

		if (nr_bytes >= bio->bi_size) {
			req->bio = bio->bi_next;
			nbytes = bio->bi_size;
			bio_endio(bio, nbytes, error);
			next_idx = 0;
			bio_nbytes = 0;
		} else {
			int idx = bio->bi_idx + next_idx;

			if (unlikely(bio->bi_idx >= bio->bi_vcnt)) {
				blk_dump_rq_flags(req, "__end_that");
				printk("%s: bio idx %d >= vcnt %d\n",
						__FUNCTION__,
						bio->bi_idx, bio->bi_vcnt);
				break;
			}

			nbytes = bio_iovec_idx(bio, idx)->bv_len;
			BIO_BUG_ON(nbytes > bio->bi_size);

			/*
			 * not a complete bvec done
			 */
			if (unlikely(nbytes > nr_bytes)) {
				bio_iovec_idx(bio, idx)->bv_offset += nr_bytes;
				bio_iovec_idx(bio, idx)->bv_len -= nr_bytes;
				bio_nbytes += nr_bytes;
				total_bytes += nr_bytes;
				break;
			}

			/*
			 * advance to the next vector
			 */
			next_idx++;
			bio_nbytes += nbytes;
		}

		total_bytes += nbytes;
		nr_bytes -= nbytes;

		if ((bio = req->bio)) {
			/*
			 * end more in this run, or just return 'not-done'
			 */
			if (unlikely(nr_bytes <= 0))
				break;
		}
	}

	/*
	 * completely done
	 */
	if (!req->bio)
		return 0;

	/*
	 * if the request wasn't completed, update state
	 */
	if (bio_nbytes) {
		bio_endio(bio, bio_nbytes, error);
		req->bio->bi_idx += next_idx;
	}

	blk_recalc_rq_sectors(req, total_bytes >> 9);
	blk_recalc_rq_segments(req);
	return 1;
}

/**
 * end_that_request_first - end I/O on a request
 * @req:      the request being processed
 * @uptodate: 0 for I/O error
 * @nr_sectors: number of sectors to end I/O on
 *
 * Description:
 *     Ends I/O on a number of sectors attached to @req, and sets it up
 *     for the next range of segments (if any) in the cluster.
 *
 * Return:
 *     0 - we are done with this request, call end_that_request_last()
 *     1 - still buffers pending for this request
 **/
int end_that_request_first(struct request *req, int uptodate, int nr_sectors)
{
	return __end_that_request_first(req, uptodate, nr_sectors << 9);
}

EXPORT_SYMBOL(end_that_request_first);

/**
 * end_that_request_chunk - end I/O on a request
 * @req:      the request being processed
 * @uptodate: 0 for I/O error
 * @nr_bytes: number of bytes to complete
 *
 * Description:
 *     Ends I/O on a number of bytes attached to @req, and sets it up
 *     for the next range of segments (if any). Like end_that_request_first(),
 *     but deals with bytes instead of sectors.
 *
 * Return:
 *     0 - we are done with this request, call end_that_request_last()
 *     1 - still buffers pending for this request
 **/
int end_that_request_chunk(struct request *req, int uptodate, int nr_bytes)
{
	return __end_that_request_first(req, uptodate, nr_bytes);
}

EXPORT_SYMBOL(end_that_request_chunk);

/*
 * queue lock must be held
 */
void end_that_request_last(struct request *req)
{
	struct gendisk *disk = req->rq_disk;
	struct completion *waiting = req->waiting;

	if (disk && blk_fs_request(req)) {
		unsigned long duration = jiffies - req->start_time;
		switch (rq_data_dir(req)) {
		    case WRITE:
			disk_stat_inc(disk, writes);
			disk_stat_add(disk, write_ticks, duration);
			break;
		    case READ:
			disk_stat_inc(disk, reads);
			disk_stat_add(disk, read_ticks, duration);
			break;
		}
		disk_round_stats(disk);
		disk->in_flight--;
	}
	__blk_put_request(req->q, req);
	/* Do this LAST! The structure may be freed immediately afterwards */
	if (waiting)
		complete(waiting);
}

EXPORT_SYMBOL(end_that_request_last);

void end_request(struct request *req, int uptodate)
{
	if (!end_that_request_first(req, uptodate, req->hard_cur_sectors)) {
		add_disk_randomness(req->rq_disk);
		blkdev_dequeue_request(req);
		end_that_request_last(req);
	}
}

EXPORT_SYMBOL(end_request);

void blk_rq_bio_prep(request_queue_t *q, struct request *rq, struct bio *bio)
{
	/* first three bits are identical in rq->flags and bio->bi_rw */
	rq->flags |= (bio->bi_rw & 7);

	rq->nr_phys_segments = bio_phys_segments(q, bio);
	rq->nr_hw_segments = bio_hw_segments(q, bio);
	rq->current_nr_sectors = bio_cur_sectors(bio);
	rq->hard_cur_sectors = rq->current_nr_sectors;
	rq->hard_nr_sectors = rq->nr_sectors = bio_sectors(bio);
	rq->nr_cbio_segments = bio_segments(bio);
	rq->nr_cbio_sectors = bio_sectors(bio);
	rq->buffer = bio_data(bio);

	rq->cbio = rq->bio = rq->biotail = bio;
}

EXPORT_SYMBOL(blk_rq_bio_prep);

void blk_rq_prep_restart(struct request *rq)
{
	struct bio *bio;

	bio = rq->cbio = rq->bio;
	if (bio) {
		rq->nr_cbio_segments = bio_segments(bio);
		rq->nr_cbio_sectors = bio_sectors(bio);
		rq->hard_cur_sectors = bio_cur_sectors(bio);
		rq->buffer = bio_data(bio);
	}
	rq->sector = rq->hard_sector;
	rq->nr_sectors = rq->hard_nr_sectors;
	rq->current_nr_sectors = rq->hard_cur_sectors;
}

EXPORT_SYMBOL(blk_rq_prep_restart);

int kblockd_schedule_work(struct work_struct *work)
{
	return queue_work(kblockd_workqueue, work);
}

void kblockd_flush(void)
{
	flush_workqueue(kblockd_workqueue);
}

int __init blk_dev_init(void)
{
	int i;

	kblockd_workqueue = create_workqueue("kblockd");
	if (!kblockd_workqueue)
		panic("Failed to create kblockd\n");

	request_cachep = kmem_cache_create("blkdev_requests",
			sizeof(struct request), 0, 0, NULL, NULL);
	if (!request_cachep)
		panic("Can't create request pool slab cache\n");

	blk_max_low_pfn = max_low_pfn;
	blk_max_pfn = max_pfn;

	for (i = 0; i < ARRAY_SIZE(congestion_wqh); i++)
		init_waitqueue_head(&congestion_wqh[i]);
	return 0;
}

/*
 * IO Context helper functions
 */
void put_io_context(struct io_context *ioc)
{
	if (ioc == NULL)
		return;

	BUG_ON(atomic_read(&ioc->refcount) == 0);

	if (atomic_dec_and_test(&ioc->refcount)) {
		if (ioc->aic && ioc->aic->dtor)
			ioc->aic->dtor(ioc->aic);
		kfree(ioc);
	}
}

/* Called by the exitting task */
void exit_io_context(void)
{
	unsigned long flags;
	struct io_context *ioc;

	local_irq_save(flags);
	ioc = current->io_context;
	if (ioc) {
		if (ioc->aic && ioc->aic->exit)
			ioc->aic->exit(ioc->aic);
		put_io_context(ioc);
		current->io_context = NULL;
	} else
		WARN_ON(1);
	local_irq_restore(flags);
}

/*
 * If the current task has no IO context then create one and initialise it.
 * If it does have a context, take a ref on it.
 *
 * This is always called in the context of the task which submitted the I/O.
 * But weird things happen, so we disable local interrupts to ensure exclusive
 * access to *current.
 */
struct io_context *get_io_context(int gfp_flags)
{
	struct task_struct *tsk = current;
	unsigned long flags;
	struct io_context *ret;

	local_irq_save(flags);
	ret = tsk->io_context;
	if (ret == NULL) {
		ret = kmalloc(sizeof(*ret), GFP_ATOMIC);
		if (ret) {
			atomic_set(&ret->refcount, 1);
			ret->pid = tsk->pid;
			ret->last_waited = jiffies; /* doesn't matter... */
			ret->nr_batch_requests = 0; /* because this is 0 */
			ret->aic = NULL;
			tsk->io_context = ret;
		}
	}
	if (ret)
		atomic_inc(&ret->refcount);
	local_irq_restore(flags);
	return ret;
}

void copy_io_context(struct io_context **pdst, struct io_context **psrc)
{
	struct io_context *src = *psrc;
	struct io_context *dst = *pdst;

	if (src) {
		BUG_ON(atomic_read(&src->refcount) == 0);
		atomic_inc(&src->refcount);
		put_io_context(dst);
		*pdst = src;
	}
}

void swap_io_context(struct io_context **ioc1, struct io_context **ioc2)
{
	struct io_context *temp;
	temp = *ioc1;
	*ioc1 = *ioc2;
	*ioc2 = temp;
}


/*
 * sysfs parts below
 */
struct queue_sysfs_entry {
	struct attribute attr;
	ssize_t (*show)(struct request_queue *, char *);
	ssize_t (*store)(struct request_queue *, const char *, size_t);
};

static ssize_t
queue_var_show(unsigned int var, char *page)
{
	return sprintf(page, "%d\n", var);
}

static ssize_t
queue_var_store(unsigned long *var, const char *page, size_t count)
{
	char *p = (char *) page;

	*var = simple_strtoul(p, &p, 10);
	return count;
}

static ssize_t queue_requests_show(struct request_queue *q, char *page)
{
	return queue_var_show(q->nr_requests, (page));
}

static ssize_t
queue_requests_store(struct request_queue *q, const char *page, size_t count)
{
	struct request_list *rl = &q->rq;

	int ret = queue_var_store(&q->nr_requests, page, count);
	if (q->nr_requests < BLKDEV_MIN_RQ)
		q->nr_requests = BLKDEV_MIN_RQ;

	if (rl->count[READ] >= queue_congestion_on_threshold(q))
		set_queue_congested(q, READ);
	else if (rl->count[READ] < queue_congestion_off_threshold(q))
		clear_queue_congested(q, READ);

	if (rl->count[WRITE] >= queue_congestion_on_threshold(q))
		set_queue_congested(q, WRITE);
	else if (rl->count[WRITE] < queue_congestion_off_threshold(q))
		clear_queue_congested(q, WRITE);

	if (rl->count[READ] >= q->nr_requests) {
		blk_set_queue_full(q, READ);
	} else if (rl->count[READ]+1 <= q->nr_requests) {
		blk_clear_queue_full(q, READ);
		wake_up(&rl->wait[READ]);
	}

	if (rl->count[WRITE] >= q->nr_requests) {
		blk_set_queue_full(q, WRITE);
	} else if (rl->count[WRITE]+1 <= q->nr_requests) {
		blk_clear_queue_full(q, WRITE);
		wake_up(&rl->wait[WRITE]);
	}
	return ret;
}

static struct queue_sysfs_entry queue_requests_entry = {
	.attr = {.name = "nr_requests", .mode = S_IRUGO | S_IWUSR },
	.show = queue_requests_show,
	.store = queue_requests_store,
};

static struct attribute *default_attrs[] = {
	&queue_requests_entry.attr,
	NULL,
};

#define to_queue(atr) container_of((atr), struct queue_sysfs_entry, attr)

static ssize_t
queue_attr_show(struct kobject *kobj, struct attribute *attr, char *page)
{
	struct queue_sysfs_entry *entry = to_queue(attr);
	struct request_queue *q;

	q = container_of(kobj, struct request_queue, kobj);
	if (!entry->show)
		return 0;

	return entry->show(q, page);
}

static ssize_t
queue_attr_store(struct kobject *kobj, struct attribute *attr,
		    const char *page, size_t length)
{
	struct queue_sysfs_entry *entry = to_queue(attr);
	struct request_queue *q;

	q = container_of(kobj, struct request_queue, kobj);
	if (!entry->store)
		return -EINVAL;

	return entry->store(q, page, length);
}

static struct sysfs_ops queue_sysfs_ops = {
	.show	= queue_attr_show,
	.store	= queue_attr_store,
};

struct kobj_type queue_ktype = {
	.sysfs_ops	= &queue_sysfs_ops,
	.default_attrs	= default_attrs,
};

int blk_register_queue(struct gendisk *disk)
{
	int ret;

	request_queue_t *q = disk->queue;

	if (!q)
		return -ENXIO;

	q->kobj.parent = kobject_get(&disk->kobj);
	if (!q->kobj.parent)
		return -EBUSY;

	snprintf(q->kobj.name, KOBJ_NAME_LEN, "%s", "queue");
	q->kobj.ktype = &queue_ktype;

	ret = kobject_register(&q->kobj);
	if (ret < 0)
		return ret;

	ret = elv_register_queue(q);
	if (ret) {
		kobject_unregister(&q->kobj);
		return ret;
	}

	return 0;
}

void blk_unregister_queue(struct gendisk *disk)
{
	request_queue_t *q = disk->queue;

	if (q) {
		elv_unregister_queue(q);

		kobject_unregister(&q->kobj);
		kobject_put(&disk->kobj);
	}
}
