#ifndef __LINUX_PREEMPT_H
#define __LINUX_PREEMPT_H

/*
 * include/linux/preempt.h - macros for accessing and manipulating
 * preempt_count (used for kernel preemption, interrupt count, etc.)
 */

#include <linux/config.h>

#define preempt_count()	(current_thread_info()->preempt_count)

#define inc_preempt_count() \
do { \
	preempt_count()++; \
} while (0)

#define dec_preempt_count() \
do { \
	preempt_count()--; \
} while (0)

#ifdef CONFIG_PREEMPT

extern void preempt_schedule(void);

// inc_preempt_count停止抢占
// 会指示编译器避免某些内存优化，以免导致某些与抢占机制相关的问题。
#define preempt_disable() \
do { \
	inc_preempt_count(); \		
	barrier(); \
} while (0)

#define preempt_enable_no_resched() \
do { \
	barrier(); \
	dec_preempt_count(); \
} while (0)

#define preempt_check_resched() \
do { \
	if (unlikely(test_thread_flag(TIF_NEED_RESCHED))) \
		preempt_schedule(); \
} while (0)

#define preempt_enable() \
do { \
	preempt_enable_no_resched(); \
	preempt_check_resched(); \
} while (0)

#else

#define preempt_disable()		do { } while (0)
#define preempt_enable_no_resched()	do { } while (0)
#define preempt_enable()		do { } while (0)
#define preempt_check_resched()		do { } while (0)

#endif

#endif /* __LINUX_PREEMPT_H */
