/*
 * linux/kernel/irq/handle.c
 *
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the core interrupt handling code.
 *
 * Detailed information is available in Documentation/DocBook/genericirq
 *
 */

#include <linux/irq.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/random.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/rculist.h>
#include <linux/hash.h>
#include <linux/bootmem.h>
#include <trace/events/irq.h>

#include "internals.h"

/*
 * lockdep: we want to handle all irq_desc locks as a single lock-class:
 */
struct lock_class_key irq_desc_lock_class;

/**
 * handle_bad_irq - handle spurious and unhandled irqs
 * @irq:       the interrupt number
 * @desc:      description of the interrupt
 *
 * Handles spurious and unhandled IRQ's. It also prints a debugmessage.
 */
void handle_bad_irq(unsigned int irq, struct irq_desc *desc)
{
	print_irq_desc(irq, desc);
	kstat_incr_irqs_this_cpu(irq, desc);
	ack_bad_irq(irq);
}

#if defined(CONFIG_SMP) && defined(CONFIG_GENERIC_HARDIRQS)
static void __init init_irq_default_affinity(void)
{
	alloc_cpumask_var(&irq_default_affinity, GFP_NOWAIT);
	cpumask_setall(irq_default_affinity);
}
#else
static void __init init_irq_default_affinity(void)
{
}
#endif

/*
 * Linux has a controller-independent interrupt architecture.
 * Every controller has a 'controller-template', that is used
 * by the main code to do the right thing. Each driver-visible
 * interrupt source is transparently wired to the appropriate
 * controller. Thus drivers need not be aware of the
 * interrupt-controller.
 *
 * The code is designed to be easily extended with new/different
 * interrupt controllers, without having to do assembly magic or
 * having to touch the generic code.
 *
 * Controller mappings for all interrupt sources:
 */
int nr_irqs = NR_IRQS;
EXPORT_SYMBOL_GPL(nr_irqs);

#ifdef CONFIG_SPARSE_IRQ

static struct irq_desc irq_desc_init = {
	.irq	    = -1,
	.status	    = IRQ_DISABLED,
	.chip	    = &no_irq_chip,
	.handle_irq = handle_bad_irq,
	.depth      = 1,
	.lock       = __SPIN_LOCK_UNLOCKED(irq_desc_init.lock),
};

void __ref init_kstat_irqs(struct irq_desc *desc, int node, int nr)
{
	void *ptr;

	if (slab_is_available())
		ptr = kzalloc_node(nr * sizeof(*desc->kstat_irqs),
				   GFP_ATOMIC, node);
	else
		ptr = alloc_bootmem_node(NODE_DATA(node),
				nr * sizeof(*desc->kstat_irqs));

	/*
	 * don't overwite if can not get new one
	 * init_copy_kstat_irqs() could still use old one
	 */
	if (ptr) {
		printk(KERN_DEBUG "  alloc kstat_irqs on node %d\n", node);
		desc->kstat_irqs = ptr;
	}
}

static void init_one_irq_desc(int irq, struct irq_desc *desc, int node)
{
	memcpy(desc, &irq_desc_init, sizeof(struct irq_desc));

	spin_lock_init(&desc->lock);
	desc->irq = irq;
#ifdef CONFIG_SMP
	desc->node = node;
#endif
	lockdep_set_class(&desc->lock, &irq_desc_lock_class);
	init_kstat_irqs(desc, node, nr_cpu_ids);
	if (!desc->kstat_irqs) {
		printk(KERN_ERR "can not alloc kstat_irqs\n");
		BUG_ON(1);
	}
	if (!alloc_desc_masks(desc, node, false)) {
		printk(KERN_ERR "can not alloc irq_desc cpumasks\n");
		BUG_ON(1);
	}
	init_desc_masks(desc);
	arch_init_chip_data(desc, node);
}

/*
 * Protect the sparse_irqs:
 */
DEFINE_SPINLOCK(sparse_irq_lock);

// IRQ相关信息的管理的关键之处在于，内核引入了一个irq_desc类型的全局数组来记录它们。
// 每个数组的项对应一个IRQ编号，数组下标与中断号一一对应
struct irq_desc **irq_desc_ptrs __read_mostly;

static struct irq_desc irq_desc_legacy[NR_IRQS_LEGACY] __cacheline_aligned_in_smp = {
	[0 ... NR_IRQS_LEGACY-1] = {
		.irq	    = -1,
		.status	    = IRQ_DISABLED,
		.chip	    = &no_irq_chip,
		.handle_irq = handle_bad_irq,
		.depth	    = 1,
		.lock	    = __SPIN_LOCK_UNLOCKED(irq_desc_init.lock),
	}
};

static unsigned int *kstat_irqs_legacy;

int __init early_irq_init(void)
{
	struct irq_desc *desc;
	int legacy_count;
	int node;
	int i;

	init_irq_default_affinity();

	 /* initialize nr_irqs based on nr_cpu_ids */
	arch_probe_nr_irqs();
	printk(KERN_INFO "NR_IRQS:%d nr_irqs:%d\n", NR_IRQS, nr_irqs);

	desc = irq_desc_legacy;
	legacy_count = ARRAY_SIZE(irq_desc_legacy);
	node = first_online_node;

   // 为中断描述符分配空间
	/* allocate irq_desc_ptrs array based on nr_irqs */
	irq_desc_ptrs = kcalloc(nr_irqs, sizeof(void *), GFP_NOWAIT);

	/* allocate based on nr_cpu_ids */
	kstat_irqs_legacy = kzalloc_node(NR_IRQS_LEGACY * nr_cpu_ids *
					  sizeof(int), GFP_NOWAIT, node);

	for (i = 0; i < legacy_count; i++) {
      // 为每个irq_desc_ptrs的元素初始化。
      // 这里并没有初始化中断描述符的电流处理函数handle_irq成员。
      // 留到具体的控制器中去完成，如8259A make_8259A_irq()
		desc[i].irq = i;
#ifdef CONFIG_SMP
		desc[i].node = node;
#endif
		desc[i].kstat_irqs = kstat_irqs_legacy + i * nr_cpu_ids;
		lockdep_set_class(&desc[i].lock, &irq_desc_lock_class);
		alloc_desc_masks(&desc[i], node, true);
		init_desc_masks(&desc[i]);
		irq_desc_ptrs[i] = desc + i;
	}

	for (i = legacy_count; i < nr_irqs; i++)
		irq_desc_ptrs[i] = NULL;

	return arch_early_irq_init();
}

// 根据设备中断号取得相应的中断描述符
struct irq_desc *irq_to_desc(unsigned int irq)
{
	if (irq_desc_ptrs && irq < nr_irqs)
		return irq_desc_ptrs[irq];

	return NULL;
}

struct irq_desc * __ref irq_to_desc_alloc_node(unsigned int irq, int node)
{
	struct irq_desc *desc;
	unsigned long flags;

	if (irq >= nr_irqs) {
		WARN(1, "irq (%d) >= nr_irqs (%d) in irq_to_desc_alloc\n",
			irq, nr_irqs);
		return NULL;
	}

	desc = irq_desc_ptrs[irq];
	if (desc)
		return desc;

	spin_lock_irqsave(&sparse_irq_lock, flags);

	/* We have to check it to avoid races with another CPU */
	desc = irq_desc_ptrs[irq];
	if (desc)
		goto out_unlock;

	if (slab_is_available())
		desc = kzalloc_node(sizeof(*desc), GFP_ATOMIC, node);
	else
		desc = alloc_bootmem_node(NODE_DATA(node), sizeof(*desc));

	printk(KERN_DEBUG "  alloc irq_desc for %d on node %d\n", irq, node);
	if (!desc) {
		printk(KERN_ERR "can not alloc irq_desc\n");
		BUG_ON(1);
	}
	init_one_irq_desc(irq, desc, node);

	irq_desc_ptrs[irq] = desc;

out_unlock:
	spin_unlock_irqrestore(&sparse_irq_lock, flags);

	return desc;
}

#else /* !CONFIG_SPARSE_IRQ */

struct irq_desc irq_desc[NR_IRQS] __cacheline_aligned_in_smp = {
	[0 ... NR_IRQS-1] = {
		.status = IRQ_DISABLED,      // 默认屏蔽中断
		.chip = &no_irq_chip,        // 没有与chip相关联
		.handle_irq = handle_bad_irq,// 未知（坏的）IRQ处理程序，输出IRQ信息供调试，更新CPU IRQ次数计数器，回应IRQ
		.depth = 1,                  // 默认是第一层（没有嵌套中断）
		.lock = __SPIN_LOCK_UNLOCKED(irq_desc->lock),
	}
};

static unsigned int kstat_irqs_all[NR_IRQS][NR_CPUS];
int __init early_irq_init(void)
{
	struct irq_desc *desc;
	int count;
	int i;

	init_irq_default_affinity();

	printk(KERN_INFO "NR_IRQS:%d\n", NR_IRQS);

	desc = irq_desc;
	count = ARRAY_SIZE(irq_desc);

	for (i = 0; i < count; i++) {
		desc[i].irq = i;
		alloc_desc_masks(&desc[i], 0, true);
		init_desc_masks(&desc[i]);
		desc[i].kstat_irqs = kstat_irqs_all[i];
	}
	return arch_early_irq_init();
}

struct irq_desc *irq_to_desc(unsigned int irq)
{
	return (irq < NR_IRQS) ? irq_desc + irq : NULL;
}

struct irq_desc *irq_to_desc_alloc_node(unsigned int irq, int node)
{
	return irq_to_desc(irq);
}
#endif /* !CONFIG_SPARSE_IRQ */

void clear_kstat_irqs(struct irq_desc *desc)
{
	memset(desc->kstat_irqs, 0, nr_cpu_ids * sizeof(*(desc->kstat_irqs)));
}

/*
 * What should we do if we get a hw irq event on an illegal vector?
 * Each architecture has to answer this themself.
 */
static void ack_bad(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	print_irq_desc(irq, desc);
	ack_bad_irq(irq);
}

/*
 * NOP functions
 */
static void noop(unsigned int irq)
{
}

static unsigned int noop_ret(unsigned int irq)
{
	return 0;
}

/*
 * Generic no controller implementation
 */
struct irq_chip no_irq_chip = {
	.name		= "none",
	.startup	= noop_ret,
	.shutdown	= noop,
	.enable		= noop,
	.disable	= noop,
	.ack		= ack_bad,
	.end		= noop,
};

/*
 * Generic dummy implementation which can be used for
 * real dumb interrupt sources
 */
struct irq_chip dummy_irq_chip = {
	.name		= "dummy",
	.startup	= noop_ret,
	.shutdown	= noop,
	.enable		= noop,
	.disable	= noop,
	.ack		= noop,
	.mask		= noop,
	.unmask		= noop,
	.end		= noop,
};

/*
 * Special, empty irq handler:
 */
irqreturn_t no_action(int cpl, void *dev_id)
{
	return IRQ_NONE;
}

static void warn_no_thread(unsigned int irq, struct irqaction *action)
{
	if (test_and_set_bit(IRQTF_WARNED, &action->thread_flags))
		return;

	printk(KERN_WARNING "IRQ %d device %s returned IRQ_WAKE_THREAD "
	       "but no thread function available.", irq, action->name);
}

/**
 * handle_IRQ_event - irq action chain handler
 * @irq:	the interrupt number
 * @action:	the interrupt action chain for this irq
 *
 * Handles the action chain of an irq event
 */
irqreturn_t handle_IRQ_event(unsigned int irq, struct irqaction *action)
{
	irqreturn_t ret, retval = IRQ_NONE;
	unsigned int flags = 0;

   // 因为CPU会禁止中断，这里将其打开。
   // 如果没有指定IRQF_DISABLED标志的话，它表示处理程序在中断禁止情况下运行
   // 也就是允不允许中断被打断
	if (!(action->flags & IRQF_DISABLED))
		local_irq_enable_in_hardirq();

   // 遍历当前irq的action链表中所有的action，调用它们的handler
	do {
		trace_irq_handler_entry(irq, action);
		ret = action->handler(irq, action->dev_id);
		trace_irq_handler_exit(irq, action, ret);

		switch (ret) {
		case IRQ_WAKE_THREAD:
			/*
			 * Set result to handled so the spurious check
			 * does not trigger.
			 */
			ret = IRQ_HANDLED;

			/*
			 * Catch drivers which return WAKE_THREAD but
			 * did not set up a thread function
			 */
			if (unlikely(!action->thread_fn)) {
				warn_no_thread(irq, action);
				break;
			}

			/*
			 * Wake up the handler thread for this
			 * action. In case the thread crashed and was
			 * killed we just pretend that we handled the
			 * interrupt. The hardirq handler above has
			 * disabled the device interrupt, so no irq
			 * storm is lurking.
			 */
			if (likely(!test_bit(IRQTF_DIED,
					     &action->thread_flags))) {
				set_bit(IRQTF_RUNTHREAD, &action->thread_flags);
				wake_up_process(action->thread);
			}

			/* Fall through to add to randomness */
		case IRQ_HANDLED:
			flags |= action->flags;
			break;

		default:
			break;
		}

		retval |= ret;
		action = action->next;
	} while (action);

	add_interrupt_randomness(irq, flags);
	local_irq_disable();

	return retval;
}

#ifndef CONFIG_GENERIC_HARDIRQS_NO__DO_IRQ

#ifdef CONFIG_ENABLE_WARN_DEPRECATED
# warning __do_IRQ is deprecated. Please convert to proper flow handlers
#endif

/**
 * __do_IRQ - original all in one highlevel IRQ handler
 * @irq:	the interrupt number
 *
 * __do_IRQ handles all normal device IRQ's (the special
 * SMP cross-CPU interrupts have their own specific
 * handlers).
 *
 * This is the original x86 implementation which is used for every
 * interrupt type.
 */
unsigned int __do_IRQ(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	struct irqaction *action;
	unsigned int status;

	kstat_incr_irqs_this_cpu(irq, desc);

	if (CHECK_IRQ_PER_CPU(desc->status)) {
		irqreturn_t action_ret;

		/*
		 * No locking required for CPU-local interrupts:
		 */
      // 确认中断
		if (desc->chip->ack)
			desc->chip->ack(irq);
		if (likely(!(desc->status & IRQ_DISABLED))) {
			action_ret = handle_IRQ_event(irq, desc->action);
			if (!noirqdebug)
				note_interrupt(irq, desc, action_ret);
		}
		desc->chip->end(irq);
		return 1;
	}

   /*
    * 加自旋锁。对于多CPU系统，这是必须的，因为同类型的其他中断可能产生，并被其他CPU处理，
    * 没有自旋锁，IRQ描述符将被多个CPU同时访问
    */
	spin_lock(&desc->lock);

   /*
    * 确认中断。对于8259A PIC，由mask_and_ack_8259A函数完成确认，并禁用当前IRQ线。
    * 屏蔽中断是为了确保该中断处理程序结束前，CPU不会又接到这种中断。
    * 虽然CPU在处理中断会自动清除EFLAGS中的IF标志，但是在执行中断服务例程前，可能重新激活
    * 本地中断，见handle_IRQ_event
    *
    * 在多处理器上，应答中断依赖于具体的中断类型。可能由ack方法做，也可能由end方法做。
    * 不管怎样，在中断处理结束前，本地APIC不再接收同样的中断，尽管这种中断可以被其他CPU接收
    */
	if (desc->chip->ack)
		desc->chip->ack(irq);
	/*
	 * REPLAY is when Linux resends an IRQ that was dropped earlier
	 * WAITING is used by probe to mark irqs that are being tested
	 */
	status = desc->status & ~(IRQ_REPLAY | IRQ_WAITING);
   // IRQ_PENDING表示一个IRQ已经出现在中断线上，且被应答，但还没有为它提供服务
	status |= IRQ_PENDING; /* we _want_ to handle it */

	/*
	 * If the IRQ is disabled for whatever reason, we cannot
	 * use the action we have.
    *
    * 现在开始检查是否真的需要处理中断。在三种情况下什么也不做：
    * （1）IRQ_DISABLED被设置。即使在相应的IRQ线被禁止的情况下，do_IRQ也可能执行
    * （2）IRQ_INPROGRESS被设置。在多CPU系统中，表示其他CPU正在处理同样中断的前一次发生。
    *      Linux中，同类型中断的中断服务例程由同一个CPU处理，这样使得中断服务例程不必是
    *      可重入的（在同一CPU上串行执行）
    * （3）action == NULL
	 */
	action = NULL;
	if (likely(!(status & (IRQ_DISABLED | IRQ_INPROGRESS)))) {
		action = desc->action;
      // 清除IRQ_PENDING标志
		status &= ~IRQ_PENDING; /* we commit to handling */
      /*
       * 表示当前CPU正在处理该中断，其他CPU不应该处理同样的中断，而应该让给本CPU处理。
       * 一旦设置了IRQ_INPROGRESS，其他CPU即使进行do_IRQ，也不会执行该程序段，则action==NULL，
       * 其他CPU什么也不做。当调用handle_IRQ_event执行中断服务例程时，由于释放了自旋锁，其他
       * CPU可能接收到同类型的中断（本CPU不会接收同类型中断），而进入do_IRQ，并设置IRQ_PENDING
       */
		status |= IRQ_INPROGRESS; /* we are handling it */
	}
	desc->status = status;

	/*
	 * If there is no IRQ handler or it was disabled, exit early.
	 * Since we set PENDING, if another processor is handling
	 * a different instance of this same irq, the other processor
	 * will take care of it.
	 */
	if (unlikely(!action))
		goto out;

	/*
	 * Edge triggered interrupts need to remember
	 * pending events.
	 * This applies to any hw interrupts that allow a second
	 * instance of the same irq to arrive while we are in do_IRQ
	 * or in the handler. But the code here only handles the _second_
	 * instance of the irq, not the third or fourth. So it is mostly
	 * useful for irq hardware that does not mask cleanly in an
	 * SMP environment.
	 */
	for (;;) {
		irqreturn_t action_ret;

      // 释放自旋锁
		spin_unlock(&desc->lock);

		action_ret = handle_IRQ_event(irq, action);
		if (!noirqdebug)
			note_interrupt(irq, desc, action_ret);
      
      /*
       * 如果此时IRQ_PENDING处于清除状态，说明中断服务例程已经执行完毕，退出循环。
       * 反之，说明在执行中断服务例程时，其他CPU进入过do_IRQ，并设置了IRQ_PENDING，
       * 也就是说，其他CPU收到了同类型的中断。此时应该清楚IRQ_PENDING，并重新循环，
       * 执行中断服务例程，处理其他CPU收到的中断
       */

      // 加自旋锁
		spin_lock(&desc->lock);
		if (likely(!(desc->status & IRQ_PENDING)))
			break;
		desc->status &= ~IRQ_PENDING;
	}
   // 所有中断处理完毕，清除IRQ_INPROGRESS
	desc->status &= ~IRQ_INPROGRESS;

out:
	/*
	 * The ->end() handler has to deal with interrupts which got
	 * disabled while the handler was running.
    * 结束中断处理，对end_8259A_irq()仅仅是重新激活中断线
    * 对于多处理器，end应答中断（如果ack方法还没有做的话）
	 */
	desc->chip->end(irq);
	spin_unlock(&desc->lock);

	return 1;
}
#endif

void early_init_irq_lock_class(void)
{
	struct irq_desc *desc;
	int i;

	for_each_irq_desc(i, desc) {
		lockdep_set_class(&desc->lock, &irq_desc_lock_class);
	}
}

unsigned int kstat_irqs_cpu(unsigned int irq, int cpu)
{
	struct irq_desc *desc = irq_to_desc(irq);
	return desc ? desc->kstat_irqs[cpu] : 0;
}
EXPORT_SYMBOL(kstat_irqs_cpu);

