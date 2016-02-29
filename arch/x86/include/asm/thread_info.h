/* thread_info.h: low-level thread information
 *
 * Copyright (C) 2002  David Howells (dhowells@redhat.com)
 * - Incorporating suggestions made by Linus Torvalds and Dave Miller
 */

#ifndef _ASM_X86_THREAD_INFO_H
#define _ASM_X86_THREAD_INFO_H

#include <linux/compiler.h>
#include <asm/page.h>
#include <asm/types.h>

/*
 * low level task data that entry.S needs immediate access to
 * - this struct should fit entirely inside of one cache line
 * - this struct shares the supervisor stack pages
 */
#ifndef __ASSEMBLY__
struct task_struct;
struct exec_domain;
#include <asm/processor.h>
#include <asm/ftrace.h>
#include <asm/atomic.h>

/*
 * 线程描述符
 * 对于每一个进程，内核把它内核态的进程堆栈和进程对应的thread_info这两个数据结构紧凑地
 * 存放在一个单独为进程分配的内存区域内
 *
 * 为了支持内核抢占，内核引入了preempt_count字段，该计数初值为0，每当使用锁时加1，释放锁时减1。
 * 当preempt_count为0时，表示内核可以被安全的抢占，大于0时，则禁止内核抢占。
 * 该字段对应三个不同的计数器，也就是说在以下三种任何一种情况，该字段的值都会大于0：
 * （1）内核执行中断处理程序，通过irq_enter增加中断计数器的值
 * （2）可延迟函数被禁止（执行软中断和tasklet时经常如此，由local_bh_disable完成
 * （3）通过把抢占计数器设置为正而显式禁止内核抢占，由preempt_disable完成。
 *
 * 当从中断返回内核空间时，内核会检查preempt_count和need_resched的值（返回用户空间只需要检查need_resched），
 * 如查preempt_count为0且need_resched设置了，则调用scheduler函数，完成任务抢占，一般来说，内核抢占
 * 发生在以下情况：
 * （1）从中断（异常）返回时，preempt_count为0且need_resched置位
 * （2）在异常处理程序中（特别是系统调用）调用preempt_enable来允许内核抢占发生
 * （3）启用可延迟函数时，即调用local_bh_enable时发生
 * （4）内核任务显式调用scheduler，属于内核自动放弃CPU
 *
 * preempt_count用来跟踪内核抢占和内核控制路径嵌套关键数据，其个位的含义如下：
 * 0 ~ 7： preemption counter 内核抢占计数器（最大值255），用来表示内核抢占被关闭的次数，0表示可以抢占。
 * 8 ~ 15： softirq counter 软中断计数器（最大值255），表示推迟函数（下半部）被关闭的次数，0表示推迟函数打开。
 * 16 ~ 27： hardirq counter 硬件中断计数器（最大值4096），表示本地CPU中断嵌套的层数，irq_enter增加该值，irq_exit减该值
 * 28： PREEMPT_ACTIVE标志
 */
struct thread_info {
	struct task_struct	*task;		/* main task structure */
	struct exec_domain	*exec_domain;	/* execution domain */
	__u32			flags;		/* low level flags */
	__u32			status;		/* thread synchronous flags */
	__u32			cpu;		/* current CPU */
	int			preempt_count;	/* 0 => preemptable,
						   <0 => BUG */
	mm_segment_t		addr_limit;
	struct restart_block    restart_block;
	void __user		*sysenter_return;
#ifdef CONFIG_X86_32
	unsigned long           previous_esp;   /* ESP of the previous stack in
						   case of nested (IRQ) stacks
						*/
	__u8			supervisor_stack[0];
#endif
	int			uaccess_err;
};

#define INIT_THREAD_INFO(tsk)			\
{						\
	.task		= &tsk,			\
	.exec_domain	= &default_exec_domain,	\
	.flags		= 0,			\
	.cpu		= 0,			\
	.preempt_count	= INIT_PREEMPT_COUNT,	\
	.addr_limit	= KERNEL_DS,		\
	.restart_block = {			\
		.fn = do_no_restart_syscall,	\
	},					\
}

#define init_thread_info	(init_thread_union.thread_info)
#define init_stack		(init_thread_union.stack)

#else /* !__ASSEMBLY__ */

#include <asm/asm-offsets.h>

#endif

/*
 * thread information flags
 * - these are process state flags that various assembly files
 *   may need to access
 * - pending work-to-be-done flags are in LSW
 * - other flags in MSW
 * Warning: layout of LSW is hardcoded in entry.S
 */
#define TIF_SYSCALL_TRACE	0	/* syscall trace active */
#define TIF_NOTIFY_RESUME	1	/* callback before returning to user */
#define TIF_SIGPENDING		2	/* signal pending */
#define TIF_NEED_RESCHED	3	/* rescheduling necessary */
#define TIF_SINGLESTEP		4	/* reenable singlestep on user return*/
#define TIF_IRET		5	/* force IRET */
#define TIF_SYSCALL_EMU		6	/* syscall emulation active */
#define TIF_SYSCALL_AUDIT	7	/* syscall auditing active */
#define TIF_SECCOMP		8	/* secure computing */
#define TIF_MCE_NOTIFY		10	/* notify userspace of an MCE */
#define TIF_NOTSC		16	/* TSC is not accessible in userland */
#define TIF_IA32		17	/* 32bit process */
#define TIF_FORK		18	/* ret_from_fork */
#define TIF_MEMDIE		20
#define TIF_DEBUG		21	/* uses debug registers */
#define TIF_IO_BITMAP		22	/* uses I/O bitmap */
#define TIF_FREEZE		23	/* is freezing for suspend */
#define TIF_FORCED_TF		24	/* true if TF in eflags artificially */
#define TIF_DEBUGCTLMSR		25	/* uses thread_struct.debugctlmsr */
#define TIF_DS_AREA_MSR		26      /* uses thread_struct.ds_area_msr */
#define TIF_LAZY_MMU_UPDATES	27	/* task is updating the mmu lazily */
#define TIF_SYSCALL_TRACEPOINT	28	/* syscall tracepoint instrumentation */

#define _TIF_SYSCALL_TRACE	(1 << TIF_SYSCALL_TRACE)
#define _TIF_NOTIFY_RESUME	(1 << TIF_NOTIFY_RESUME)
#define _TIF_SIGPENDING		(1 << TIF_SIGPENDING)
#define _TIF_SINGLESTEP		(1 << TIF_SINGLESTEP)
#define _TIF_NEED_RESCHED	(1 << TIF_NEED_RESCHED)
#define _TIF_IRET		(1 << TIF_IRET)
#define _TIF_SYSCALL_EMU	(1 << TIF_SYSCALL_EMU)
#define _TIF_SYSCALL_AUDIT	(1 << TIF_SYSCALL_AUDIT)
#define _TIF_SECCOMP		(1 << TIF_SECCOMP)
#define _TIF_MCE_NOTIFY		(1 << TIF_MCE_NOTIFY)
#define _TIF_NOTSC		(1 << TIF_NOTSC)
#define _TIF_IA32		(1 << TIF_IA32)
#define _TIF_FORK		(1 << TIF_FORK)
#define _TIF_DEBUG		(1 << TIF_DEBUG)
#define _TIF_IO_BITMAP		(1 << TIF_IO_BITMAP)
#define _TIF_FREEZE		(1 << TIF_FREEZE)
#define _TIF_FORCED_TF		(1 << TIF_FORCED_TF)
#define _TIF_DEBUGCTLMSR	(1 << TIF_DEBUGCTLMSR)
#define _TIF_DS_AREA_MSR	(1 << TIF_DS_AREA_MSR)
#define _TIF_LAZY_MMU_UPDATES	(1 << TIF_LAZY_MMU_UPDATES)
#define _TIF_SYSCALL_TRACEPOINT	(1 << TIF_SYSCALL_TRACEPOINT)

/* work to do in syscall_trace_enter() */
#define _TIF_WORK_SYSCALL_ENTRY	\
	(_TIF_SYSCALL_TRACE | _TIF_SYSCALL_EMU | _TIF_SYSCALL_AUDIT |	\
	 _TIF_SECCOMP | _TIF_SINGLESTEP | _TIF_SYSCALL_TRACEPOINT)

/* work to do in syscall_trace_leave() */
#define _TIF_WORK_SYSCALL_EXIT	\
	(_TIF_SYSCALL_TRACE | _TIF_SYSCALL_AUDIT | _TIF_SINGLESTEP |	\
	 _TIF_SYSCALL_TRACEPOINT)

/* work to do on interrupt/exception return */
#define _TIF_WORK_MASK							\
	(0x0000FFFF &							\
	 ~(_TIF_SYSCALL_TRACE|_TIF_SYSCALL_AUDIT|			\
	   _TIF_SINGLESTEP|_TIF_SECCOMP|_TIF_SYSCALL_EMU))

/* work to do on any return to user space */
#define _TIF_ALLWORK_MASK						\
	((0x0000FFFF & ~_TIF_SECCOMP) | _TIF_SYSCALL_TRACEPOINT)

/* Only used for 64 bit */
#define _TIF_DO_NOTIFY_MASK						\
	(_TIF_SIGPENDING|_TIF_MCE_NOTIFY|_TIF_NOTIFY_RESUME)

/* flags to check in __switch_to() */
#define _TIF_WORK_CTXSW							\
	(_TIF_IO_BITMAP|_TIF_DEBUGCTLMSR|_TIF_DS_AREA_MSR|_TIF_NOTSC)

#define _TIF_WORK_CTXSW_PREV _TIF_WORK_CTXSW
#define _TIF_WORK_CTXSW_NEXT (_TIF_WORK_CTXSW|_TIF_DEBUG)

#define PREEMPT_ACTIVE		0x10000000

/* thread information allocation */
#ifdef CONFIG_DEBUG_STACK_USAGE
#define THREAD_FLAGS (GFP_KERNEL | __GFP_NOTRACK | __GFP_ZERO)
#else
#define THREAD_FLAGS (GFP_KERNEL | __GFP_NOTRACK)
#endif

#define __HAVE_ARCH_THREAD_INFO_ALLOCATOR

#define alloc_thread_info(tsk)						\
	((struct thread_info *)__get_free_pages(THREAD_FLAGS, THREAD_ORDER))

#ifdef CONFIG_X86_32

#define STACK_WARN	(THREAD_SIZE/8)
/*
 * macros/functions for gaining access to the thread information structure
 *
 * preempt_count needs to be 1 initially, until the scheduler is functional.
 */
#ifndef __ASSEMBLY__


/* how to get the current stack pointer from C */
// 堆栈指针ESP
register unsigned long current_stack_pointer asm("esp") __used;

/* how to get the thread information struct from C */
static inline struct thread_info *current_thread_info(void)
{
   // THREAD_SIZE是8Kbytes的情况下~(THREAD_SIZE-1)=11...110 0000 0000 0000
   // 也就是低13位是0
   // 这里把ESP的低13位清零即可得到当前进程的thread_info指针
	return (struct thread_info *)
		(current_stack_pointer & ~(THREAD_SIZE - 1));
}

#else /* !__ASSEMBLY__ */

/* how to get the thread information struct from ASM */
#define GET_THREAD_INFO(reg)	 \
	movl $-THREAD_SIZE, reg; \
	andl %esp, reg

/* use this one if reg already contains %esp */
#define GET_THREAD_INFO_WITH_ESP(reg) \
	andl $-THREAD_SIZE, reg

#endif

#else /* X86_32 */

#include <asm/percpu.h>
#define KERNEL_STACK_OFFSET (5*8)

/*
 * macros/functions for gaining access to the thread information structure
 * preempt_count needs to be 1 initially, until the scheduler is functional.
 */
#ifndef __ASSEMBLY__
DECLARE_PER_CPU(unsigned long, kernel_stack);

static inline struct thread_info *current_thread_info(void)
{
	struct thread_info *ti;
	ti = (void *)(percpu_read_stable(kernel_stack) +
		      KERNEL_STACK_OFFSET - THREAD_SIZE);
	return ti;
}

#else /* !__ASSEMBLY__ */

/* how to get the thread information struct from ASM */
#define GET_THREAD_INFO(reg) \
	movq PER_CPU_VAR(kernel_stack),reg ; \
	subq $(THREAD_SIZE-KERNEL_STACK_OFFSET),reg

#endif

#endif /* !X86_32 */

/*
 * Thread-synchronous status.
 *
 * This is different from the flags in that nobody else
 * ever touches our thread-synchronous status, so we don't
 * have to worry about atomic accesses.
 */
#define TS_USEDFPU		0x0001	/* FPU was used by this task
					   this quantum (SMP) */
#define TS_COMPAT		0x0002	/* 32bit syscall active (64BIT)*/
#define TS_POLLING		0x0004	/* true if in idle loop
					   and not sleeping */
#define TS_RESTORE_SIGMASK	0x0008	/* restore signal mask in do_signal() */
#define TS_XSAVE		0x0010	/* Use xsave/xrstor */

#define tsk_is_polling(t) (task_thread_info(t)->status & TS_POLLING)

#ifndef __ASSEMBLY__
#define HAVE_SET_RESTORE_SIGMASK	1
static inline void set_restore_sigmask(void)
{
	struct thread_info *ti = current_thread_info();
	ti->status |= TS_RESTORE_SIGMASK;
	set_bit(TIF_SIGPENDING, (unsigned long *)&ti->flags);
}
#endif	/* !__ASSEMBLY__ */

#ifndef __ASSEMBLY__
extern void arch_task_cache_init(void);
extern void free_thread_info(struct thread_info *ti);
extern int arch_dup_task_struct(struct task_struct *dst, struct task_struct *src);
#define arch_task_cache_init arch_task_cache_init
#endif
#endif /* _ASM_X86_THREAD_INFO_H */
