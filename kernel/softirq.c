/*
 *	linux/kernel/softirq.c
 *
 *	Copyright (C) 1992 Linus Torvalds
 *
 *	Distribute under GPLv2.
 *
 *	Rewritten. Old one was good in 2.2, but in 2.3 it was immoral. --ANK (990903)
 *
 *	Remote softirq infrastructure is by Jens Axboe.
 */

#include <linux/module.h>
#include <linux/kernel_stat.h>
#include <linux/interrupt.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/notifier.h>
#include <linux/percpu.h>
#include <linux/cpu.h>
#include <linux/freezer.h>
#include <linux/kthread.h>
#include <linux/rcupdate.h>
#include <linux/ftrace.h>
#include <linux/smp.h>
#include <linux/tick.h>

#define CREATE_TRACE_POINTS
#include <trace/events/irq.h>

#include <asm/irq.h>
/*
   - No shared variables, all the data are CPU local.
   - If a softirq needs serialization, let it serialize itself
     by its own spinlocks.
   - Even if softirq is serialized, only local cpu is marked for
     execution. Hence, we get something sort of weak cpu binding.
     Though it is still not clear, will it result in better locality
     or will not.

   Examples:
   - NET RX softirq. It is multithreaded and does not require
     any global serialization.
   - NET TX softirq. It kicks software netdevice queues, hence
     it is logically serialized per device, but this serialization
     is invisible to common code.
   - Tasklets: serialized wrt itself.
 */

#ifndef __ARCH_IRQ_STAT
irq_cpustat_t irq_stat[NR_CPUS] ____cacheline_aligned;
EXPORT_SYMBOL(irq_stat);
#endif

// 全局软中断数组
// 一共定义了32个软中断请求描述符。
// 这个数组是个系统级的全局数组，它被所有的CPU所共享。
// 每个CPU虽然都有它自己的触发和控制机制，并且只执行它自己触发的软中断请求，
// 但是，各个CPU所执行的软中断服务例程却是相同的，也即是softirq_vec定义的软中断服务例程
static struct softirq_action softirq_vec[NR_SOFTIRQS] __cacheline_aligned_in_smp;

static DEFINE_PER_CPU(struct task_struct *, ksoftirqd);

char *softirq_to_name[NR_SOFTIRQS] = {
	"HI", "TIMER", "NET_TX", "NET_RX", "BLOCK", "BLOCK_IOPOLL",
	"TASKLET", "SCHED", "HRTIMER",	"RCU"
};

/*
 * we cannot loop indefinitely here to avoid userspace starvation,
 * but we also don't want to introduce a worst case 1/HZ latency
 * to the pending events, so lets the scheduler to balance
 * the softirq load for us.
 */
void wakeup_softirqd(void)
{
	/* Interrupts are disabled: no need to stop preemption */
	struct task_struct *tsk = __get_cpu_var(ksoftirqd);

	if (tsk && tsk->state != TASK_RUNNING)
		wake_up_process(tsk);
}

/*
 * preempt_count and SOFTIRQ_OFFSET usage:
 * - preempt_count is changed by SOFTIRQ_OFFSET on entering or leaving
 *   softirq processing.
 * - preempt_count is changed by SOFTIRQ_DISABLE_OFFSET (= 2 * SOFTIRQ_OFFSET)
 *   on local_bh_disable or local_bh_enable.
 * This lets us distinguish between whether we are currently processing
 * softirq and whether we just have bh disabled.
 */

/*
 * This one is for softirq.c-internal use,
 * where hardirqs are disabled legitimately:
 */
#ifdef CONFIG_TRACE_IRQFLAGS
static void __local_bh_disable(unsigned long ip, unsigned int cnt)
{
	unsigned long flags;

	WARN_ON_ONCE(in_irq());

	raw_local_irq_save(flags);
	/*
	 * The preempt tracer hooks into add_preempt_count and will break
	 * lockdep because it calls back into lockdep after SOFTIRQ_OFFSET
	 * is set and before current->softirq_enabled is cleared.
	 * We must manually increment preempt_count here and manually
	 * call the trace_preempt_off later.
	 */
	preempt_count() += cnt;
	/*
	 * Were softirqs turned off above:
	 */
	if (softirq_count() == cnt)
		trace_softirqs_off(ip);
	raw_local_irq_restore(flags);

	if (preempt_count() == cnt)
		trace_preempt_off(CALLER_ADDR0, get_parent_ip(CALLER_ADDR1));
}
#else /* !CONFIG_TRACE_IRQFLAGS */
static inline void __local_bh_disable(unsigned long ip, unsigned int cnt)
{
	add_preempt_count(cnt);
	barrier();
}
#endif /* CONFIG_TRACE_IRQFLAGS */

void local_bh_disable(void)
{
	__local_bh_disable((unsigned long)__builtin_return_address(0),
				SOFTIRQ_DISABLE_OFFSET);
}

EXPORT_SYMBOL(local_bh_disable);

static void __local_bh_enable(unsigned int cnt)
{
	WARN_ON_ONCE(in_irq());
	WARN_ON_ONCE(!irqs_disabled());

	if (softirq_count() == cnt)
		trace_softirqs_on((unsigned long)__builtin_return_address(0));
	sub_preempt_count(cnt);
}

/*
 * Special-case - softirqs can safely be enabled in
 * cond_resched_softirq(), or by __do_softirq(),
 * without processing still-pending softirqs:
 */
void _local_bh_enable(void)
{
	__local_bh_enable(SOFTIRQ_DISABLE_OFFSET);
}

EXPORT_SYMBOL(_local_bh_enable);

static inline void _local_bh_enable_ip(unsigned long ip)
{
	WARN_ON_ONCE(in_irq() || irqs_disabled());
#ifdef CONFIG_TRACE_IRQFLAGS
	local_irq_disable();
#endif
	/*
	 * Are softirqs going to be turned on now:
	 */
	if (softirq_count() == SOFTIRQ_DISABLE_OFFSET)
		trace_softirqs_on(ip);
	/*
	 * Keep preemption disabled until we are done with
	 * softirq processing:
 	 */
	sub_preempt_count(SOFTIRQ_DISABLE_OFFSET - 1);

	if (unlikely(!in_interrupt() && local_softirq_pending()))
		do_softirq();

	dec_preempt_count();
#ifdef CONFIG_TRACE_IRQFLAGS
	local_irq_enable();
#endif
	preempt_check_resched();
}

void local_bh_enable(void)
{
	_local_bh_enable_ip((unsigned long)__builtin_return_address(0));
}
EXPORT_SYMBOL(local_bh_enable);

void local_bh_enable_ip(unsigned long ip)
{
	_local_bh_enable_ip(ip);
}
EXPORT_SYMBOL(local_bh_enable_ip);

/*
 * We restart softirq processing for at most MAX_SOFTIRQ_RESTART times,
 * but break the loop if need_resched() is set or after 2 ms.
 * The MAX_SOFTIRQ_TIME provides a nice upper bound in most cases, but in
 * certain cases, such as stop_machine(), jiffies may cease to
 * increment and so we need the MAX_SOFTIRQ_RESTART limit as
 * well to make sure we eventually return from this method.
 *
 * These limits have been established via experimentation.
 * The two things to balance is latency against fairness -
 * we want to handle softirqs as soon as possible, but they
 * should not be able to lock up the box.
 */
#define MAX_SOFTIRQ_TIME  msecs_to_jiffies(2)
// 最大软中断调用次数为10次
#define MAX_SOFTIRQ_RESTART 10

/*
 * http://linux.chinaunix.net/techdoc/system/2007/06/12/959951.shtml
 */
asmlinkage void __do_softirq(void)
{
   // 软中断处理结构，这个结构包括了ISR中注册的回调函数
	struct softirq_action *h;
	__u32 pending;
	unsigned long end = jiffies + MAX_SOFTIRQ_TIME;
	int cpu;
	int max_restart = MAX_SOFTIRQ_RESTART;

   // 得到当前所有pending的软中断
	pending = local_softirq_pending();
	account_system_vtime(current);

   // 执行到这里要屏蔽其他软中断。
   // 这里也就证明了每个CPU上同时运行的软中断只能有一个
	__local_bh_disable((unsigned long)__builtin_return_address(0),
				SOFTIRQ_OFFSET);
	lockdep_softirq_enter();

   // 针对SMP得到当前正在处理的CPU
	cpu = smp_processor_id();
restart:
	/* Reset the pending bitmask before enabling irqs */
	set_softirq_pending(0);

   // 到这里才开中断运行。
   // 注意：以前运行状态一直是关中断运行，这是当前处理软中断才可能被硬件中断抢占。
   // 也就是说，进入软中断时不是一开始就会被硬件中断抢占。
   // 只有在这里以后的代码才可能被硬件中断抢占
	local_irq_enable();

   // 这里要注意：以下代码运行时可能被硬件中断抢占，但这个硬件ISR执行完成后，它的所注册的软中断
   // 无法马上运行。因为虽是开硬件中断执行，但是前面的__local_bh_enable函数屏蔽了软中断。
   // 所以这种情况下只能被硬件中断抢占，但这个硬件中断注册的软中断回调函数无法执行。
   
   // 得到软中断向量表
	h = softirq_vec;

   // 循环处理所有软中断注册函数
	do {
      // 如果对应的软中断设置pending标志则表明需要进一步处理它注册的函数
		if (pending & 1) {
			int prev_count = preempt_count();
			kstat_incr_softirqs_this_cpu(h - softirq_vec);

			trace_softirq_entry(h, softirq_vec);
			h->action(h);
			trace_softirq_exit(h, softirq_vec);
			if (unlikely(prev_count != preempt_count())) {
				printk(KERN_ERR "huh, entered softirq %td %s %p"
				       "with preempt_count %08x,"
				       " exited with %08x?\n", h - softirq_vec,
				       softirq_to_name[h - softirq_vec],
				       h->action, prev_count, preempt_count());
				preempt_count() = prev_count;
			}

			rcu_bh_qs(cpu);
		}
		h++;
      // 按位操作，表明一次循环只处理32个软中断回调函数
		pending >>= 1;
	} while (pending);

   // 关中断。
   // 注意：这里又关中断了，下面的代码执行过程中硬件中断无法抢占
	local_irq_disable();

   // 上面提到过，在刚才开硬件中断执行环境时只能被硬件中断抢占，但是无法处理软中断的，
   // 因为刚才开中断执行过程中可能多次被硬件中断抢占，每抢占一次就有可能注册一个软中断，
   // 所以要再重新取一次所有的软中断，以便下面的代码进行处理后跳转到restart处重复执行。
	pending = local_softirq_pending();
   // 如果在上面的开中断执行环境中触发了硬件中断，且每个都注册了一个软中断的话，
   // 这个软中断会设置pending位，但是当前一直屏蔽软中断的环境下无法得到执行。
   // 那么在这里又有了一次执行的机会。
	if (pending) {
		if (time_before(jiffies, end) && !need_resched() &&
		    --max_restart)
			goto restart;

		wakeup_softirqd();
	}

	lockdep_softirq_exit();

	account_system_vtime(current);
	__local_bh_enable(SOFTIRQ_OFFSET);
}

#ifndef __ARCH_HAS_DO_SOFTIRQ

asmlinkage void do_softirq(void)
{
	__u32 pending;
	unsigned long flags;

   // 判断当前是否有硬件中断嵌套，或者有软中断正在执行，是则返回。
   // 这个入口判断主要是为了与ksoftirqd互斥
	if (in_interrupt())
		return;

   // 关中断执行以下代码
	local_irq_save(flags);

   // 判断是否有pending的软中断需要处理
   // 有则调用__do_softirq处理
	pending = local_softirq_pending();

	if (pending)
		__do_softirq();

	local_irq_restore(flags);
}

#endif

/*
 * Enter an interrupt context.
 */
void irq_enter(void)
{
	int cpu = smp_processor_id();

	rcu_irq_enter();
	if (idle_cpu(cpu) && !in_interrupt()) {
		/*
		 * Prevent raise_softirq from needlessly waking up ksoftirqd
		 * here, as softirq will be serviced on return from interrupt.
		 */
		local_bh_disable();
		tick_check_idle(cpu);
		_local_bh_enable();
	}

	__irq_enter();
}

#ifdef __ARCH_IRQ_EXIT_IRQS_DISABLED
# define invoke_softirq()	__do_softirq()
#else
# define invoke_softirq()	do_softirq()
#endif

/*
 * Exit an interrupt context. Process softirqs if needed and possible:
 * do_IRQ函数执行完硬件ISR后退出是调用此函数
 */
void irq_exit(void)
{
	account_system_vtime(current);
	trace_hardirq_exit();
	sub_preempt_count(IRQ_EXIT_OFFSET);
   // 判断当前是否有硬件中断嵌套，并且是否有软中断在pending状态。
   // 注意：这里只有这两个条件同时满足，才有可能调用do_softirq进入软中断。
   // 也就是说，确认当前所有硬件中断处理完成，且有硬件中断安装了软中断处理
   // 时才会进入
	if (!in_interrupt() && local_softirq_pending())
      // 这里其实就是调用do_softirq
		invoke_softirq();

#ifdef CONFIG_NO_HZ
	/* Make sure that timer wheel updates are propagated */
	rcu_irq_exit();
	if (idle_cpu(smp_processor_id()) && !in_interrupt() && !need_resched())
		tick_nohz_stop_sched_tick(0);
#endif
	preempt_enable_no_resched();
}

/*
 * This function must run with irqs disabled!
 */
inline void raise_softirq_irqoff(unsigned int nr)
{
	__raise_softirq_irqoff(nr);

	/*
	 * If we're in an interrupt or softirq, we're done
	 * (this also catches softirq-disabled code). We will
	 * actually run the softirq once we return from
	 * the irq or softirq.
	 *
	 * Otherwise we wake up ksoftirqd to make sure we
	 * schedule the softirq soon.
	 */
	if (!in_interrupt())
		wakeup_softirqd();
}

/*
 * 激活软中断
 * 1. 执行local_irq_save宏以保存EFLAGS寄存器IF标志并禁用本地CPU上的中断
 * 2. 把软中断标记为挂起状态，通过设置本地CPU的软中断掩码中与下标nr相关的位来实现
 * 3. 如果in_interrupt()产生为1的值，跳转到第5步
 *    这种情况是：要么已经在中断上下文中调用了raise_softirq，要么当前禁用了软中断
 * 4. 否则，就在需要的时候去调用wakeup_softirqd以唤醒本地CPU的ksoftirqd内核线程
 * 5. 执行local_irq_restore宏，恢复IF的状态值
 */
void raise_softirq(unsigned int nr)
{
	unsigned long flags;

	local_irq_save(flags);
	raise_softirq_irqoff(nr);
	local_irq_restore(flags);
}

// 软中断的初始化，设置软中断下标和对应的软中断处理函数指针
void open_softirq(int nr, void (*action)(struct softirq_action *))
{
	softirq_vec[nr].action = action;
}

/*
 * Tasklets
 */
struct tasklet_head
{
	struct tasklet_struct *head;
	struct tasklet_struct **tail;
};

static DEFINE_PER_CPU(struct tasklet_head, tasklet_vec);
static DEFINE_PER_CPU(struct tasklet_head, tasklet_hi_vec);

void __tasklet_schedule(struct tasklet_struct *t)
{
	unsigned long flags;

	local_irq_save(flags);
	t->next = NULL;
	*__get_cpu_var(tasklet_vec).tail = t;
	__get_cpu_var(tasklet_vec).tail = &(t->next);
	raise_softirq_irqoff(TASKLET_SOFTIRQ);
	local_irq_restore(flags);
}

EXPORT_SYMBOL(__tasklet_schedule);

void __tasklet_hi_schedule(struct tasklet_struct *t)
{
	unsigned long flags;

	local_irq_save(flags);
	t->next = NULL;
	*__get_cpu_var(tasklet_hi_vec).tail = t;
	__get_cpu_var(tasklet_hi_vec).tail = &(t->next);
	raise_softirq_irqoff(HI_SOFTIRQ);
	local_irq_restore(flags);
}

EXPORT_SYMBOL(__tasklet_hi_schedule);

void __tasklet_hi_schedule_first(struct tasklet_struct *t)
{
	BUG_ON(!irqs_disabled());

	t->next = __get_cpu_var(tasklet_hi_vec).head;
	__get_cpu_var(tasklet_hi_vec).head = t;
	__raise_softirq_irqoff(HI_SOFTIRQ);
}

EXPORT_SYMBOL(__tasklet_hi_schedule_first);

static void tasklet_action(struct softirq_action *a)
{
	struct tasklet_struct *list;

	local_irq_disable();
	list = __get_cpu_var(tasklet_vec).head;
	__get_cpu_var(tasklet_vec).head = NULL;
	__get_cpu_var(tasklet_vec).tail = &__get_cpu_var(tasklet_vec).head;
	local_irq_enable();

	while (list) {
		struct tasklet_struct *t = list;

		list = list->next;

		if (tasklet_trylock(t)) {
			if (!atomic_read(&t->count)) {
				if (!test_and_clear_bit(TASKLET_STATE_SCHED, &t->state))
					BUG();
				t->func(t->data);
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}

		local_irq_disable();
		t->next = NULL;
		*__get_cpu_var(tasklet_vec).tail = t;
		__get_cpu_var(tasklet_vec).tail = &(t->next);
		__raise_softirq_irqoff(TASKLET_SOFTIRQ);
		local_irq_enable();
	}
}

static void tasklet_hi_action(struct softirq_action *a)
{
	struct tasklet_struct *list;

	local_irq_disable();
	list = __get_cpu_var(tasklet_hi_vec).head;
	__get_cpu_var(tasklet_hi_vec).head = NULL;
	__get_cpu_var(tasklet_hi_vec).tail = &__get_cpu_var(tasklet_hi_vec).head;
	local_irq_enable();

	while (list) {
		struct tasklet_struct *t = list;

		list = list->next;

		if (tasklet_trylock(t)) {
			if (!atomic_read(&t->count)) {
				if (!test_and_clear_bit(TASKLET_STATE_SCHED, &t->state))
					BUG();
				t->func(t->data);
				tasklet_unlock(t);
				continue;
			}
			tasklet_unlock(t);
		}

		local_irq_disable();
		t->next = NULL;
		*__get_cpu_var(tasklet_hi_vec).tail = t;
		__get_cpu_var(tasklet_hi_vec).tail = &(t->next);
		__raise_softirq_irqoff(HI_SOFTIRQ);
		local_irq_enable();
	}
}


void tasklet_init(struct tasklet_struct *t,
		  void (*func)(unsigned long), unsigned long data)
{
	t->next = NULL;
	t->state = 0;
	atomic_set(&t->count, 0);
	t->func = func;
	t->data = data;
}

EXPORT_SYMBOL(tasklet_init);

void tasklet_kill(struct tasklet_struct *t)
{
	if (in_interrupt())
		printk("Attempt to kill tasklet from interrupt\n");

	while (test_and_set_bit(TASKLET_STATE_SCHED, &t->state)) {
		do {
			yield();
		} while (test_bit(TASKLET_STATE_SCHED, &t->state));
	}
	tasklet_unlock_wait(t);
	clear_bit(TASKLET_STATE_SCHED, &t->state);
}

EXPORT_SYMBOL(tasklet_kill);

/*
 * tasklet_hrtimer
 */

/*
 * The trampoline is called when the hrtimer expires. If this is
 * called from the hrtimer interrupt then we schedule the tasklet as
 * the timer callback function expects to run in softirq context. If
 * it's called in softirq context anyway (i.e. high resolution timers
 * disabled) then the hrtimer callback is called right away.
 */
static enum hrtimer_restart __hrtimer_tasklet_trampoline(struct hrtimer *timer)
{
	struct tasklet_hrtimer *ttimer =
		container_of(timer, struct tasklet_hrtimer, timer);

	if (hrtimer_is_hres_active(timer)) {
		tasklet_hi_schedule(&ttimer->tasklet);
		return HRTIMER_NORESTART;
	}
	return ttimer->function(timer);
}

/*
 * Helper function which calls the hrtimer callback from
 * tasklet/softirq context
 */
static void __tasklet_hrtimer_trampoline(unsigned long data)
{
	struct tasklet_hrtimer *ttimer = (void *)data;
	enum hrtimer_restart restart;

	restart = ttimer->function(&ttimer->timer);
	if (restart != HRTIMER_NORESTART)
		hrtimer_restart(&ttimer->timer);
}

/**
 * tasklet_hrtimer_init - Init a tasklet/hrtimer combo for softirq callbacks
 * @ttimer:	 tasklet_hrtimer which is initialized
 * @function:	 hrtimer callback funtion which gets called from softirq context
 * @which_clock: clock id (CLOCK_MONOTONIC/CLOCK_REALTIME)
 * @mode:	 hrtimer mode (HRTIMER_MODE_ABS/HRTIMER_MODE_REL)
 */
void tasklet_hrtimer_init(struct tasklet_hrtimer *ttimer,
			  enum hrtimer_restart (*function)(struct hrtimer *),
			  clockid_t which_clock, enum hrtimer_mode mode)
{
	hrtimer_init(&ttimer->timer, which_clock, mode);
	ttimer->timer.function = __hrtimer_tasklet_trampoline;
	tasklet_init(&ttimer->tasklet, __tasklet_hrtimer_trampoline,
		     (unsigned long)ttimer);
	ttimer->function = function;
}
EXPORT_SYMBOL_GPL(tasklet_hrtimer_init);

/*
 * Remote softirq bits
 */

DEFINE_PER_CPU(struct list_head [NR_SOFTIRQS], softirq_work_list);
EXPORT_PER_CPU_SYMBOL(softirq_work_list);

static void __local_trigger(struct call_single_data *cp, int softirq)
{
	struct list_head *head = &__get_cpu_var(softirq_work_list[softirq]);

	list_add_tail(&cp->list, head);

	/* Trigger the softirq only if the list was previously empty.  */
	if (head->next == &cp->list)
		raise_softirq_irqoff(softirq);
}

#ifdef CONFIG_USE_GENERIC_SMP_HELPERS
static void remote_softirq_receive(void *data)
{
	struct call_single_data *cp = data;
	unsigned long flags;
	int softirq;

	softirq = cp->priv;

	local_irq_save(flags);
	__local_trigger(cp, softirq);
	local_irq_restore(flags);
}

static int __try_remote_softirq(struct call_single_data *cp, int cpu, int softirq)
{
	if (cpu_online(cpu)) {
		cp->func = remote_softirq_receive;
		cp->info = cp;
		cp->flags = 0;
		cp->priv = softirq;

		__smp_call_function_single(cpu, cp, 0);
		return 0;
	}
	return 1;
}
#else /* CONFIG_USE_GENERIC_SMP_HELPERS */
static int __try_remote_softirq(struct call_single_data *cp, int cpu, int softirq)
{
	return 1;
}
#endif

/**
 * __send_remote_softirq - try to schedule softirq work on a remote cpu
 * @cp: private SMP call function data area
 * @cpu: the remote cpu
 * @this_cpu: the currently executing cpu
 * @softirq: the softirq for the work
 *
 * Attempt to schedule softirq work on a remote cpu.  If this cannot be
 * done, the work is instead queued up on the local cpu.
 *
 * Interrupts must be disabled.
 */
void __send_remote_softirq(struct call_single_data *cp, int cpu, int this_cpu, int softirq)
{
	if (cpu == this_cpu || __try_remote_softirq(cp, cpu, softirq))
		__local_trigger(cp, softirq);
}
EXPORT_SYMBOL(__send_remote_softirq);

/**
 * send_remote_softirq - try to schedule softirq work on a remote cpu
 * @cp: private SMP call function data area
 * @cpu: the remote cpu
 * @softirq: the softirq for the work
 *
 * Like __send_remote_softirq except that disabling interrupts and
 * computing the current cpu is done for the caller.
 */
void send_remote_softirq(struct call_single_data *cp, int cpu, int softirq)
{
	unsigned long flags;
	int this_cpu;

	local_irq_save(flags);
	this_cpu = smp_processor_id();
	__send_remote_softirq(cp, cpu, this_cpu, softirq);
	local_irq_restore(flags);
}
EXPORT_SYMBOL(send_remote_softirq);

static int __cpuinit remote_softirq_cpu_notify(struct notifier_block *self,
					       unsigned long action, void *hcpu)
{
	/*
	 * If a CPU goes away, splice its entries to the current CPU
	 * and trigger a run of the softirq
	 */
	if (action == CPU_DEAD || action == CPU_DEAD_FROZEN) {
		int cpu = (unsigned long) hcpu;
		int i;

		local_irq_disable();
		for (i = 0; i < NR_SOFTIRQS; i++) {
			struct list_head *head = &per_cpu(softirq_work_list[i], cpu);
			struct list_head *local_head;

			if (list_empty(head))
				continue;

			local_head = &__get_cpu_var(softirq_work_list[i]);
			list_splice_init(head, local_head);
			raise_softirq_irqoff(i);
		}
		local_irq_enable();
	}

	return NOTIFY_OK;
}

static struct notifier_block __cpuinitdata remote_softirq_cpu_notifier = {
	.notifier_call	= remote_softirq_cpu_notify,
};

void __init softirq_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		int i;

		per_cpu(tasklet_vec, cpu).tail =
			&per_cpu(tasklet_vec, cpu).head;
		per_cpu(tasklet_hi_vec, cpu).tail =
			&per_cpu(tasklet_hi_vec, cpu).head;
		for (i = 0; i < NR_SOFTIRQS; i++)
			INIT_LIST_HEAD(&per_cpu(softirq_work_list[i], cpu));
	}

	register_hotcpu_notifier(&remote_softirq_cpu_notifier);

	open_softirq(TASKLET_SOFTIRQ, tasklet_action);
	open_softirq(HI_SOFTIRQ, tasklet_hi_action);
}

// 每个CPU都有自己的ksoftirqd/n内核线程（n为CPU的逻辑号）
static int ksoftirqd(void * __bind_cpu)
{
   // 设置当前进程状态为可中断的状态。这种睡眠状态可响应信号处理等
   // 为什么这里要设置状态？
	set_current_state(TASK_INTERRUPTIBLE);

	current->flags |= PF_KSOFTIRQD;
   // 循环判断当前进程是否会停止，不会则继续判断是否有pending的软中断需要处理
	while (!kthread_should_stop()) {
      // 如果可以进行处理，在此处理期间禁止当前进程被抢占
		preempt_disable();
      // 先判断系统当前有没有需要处理的pending状态的软中断
		if (!local_softirq_pending()) {
         // 如果没有，主动放弃CPU
         // 先要恢复允许抢占，因为一直是在不允许抢占状态下执行的代码
			preempt_enable_no_resched();
         // 显式调用此函数主动放弃CPU
         // 将当前进程放入睡眠队列，并切换新的进程执行
			schedule();
         // 注意：如果当前显式调用schedule()函数主动切换的进程再次被调度执行的话，
         // 那么，将从调用这个函数的下一条语句开始执行。
         // 也就是说，在这里当前进程再次被执行的话，将会从下面的preempt_disable开始执行
			preempt_disable();
		}

		__set_current_state(TASK_RUNNING);

      /*
       * 循环判断是否有pending的软中断，有则调用do_softirq()来处理。
       * 注意：
       * 这里是一个do_softirq的入口点，那么在__do_softirq当中循环处理10次软中断的回调函数后，
       * 如果还有pending的话，会又调用到这里
       * 处理10次还处理不完的话，说明系统正处于繁忙状态。
       * 试想如果在系统非常繁忙时，这个进程将会与do_softirq相互交替执行，这时此进程占用的
       * CPU应该会很高，虽然下面的cond_resched函数做了一些处理，它在处理完一轮软中断后
       * 当前处理进程可能会被调度而减少CPU负荷。
       */

      // 检查local_softirq_pending中的软中断位掩码
      // 在必要时调用do_softirq
      // 如果没有挂起的软中断，把当前内核线程的状态设为TASK_INTERRUPTIBLE
		while (local_softirq_pending()) {
			/* Preempt disable stops cpu going offline.
			   If already offline, we'll be on wrong CPU:
			   don't process */
			if (cpu_is_offline((long)__bind_cpu))
            // 如果当前被关联的CPU无法继续处理则跳转，等待结束并退出
				goto wait_to_die;
         // 执行do_softirq来处理具体的软中断回调函数。
         // 注意：如果此时有一个正在处理的软中断的话，则会马上返回。
			do_softirq();
         // 允许当前进程被抢占
			preempt_enable_no_resched();
         // 这个函数可能间接的调用schedule来切换当前进程，而且上面已经允许当前进程被抢占。
         // 也即是说在处理完一轮软中断回调函数时，有可能会切换到其他进程
			cond_resched();
         // 禁止当前进程被抢占
			preempt_disable();
			rcu_sched_qs((long)__bind_cpu);
		}
      // 待一切都处理完成后，允许当前进程被抢占，并设置当前进程状态可中断，
      // 继续循环以上过程
		preempt_enable();
		set_current_state(TASK_INTERRUPTIBLE);
	}
   // 如果将会停止则设置当前进程为可运行状态
   // 调度器会根据优先级来使当前进程运行
	__set_current_state(TASK_RUNNING);
	return 0;

wait_to_die:
	preempt_enable();
	/* Wait for kthread_stop */
	set_current_state(TASK_INTERRUPTIBLE);
	while (!kthread_should_stop()) {
		schedule();
		set_current_state(TASK_INTERRUPTIBLE);
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

#ifdef CONFIG_HOTPLUG_CPU
/*
 * tasklet_kill_immediate is called to remove a tasklet which can already be
 * scheduled for execution on @cpu.
 *
 * Unlike tasklet_kill, this function removes the tasklet
 * _immediately_, even if the tasklet is in TASKLET_STATE_SCHED state.
 *
 * When this function is called, @cpu must be in the CPU_DEAD state.
 */
void tasklet_kill_immediate(struct tasklet_struct *t, unsigned int cpu)
{
	struct tasklet_struct **i;

	BUG_ON(cpu_online(cpu));
	BUG_ON(test_bit(TASKLET_STATE_RUN, &t->state));

	if (!test_bit(TASKLET_STATE_SCHED, &t->state))
		return;

	/* CPU is dead, so no lock needed. */
	for (i = &per_cpu(tasklet_vec, cpu).head; *i; i = &(*i)->next) {
		if (*i == t) {
			*i = t->next;
			/* If this was the tail element, move the tail ptr */
			if (*i == NULL)
				per_cpu(tasklet_vec, cpu).tail = i;
			return;
		}
	}
	BUG();
}

static void takeover_tasklets(unsigned int cpu)
{
	/* CPU is dead, so no lock needed. */
	local_irq_disable();

	/* Find end, append list for that CPU. */
	if (&per_cpu(tasklet_vec, cpu).head != per_cpu(tasklet_vec, cpu).tail) {
		*(__get_cpu_var(tasklet_vec).tail) = per_cpu(tasklet_vec, cpu).head;
		__get_cpu_var(tasklet_vec).tail = per_cpu(tasklet_vec, cpu).tail;
		per_cpu(tasklet_vec, cpu).head = NULL;
		per_cpu(tasklet_vec, cpu).tail = &per_cpu(tasklet_vec, cpu).head;
	}
	raise_softirq_irqoff(TASKLET_SOFTIRQ);

	if (&per_cpu(tasklet_hi_vec, cpu).head != per_cpu(tasklet_hi_vec, cpu).tail) {
		*__get_cpu_var(tasklet_hi_vec).tail = per_cpu(tasklet_hi_vec, cpu).head;
		__get_cpu_var(tasklet_hi_vec).tail = per_cpu(tasklet_hi_vec, cpu).tail;
		per_cpu(tasklet_hi_vec, cpu).head = NULL;
		per_cpu(tasklet_hi_vec, cpu).tail = &per_cpu(tasklet_hi_vec, cpu).head;
	}
	raise_softirq_irqoff(HI_SOFTIRQ);

	local_irq_enable();
}
#endif /* CONFIG_HOTPLUG_CPU */

static int __cpuinit cpu_callback(struct notifier_block *nfb,
				  unsigned long action,
				  void *hcpu)
{
	int hotcpu = (unsigned long)hcpu;
	struct task_struct *p;

	switch (action) {
	case CPU_UP_PREPARE:
	case CPU_UP_PREPARE_FROZEN:
		p = kthread_create(ksoftirqd, hcpu, "ksoftirqd/%d", hotcpu);
		if (IS_ERR(p)) {
			printk("ksoftirqd for %i failed\n", hotcpu);
			return NOTIFY_BAD;
		}
		kthread_bind(p, hotcpu);
  		per_cpu(ksoftirqd, hotcpu) = p;
 		break;
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
		wake_up_process(per_cpu(ksoftirqd, hotcpu));
		break;
#ifdef CONFIG_HOTPLUG_CPU
	case CPU_UP_CANCELED:
	case CPU_UP_CANCELED_FROZEN:
		if (!per_cpu(ksoftirqd, hotcpu))
			break;
		/* Unbind so it can run.  Fall thru. */
		kthread_bind(per_cpu(ksoftirqd, hotcpu),
			     cpumask_any(cpu_online_mask));
	case CPU_DEAD:
	case CPU_DEAD_FROZEN: {
		struct sched_param param = { .sched_priority = MAX_RT_PRIO-1 };

		p = per_cpu(ksoftirqd, hotcpu);
		per_cpu(ksoftirqd, hotcpu) = NULL;
		sched_setscheduler_nocheck(p, SCHED_FIFO, &param);
		kthread_stop(p);
		takeover_tasklets(hotcpu);
		break;
	}
#endif /* CONFIG_HOTPLUG_CPU */
 	}
	return NOTIFY_OK;
}

static struct notifier_block __cpuinitdata cpu_nfb = {
	.notifier_call = cpu_callback
};

static __init int spawn_ksoftirqd(void)
{
	void *cpu = (void *)(long)smp_processor_id();
	int err = cpu_callback(&cpu_nfb, CPU_UP_PREPARE, cpu);

	BUG_ON(err == NOTIFY_BAD);
	cpu_callback(&cpu_nfb, CPU_ONLINE, cpu);
	register_cpu_notifier(&cpu_nfb);
	return 0;
}
early_initcall(spawn_ksoftirqd);

#ifdef CONFIG_SMP
/*
 * Call a function on all processors
 */
int on_each_cpu(void (*func) (void *info), void *info, int wait)
{
	int ret = 0;

	preempt_disable();
	ret = smp_call_function(func, info, wait);
	local_irq_disable();
	func(info);
	local_irq_enable();
	preempt_enable();
	return ret;
}
EXPORT_SYMBOL(on_each_cpu);
#endif

/*
 * [ These __weak aliases are kept in a separate compilation unit, so that
 *   GCC does not inline them incorrectly. ]
 */

int __init __weak early_irq_init(void)
{
	return 0;
}

int __init __weak arch_probe_nr_irqs(void)
{
	return 0;
}

int __init __weak arch_early_irq_init(void)
{
	return 0;
}

int __weak arch_init_chip_data(struct irq_desc *desc, int node)
{
	return 0;
}
