#ifndef __LINUX_PREEMPT_H
#define __LINUX_PREEMPT_H

/*
 * include/linux/preempt.h - macros for accessing and manipulating
 * preempt_count (used for kernel preemption, interrupt count, etc.)
 */

#include <linux/thread_info.h>
#include <linux/linkage.h>
#include <linux/list.h>

#if defined(CONFIG_DEBUG_PREEMPT) || defined(CONFIG_PREEMPT_TRACER)
  extern void add_preempt_count(int val);
  extern void sub_preempt_count(int val);
#else
# define add_preempt_count(val)	do { preempt_count() += (val); } while (0)
# define sub_preempt_count(val)	do { preempt_count() -= (val); } while (0)
#endif

/*
 * 只有PREEMPT为0时才允许内核态抢占
 * inc_preempt_count增加PREEMPT，从而禁止内核态抢占
 */
#define inc_preempt_count() add_preempt_count(1)
#define dec_preempt_count() sub_preempt_count(1)

#define preempt_count()	(current_thread_info()->preempt_count)

#ifdef CONFIG_PREEMPT

asmlinkage void preempt_schedule(void);

/*
 * http://blog.csdn.net/joker0910/article/details/7782765
 *
 * 如果是可抢占的，内核中的进程在任何时刻都可能停下来以便另一个更高优先级的进程运行。
 * 这意味着一个任务与被抢占的任务可能会在同一个临界区内运行。
 * 为了避免这个情况，内核抢占代码使用自旋锁作为非抢占区域的标记。
 * 如果一个自旋锁被持有，内核便不能进行抢占。
 *
 * 该函数用来禁止抢占
 * 如何理解这个函数的实现？
 *
 * 内存屏障
 * 内存屏障的出现是因为编译器或现在的处理器会“自作聪明”地对指令序列进行一些处理，比如数据缓存，读写指令乱序等等。
 * 如果优化对象是普通内存，那么一般会提升性能而且不会产生逻辑错误。
 * 但是，如果对IO操作进行类似优化可能会造成致命错误。
 * 所以要使用内存屏障，以强制该语句前后的指令以正确的次序完成。
 *
 * 其实在指令序列中放一个wmb的效果是使得指令执行到该处时，
 * 把所有缓存的数据写到该写的地方，同时使得wmb前面的指令的写指令一定会在wmb的写指令之前执行。
 * rmb（读内存屏障）保证了屏障之前的读操作一定会在后来的写操作之前完成。
 */
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
	barrier(); \
	preempt_check_resched(); \
} while (0)

/* For debugging and tracer internals only! */
#define add_preempt_count_notrace(val)			\
	do { preempt_count() += (val); } while (0)
#define sub_preempt_count_notrace(val)			\
	do { preempt_count() -= (val); } while (0)
#define inc_preempt_count_notrace() add_preempt_count_notrace(1)
#define dec_preempt_count_notrace() sub_preempt_count_notrace(1)

#define preempt_disable_notrace() \
do { \
	inc_preempt_count_notrace(); \
	barrier(); \
} while (0)

#define preempt_enable_no_resched_notrace() \
do { \
	barrier(); \
	dec_preempt_count_notrace(); \
} while (0)

/* preempt_check_resched is OK to trace */
#define preempt_enable_notrace() \
do { \
	preempt_enable_no_resched_notrace(); \
	barrier(); \
	preempt_check_resched(); \
} while (0)

#else

#define preempt_disable()		do { } while (0)
#define preempt_enable_no_resched()	do { } while (0)
#define preempt_enable()		do { } while (0)
#define preempt_check_resched()		do { } while (0)

#define preempt_disable_notrace()		do { } while (0)
#define preempt_enable_no_resched_notrace()	do { } while (0)
#define preempt_enable_notrace()		do { } while (0)

#endif

#ifdef CONFIG_PREEMPT_NOTIFIERS

struct preempt_notifier;

/**
 * preempt_ops - notifiers called when a task is preempted and rescheduled
 * @sched_in: we're about to be rescheduled:
 *    notifier: struct preempt_notifier for the task being scheduled
 *    cpu:  cpu we're scheduled on
 * @sched_out: we've just been preempted
 *    notifier: struct preempt_notifier for the task being preempted
 *    next: the task that's kicking us out
 */
struct preempt_ops {
	void (*sched_in)(struct preempt_notifier *notifier, int cpu);
	void (*sched_out)(struct preempt_notifier *notifier,
			  struct task_struct *next);
};

/**
 * preempt_notifier - key for installing preemption notifiers
 * @link: internal use
 * @ops: defines the notifier functions to be called
 *
 * Usually used in conjunction with container_of().
 */
struct preempt_notifier {
	struct hlist_node link;
	struct preempt_ops *ops;
};

void preempt_notifier_register(struct preempt_notifier *notifier);
void preempt_notifier_unregister(struct preempt_notifier *notifier);

static inline void preempt_notifier_init(struct preempt_notifier *notifier,
				     struct preempt_ops *ops)
{
	INIT_HLIST_NODE(&notifier->link);
	notifier->ops = ops;
}

#endif

#endif /* __LINUX_PREEMPT_H */
