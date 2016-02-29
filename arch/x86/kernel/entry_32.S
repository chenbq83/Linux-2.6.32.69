/*
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * entry.S contains the system-call and fault low-level handling routines.
 * This also contains the timer-interrupt handler, as well as all interrupts
 * and faults that can result in a task-switch.
 *
 * NOTE: This code handles signal-recognition, which happens every time
 * after a timer-interrupt and after each system call.
 *
 * I changed all the .align's to 4 (16 byte alignment), as that's faster
 * on a 486.
 *
 * Stack layout in 'syscall_exit':
 * 	ptrace needs to have all regs on the stack.
 *	if the order here is changed, it needs to be
 *	updated in fork.c:copy_process, signal.c:do_signal,
 *	ptrace.c and ptrace.h
 *
 * 相关寄存器与堆栈指针（ESP）的相对位置
 * 寄存器的偏移位置与struct pt_regs中的成员一致
 *	 0(%esp) - %ebx
 *	 4(%esp) - %ecx
 *	 8(%esp) - %edx
 *  C(%esp) - %esi
 *	10(%esp) - %edi
 *	14(%esp) - %ebp
 *	18(%esp) - %eax
 *	1C(%esp) - %ds
 *	20(%esp) - %es
 *	24(%esp) - %fs
 *	28(%esp) - %gs		saved iff !CONFIG_X86_32_LAZY_GS
 *	2C(%esp) - orig_eax
 *	30(%esp) - %eip
 *	34(%esp) - %cs    用户空间的CS:EIP，构成了返回到用户态的地址
 *	38(%esp) - %eflags
 *	3C(%esp) - %oldesp
 *	40(%esp) - %oldss 用户栈的SS:ESP
 *
 * "current" is in register %ebx during any slow entries.
 */

#include <linux/linkage.h>
#include <asm/thread_info.h>
#include <asm/irqflags.h>
#include <asm/errno.h>
#include <asm/segment.h>
#include <asm/smp.h>
#include <asm/page_types.h>
#include <asm/percpu.h>
#include <asm/dwarf2.h>
#include <asm/processor-flags.h>
#include <asm/ftrace.h>
#include <asm/irq_vectors.h>

/* Avoid __ASSEMBLER__'ifying <linux/audit.h> just for this.  */
#include <linux/elf-em.h>
#define AUDIT_ARCH_I386		(EM_386|__AUDIT_ARCH_LE)
#define __AUDIT_ARCH_LE	   0x40000000

#ifndef CONFIG_AUDITSYSCALL
#define sysenter_audit	syscall_trace_entry
#define sysexit_audit	syscall_exit_work
#endif

/*
 * We use macros for low-level operations which need to be overridden
 * for paravirtualization.  The following will never clobber any registers:
 *   INTERRUPT_RETURN (aka. "iret")
 *   GET_CR0_INTO_EAX (aka. "movl %cr0, %eax")
 *   ENABLE_INTERRUPTS_SYSEXIT (aka "sti; sysexit").
 *
 * For DISABLE_INTERRUPTS/ENABLE_INTERRUPTS (aka "cli"/"sti"), you must
 * specify what registers can be overwritten (CLBR_NONE, CLBR_EAX/EDX/ECX/ANY).
 * Allowing a register to be clobbered can shrink the paravirt replacement
 * enough to patch inline, increasing performance.
 */

#define nr_syscalls ((syscall_table_size)/4)

#ifdef CONFIG_PREEMPT
#define preempt_stop(clobbers)	DISABLE_INTERRUPTS(clobbers); TRACE_IRQS_OFF
#else
#define preempt_stop(clobbers)
#define resume_kernel		restore_all
#endif

.macro TRACE_IRQS_IRET
#ifdef CONFIG_TRACE_IRQFLAGS
	testl $X86_EFLAGS_IF,PT_EFLAGS(%esp)     # interrupts off?
	jz 1f
	TRACE_IRQS_ON
1:
#endif
.endm

#ifdef CONFIG_VM86
#define resume_userspace_sig	check_userspace
#else
#define resume_userspace_sig	resume_userspace
#endif

/*
 * User gs save/restore
 *
 * %gs is used for userland TLS and kernel only uses it for stack
 * canary which is required to be at %gs:20 by gcc.  Read the comment
 * at the top of stackprotector.h for more info.
 *
 * Local labels 98 and 99 are used.
 */
#ifdef CONFIG_X86_32_LAZY_GS

 /* unfortunately push/pop can't be no-op */
.macro PUSH_GS
	pushl $0
	CFI_ADJUST_CFA_OFFSET 4
.endm
.macro POP_GS pop=0
	addl $(4 + \pop), %esp
	CFI_ADJUST_CFA_OFFSET -(4 + \pop)
.endm
.macro POP_GS_EX
.endm

 /* all the rest are no-op */
.macro PTGS_TO_GS
.endm
.macro PTGS_TO_GS_EX
.endm
.macro GS_TO_REG reg
.endm
.macro REG_TO_PTGS reg
.endm
.macro SET_KERNEL_GS reg
.endm

#else	/* CONFIG_X86_32_LAZY_GS */

.macro PUSH_GS
	pushl %gs
	CFI_ADJUST_CFA_OFFSET 4
	/*CFI_REL_OFFSET gs, 0*/
.endm

.macro POP_GS pop=0
98:	popl %gs
	CFI_ADJUST_CFA_OFFSET -4
	/*CFI_RESTORE gs*/
  .if \pop <> 0
	add $\pop, %esp
	CFI_ADJUST_CFA_OFFSET -\pop
  .endif
.endm
.macro POP_GS_EX
.pushsection .fixup, "ax"
99:	movl $0, (%esp)
	jmp 98b
.section __ex_table, "a"
	.align 4
	.long 98b, 99b
.popsection
.endm

.macro PTGS_TO_GS
98:	mov PT_GS(%esp), %gs
.endm
.macro PTGS_TO_GS_EX
.pushsection .fixup, "ax"
99:	movl $0, PT_GS(%esp)
	jmp 98b
.section __ex_table, "a"
	.align 4
	.long 98b, 99b
.popsection
.endm

.macro GS_TO_REG reg
	movl %gs, \reg
	/*CFI_REGISTER gs, \reg*/
.endm
.macro REG_TO_PTGS reg
	movl \reg, PT_GS(%esp)
	/*CFI_REL_OFFSET gs, PT_GS*/
.endm
.macro SET_KERNEL_GS reg
	movl $(__KERNEL_STACK_CANARY), \reg
	movl \reg, %gs
.endm

#endif	/* CONFIG_X86_32_LAZY_GS */

/*
 * SAVE_ALL可以在栈中保存中断处理程序可能会用到的所有CPU寄存器，
 * 但是EFLAGS、CS、EIP、SS和ESP除外，
 * 因为这些寄存器已经由控制单元自动保存了
 * 然后把用户数据段的选择符装载到DS和ES寄存器
 */
.macro SAVE_ALL
	cld
	PUSH_GS
	pushl %fs
	CFI_ADJUST_CFA_OFFSET 4
	/*CFI_REL_OFFSET fs, 0;*/
	pushl %es
	CFI_ADJUST_CFA_OFFSET 4
	/*CFI_REL_OFFSET es, 0;*/
	pushl %ds
	CFI_ADJUST_CFA_OFFSET 4
	/*CFI_REL_OFFSET ds, 0;*/
	pushl %eax
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET eax, 0
	pushl %ebp
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET ebp, 0
	pushl %edi
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET edi, 0
	pushl %esi
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET esi, 0
	pushl %edx
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET edx, 0
	pushl %ecx
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET ecx, 0
	pushl %ebx
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET ebx, 0
	movl $(__USER_DS), %edx
	movl %edx, %ds
	movl %edx, %es
	movl $(__KERNEL_PERCPU), %edx
	movl %edx, %fs
	SET_KERNEL_GS %edx
.endm

.macro RESTORE_INT_REGS
	popl %ebx
	CFI_ADJUST_CFA_OFFSET -4
	CFI_RESTORE ebx
	popl %ecx
	CFI_ADJUST_CFA_OFFSET -4
	CFI_RESTORE ecx
	popl %edx
	CFI_ADJUST_CFA_OFFSET -4
	CFI_RESTORE edx
	popl %esi
	CFI_ADJUST_CFA_OFFSET -4
	CFI_RESTORE esi
	popl %edi
	CFI_ADJUST_CFA_OFFSET -4
	CFI_RESTORE edi
	popl %ebp
	CFI_ADJUST_CFA_OFFSET -4
	CFI_RESTORE ebp
	popl %eax
	CFI_ADJUST_CFA_OFFSET -4
	CFI_RESTORE eax
.endm

.macro RESTORE_REGS pop=0
	RESTORE_INT_REGS
1:	popl %ds
	CFI_ADJUST_CFA_OFFSET -4
	/*CFI_RESTORE ds;*/
2:	popl %es
	CFI_ADJUST_CFA_OFFSET -4
	/*CFI_RESTORE es;*/
3:	popl %fs
	CFI_ADJUST_CFA_OFFSET -4
	/*CFI_RESTORE fs;*/
	POP_GS \pop
.pushsection .fixup, "ax"
4:	movl $0, (%esp)
	jmp 1b
5:	movl $0, (%esp)
	jmp 2b
6:	movl $0, (%esp)
	jmp 3b
.section __ex_table, "a"
	.align 4
	.long 1b, 4b
	.long 2b, 5b
	.long 3b, 6b
.popsection
	POP_GS_EX
.endm

.macro RING0_INT_FRAME
	CFI_STARTPROC simple
	CFI_SIGNAL_FRAME
	CFI_DEF_CFA esp, 3*4
	/*CFI_OFFSET cs, -2*4;*/
	CFI_OFFSET eip, -3*4
.endm

.macro RING0_EC_FRAME
	CFI_STARTPROC simple
	CFI_SIGNAL_FRAME
	CFI_DEF_CFA esp, 4*4
	/*CFI_OFFSET cs, -2*4;*/
	CFI_OFFSET eip, -3*4
.endm

.macro RING0_PTREGS_FRAME
	CFI_STARTPROC simple
	CFI_SIGNAL_FRAME
	CFI_DEF_CFA esp, PT_OLDESP-PT_EBX
	/*CFI_OFFSET cs, PT_CS-PT_OLDESP;*/
	CFI_OFFSET eip, PT_EIP-PT_OLDESP
	/*CFI_OFFSET es, PT_ES-PT_OLDESP;*/
	/*CFI_OFFSET ds, PT_DS-PT_OLDESP;*/
	CFI_OFFSET eax, PT_EAX-PT_OLDESP
	CFI_OFFSET ebp, PT_EBP-PT_OLDESP
	CFI_OFFSET edi, PT_EDI-PT_OLDESP
	CFI_OFFSET esi, PT_ESI-PT_OLDESP
	CFI_OFFSET edx, PT_EDX-PT_OLDESP
	CFI_OFFSET ecx, PT_ECX-PT_OLDESP
	CFI_OFFSET ebx, PT_EBX-PT_OLDESP
.endm

ENTRY(ret_from_fork)
	CFI_STARTPROC
	pushl %eax
	CFI_ADJUST_CFA_OFFSET 4
	call schedule_tail
	GET_THREAD_INFO(%ebp)
	popl %eax
	CFI_ADJUST_CFA_OFFSET -4
	pushl $0x0202			# Reset kernel eflags
	CFI_ADJUST_CFA_OFFSET 4
	popfl
	CFI_ADJUST_CFA_OFFSET -4
	jmp syscall_exit
	CFI_ENDPROC
END(ret_from_fork)

/*
 * Return to user mode is not as complex as all this looks,
 * but we want the default path for a system call return to
 * go as quickly as possible which is why some of this is
 * less clear than it otherwise should be.
 *
 * 当内核从中断返回时，应当考虑以下几种情况：
   （1）内核控制路径并发执行的数量，如果为1，则CPU返回用户态
   （2）挂起进程的切换请求，如果有挂起请求，则进行调度；否则，返回被中断的进程
   （3）待处理信号，如果有信号发送给当前进程，则必须进行信号处理
   （4）单步调试模式，如果调试器正在跟踪当前进程，在返回用户态时必须恢复单步模式
   （5）Virtual-8086模式，如果中断时CPU处于该模式，则进行特殊的处理
 */

	# userspace resumption stub bypassing syscall exit tracing
	ALIGN
	RING0_PTREGS_FRAME
ret_from_exception:
	preempt_stop(CLBR_ANY)
ret_from_intr:
	GET_THREAD_INFO(%ebp)
check_userspace:
	movl PT_EFLAGS(%esp), %eax	# mix EFLAGS and CS
	movb PT_CS(%esp), %al
	andl $(X86_EFLAGS_VM | SEGMENT_RPL_MASK), %eax #是否运行在VM86模式或者用户态
	cmpl $USER_RPL, %eax
	jb resume_kernel		# not returning to v8086 or userspace

/* 返回用户空间，只需要检查need_resched */
/* 当中断或异常发生时，任务处于用户空间 */
ENTRY(resume_userspace)
	LOCKDEP_SYS_EXIT
 	DISABLE_INTERRUPTS(CLBR_ANY)	# make sure we don't miss an interrupt
					# setting need_resched or sigpending
					# between sampling and the iret
	TRACE_IRQS_OFF
	movl TI_flags(%ebp), %ecx
	andl $_TIF_WORK_MASK, %ecx	# is there any work to be done on
					# int/exception return?
	jne work_pending # 还有其他工作要做
	jmp restore_all  # 所有工作都做完，则恢复处理器状态
END(ret_from_exception)

#ifdef CONFIG_PREEMPT
ENTRY(resume_kernel)
	DISABLE_INTERRUPTS(CLBR_ANY)
	cmpl $0,TI_preempt_count(%ebp)	# non-zero preempt_count ?
	jnz restore_all
need_resched:
	movl TI_flags(%ebp), %ecx	# need_resched set ?
	testb $_TIF_NEED_RESCHED, %cl
	jz restore_all
	testl $X86_EFLAGS_IF,PT_EFLAGS(%esp)	# interrupts off (exception path) ?
	jz restore_all
	call preempt_schedule_irq
	jmp need_resched
END(resume_kernel)
#endif
	CFI_ENDPROC

/* SYSENTER_RETURN points to after the "sysenter" instruction in
   the vsyscall page.  See vsyscall-sysentry.S, which defines the symbol.  */

	# sysenter call handler stub
ENTRY(ia32_sysenter_target)
	CFI_STARTPROC simple
	CFI_SIGNAL_FRAME
	CFI_DEF_CFA esp, 0
	CFI_REGISTER esp, ebp
	movl TSS_sysenter_sp0(%esp),%esp
sysenter_past_esp:
	/*
	 * Interrupts are disabled here, but we can't trace it until
	 * enough kernel state to call TRACE_IRQS_OFF can be called - but
	 * we immediately enable interrupts at that point anyway.
	 */
	pushl $(__USER_DS)
	CFI_ADJUST_CFA_OFFSET 4
	/*CFI_REL_OFFSET ss, 0*/
	pushl %ebp
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET esp, 0
	pushfl
	orl $X86_EFLAGS_IF, (%esp)
	CFI_ADJUST_CFA_OFFSET 4
	pushl $(__USER_CS)
	CFI_ADJUST_CFA_OFFSET 4
	/*CFI_REL_OFFSET cs, 0*/
	/*
	 * Push current_thread_info()->sysenter_return to the stack.
	 * A tiny bit of offset fixup is necessary - 4*4 means the 4 words
	 * pushed above; +8 corresponds to copy_thread's esp0 setting.
	 */
	pushl (TI_sysenter_return-THREAD_SIZE+8+4*4)(%esp)
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET eip, 0

	pushl %eax
	CFI_ADJUST_CFA_OFFSET 4
	SAVE_ALL
	ENABLE_INTERRUPTS(CLBR_NONE)

/*
 * Load the potential sixth argument from user stack.
 * Careful about security.
 */
	cmpl $__PAGE_OFFSET-3,%ebp
	jae syscall_fault
1:	movl (%ebp),%ebp
	movl %ebp,PT_EBP(%esp)
.section __ex_table,"a"
	.align 4
	.long 1b,syscall_fault
.previous

	GET_THREAD_INFO(%ebp)

	testl $_TIF_WORK_SYSCALL_ENTRY,TI_flags(%ebp)
	jnz sysenter_audit
sysenter_do_call:
	cmpl $(nr_syscalls), %eax
	jae sysenter_badsys
	call *sys_call_table(,%eax,4)
sysenter_after_call:
	movl %eax,PT_EAX(%esp)
	LOCKDEP_SYS_EXIT
	DISABLE_INTERRUPTS(CLBR_ANY)
	TRACE_IRQS_OFF
	movl TI_flags(%ebp), %ecx
	testl $_TIF_ALLWORK_MASK, %ecx
	jne sysexit_audit
sysenter_exit:
/* if something modifies registers it must also disable sysexit */
	movl PT_EIP(%esp), %edx
	movl PT_OLDESP(%esp), %ecx
	xorl %ebp,%ebp
	TRACE_IRQS_ON
1:	mov  PT_FS(%esp), %fs
	PTGS_TO_GS
	ENABLE_INTERRUPTS_SYSEXIT

#ifdef CONFIG_AUDITSYSCALL
sysenter_audit:
	testl $(_TIF_WORK_SYSCALL_ENTRY & ~_TIF_SYSCALL_AUDIT),TI_flags(%ebp)
	jnz syscall_trace_entry
	addl $4,%esp
	CFI_ADJUST_CFA_OFFSET -4
	/* %esi already in 8(%esp)	   6th arg: 4th syscall arg */
	/* %edx already in 4(%esp)	   5th arg: 3rd syscall arg */
	/* %ecx already in 0(%esp)	   4th arg: 2nd syscall arg */
	movl %ebx,%ecx			/* 3rd arg: 1st syscall arg */
	movl %eax,%edx			/* 2nd arg: syscall number */
	movl $AUDIT_ARCH_I386,%eax	/* 1st arg: audit arch */
	call audit_syscall_entry
	pushl %ebx
	CFI_ADJUST_CFA_OFFSET 4
	movl PT_EAX(%esp),%eax		/* reload syscall number */
	jmp sysenter_do_call

sysexit_audit:
	testl $(_TIF_ALLWORK_MASK & ~_TIF_SYSCALL_AUDIT), %ecx
	jne syscall_exit_work
	TRACE_IRQS_ON
	ENABLE_INTERRUPTS(CLBR_ANY)
	movl %eax,%edx		/* second arg, syscall return value */
	cmpl $0,%eax		/* is it < 0? */
	setl %al		/* 1 if so, 0 if not */
	movzbl %al,%eax		/* zero-extend that */
	inc %eax /* first arg, 0->1(AUDITSC_SUCCESS), 1->2(AUDITSC_FAILURE) */
	call audit_syscall_exit
	DISABLE_INTERRUPTS(CLBR_ANY)
	TRACE_IRQS_OFF
	movl TI_flags(%ebp), %ecx
	testl $(_TIF_ALLWORK_MASK & ~_TIF_SYSCALL_AUDIT), %ecx
	jne syscall_exit_work
	movl PT_EAX(%esp),%eax	/* reload syscall return value */
	jmp sysenter_exit
#endif

	CFI_ENDPROC
.pushsection .fixup,"ax"
2:	movl $0,PT_FS(%esp)
	jmp 1b
.section __ex_table,"a"
	.align 4
	.long 1b,2b
.popsection
	PTGS_TO_GS_EX
ENDPROC(ia32_sysenter_target)

/* 用户程序需要系统提供服务的时候，会通过系统调用产生一个int 0x80的软中断
   就会进入到系统调用的入口函数 */
	# system call handler stub
ENTRY(system_call)
	RING0_INT_FRAME			# can't unwind into user space anyway
	pushl %eax			      # save orig_eax 将系统调用号压入栈中
	CFI_ADJUST_CFA_OFFSET 4
	SAVE_ALL                # 将寄存器的值压入堆栈当中，压入的顺序对应着结构体struct pt_regs
                           # 当出栈的时候，就将这些值传递到结构体struct pt_regs里面的成员
                           # 从而实现了从汇编代码向C程序传递参数
	GET_THREAD_INFO(%ebp)   # 根据ebp寄存器获得当前进程的thread_info结构的地址，从而获得当前进程的信息
					# system call tracing in operation / emulation
	testl $_TIF_WORK_SYSCALL_ENTRY,TI_flags(%ebp)
	jnz syscall_trace_entry # 如果发生被跟踪的情况则转向相应的处理命令处
	cmpl $(nr_syscalls), %eax # 对用户态进程传递过来的系统调用号的合法性进行检查
	jae syscall_badsys
syscall_call:              # 合法则跳转到相应系统调用号所对应的服务例程中
                           # 也就是sys_call_table表中找到相应的函数入口
	call *sys_call_table(,%eax,4) # 每个表项4个字节，表的基地址+%eax*4
syscall_after_call:
	movl %eax,PT_EAX(%esp)		# store the return value 返回值保存到eax
# 系统调用返回
syscall_exit:
	LOCKDEP_SYS_EXIT
	DISABLE_INTERRUPTS(CLBR_ANY)	# make sure we don't miss an interrupt
					# setting need_resched or sigpending
					# between sampling and the iret
	TRACE_IRQS_OFF
	movl TI_flags(%ebp), %ecx
	testl $_TIF_ALLWORK_MASK, %ecx	# current->work 检查是否还有其他工作要完成
	jne syscall_exit_work

restore_all:
	TRACE_IRQS_IRET
restore_all_notrace:
#ifdef CONFIG_X86_ESPFIX32
	movl PT_EFLAGS(%esp), %eax	# mix EFLAGS, SS and CS
	# Warning: PT_OLDSS(%esp) contains the wrong/random values if we
	# are returning to the kernel.
	# See comments in process.c:copy_thread() for details.
	movb PT_OLDSS(%esp), %ah
	movb PT_CS(%esp), %al
	andl $(X86_EFLAGS_VM | (SEGMENT_TI_MASK << 8) | SEGMENT_RPL_MASK), %eax
	cmpl $((SEGMENT_LDT << 8) | USER_RPL), %eax
	CFI_REMEMBER_STATE
	je ldt_ss			# returning to user-space with LDT SS
#endif
restore_nocheck:
	RESTORE_REGS 4			# skip orig_eax/error_code
	CFI_ADJUST_CFA_OFFSET -4
irq_return:
	INTERRUPT_RETURN
.section .fixup,"ax"
ENTRY(iret_exc)
   # 当异常发生时，如果控制单元没有自动把一个硬件出错代码压入栈中，
   # 相应的汇编语言片段就会包含一条pushl $0指令，在栈中垫上一个空值
	pushl $0			# no error code
   # 把C函数的地址压入栈中
	pushl $do_iret_error
	jmp error_code
.previous
.section __ex_table,"a"
	.align 4
	.long irq_return,iret_exc
.previous

#ifdef CONFIG_X86_ESPFIX32
	CFI_RESTORE_STATE
ldt_ss:
#ifdef CONFIG_PARAVIRT
	/*
	 * The kernel can't run on a non-flat stack if paravirt mode
	 * is active.  Rather than try to fixup the high bits of
	 * ESP, bypass this code entirely.  This may break DOSemu
	 * and/or Wine support in a paravirt VM, although the option
	 * is still available to implement the setting of the high
	 * 16-bits in the INTERRUPT_RETURN paravirt-op.
	 */
	cmpl $0, pv_info+PARAVIRT_enabled
	jne restore_nocheck
#endif

/*
 * Setup and switch to ESPFIX stack
 *
 * We're returning to userspace with a 16 bit stack. The CPU will not
 * restore the high word of ESP for us on executing iret... This is an
 * "official" bug of all the x86-compatible CPUs, which we can work
 * around to make dosemu and wine happy. We do this by preloading the
 * high word of ESP with the high word of the userspace ESP while
 * compensating for the offset by changing to the ESPFIX segment with
 * a base address that matches for the difference.
 */
	mov %esp, %edx			/* load kernel esp */
	mov PT_OLDESP(%esp), %eax	/* load userspace esp */
	mov %dx, %ax			/* eax: new kernel esp */
	sub %eax, %edx			/* offset (low word is 0) */
	PER_CPU(gdt_page, %ebx)
	shr $16, %edx
	mov %dl, GDT_ENTRY_ESPFIX_SS * 8 + 4(%ebx) /* bits 16..23 */
	mov %dh, GDT_ENTRY_ESPFIX_SS * 8 + 7(%ebx) /* bits 24..31 */
	pushl $__ESPFIX_SS
	CFI_ADJUST_CFA_OFFSET 4
	push %eax			/* new kernel esp */
	CFI_ADJUST_CFA_OFFSET 4
	/* Disable interrupts, but do not irqtrace this section: we
	 * will soon execute iret and the tracer was already set to
	 * the irqstate after the iret */
	DISABLE_INTERRUPTS(CLBR_EAX)
	lss (%esp), %esp		/* switch to espfix segment */
	CFI_ADJUST_CFA_OFFSET -8
	jmp restore_nocheck
#endif
	CFI_ENDPROC
ENDPROC(system_call)

	# perform work that needs to be done immediately before resumption
	ALIGN
	RING0_PTREGS_FRAME		# can't unwind into user space anyway
work_pending:
	testb $_TIF_NEED_RESCHED, %cl
	jz work_notifysig
work_resched:
	call schedule
	LOCKDEP_SYS_EXIT
	DISABLE_INTERRUPTS(CLBR_ANY)	# make sure we don't miss an interrupt
					# setting need_resched or sigpending
					# between sampling and the iret
	TRACE_IRQS_OFF
	movl TI_flags(%ebp), %ecx
	andl $_TIF_WORK_MASK, %ecx	# is there any work to be done other
					# than syscall tracing?
	jz restore_all
	testb $_TIF_NEED_RESCHED, %cl
	jnz work_resched

work_notifysig:				# deal with pending signals and
					# notify-resume requests
#ifdef CONFIG_VM86
	testl $X86_EFLAGS_VM, PT_EFLAGS(%esp)
	movl %esp, %eax
	jne work_notifysig_v86		# returning to kernel-space or
					# vm86-space
	xorl %edx, %edx
	call do_notify_resume
	jmp resume_userspace_sig

	ALIGN
work_notifysig_v86:
	pushl %ecx			# save ti_flags for do_notify_resume
	CFI_ADJUST_CFA_OFFSET 4
	call save_v86_state		# %eax contains pt_regs pointer
	popl %ecx
	CFI_ADJUST_CFA_OFFSET -4
	movl %eax, %esp
#else
	movl %esp, %eax
#endif
	xorl %edx, %edx
	call do_notify_resume
	jmp resume_userspace_sig
END(work_pending)

	# perform syscall exit tracing
	ALIGN
syscall_trace_entry:
	movl $-ENOSYS,PT_EAX(%esp)
	movl %esp, %eax
	call syscall_trace_enter
	/* What it returned is what we'll actually use.  */
	cmpl $(nr_syscalls), %eax
	jnae syscall_call
	jmp syscall_exit
END(syscall_trace_entry)

	# perform syscall exit tracing
	ALIGN
syscall_exit_work:
   # 检查是否系统调用跟踪、审计、单步执行，不需要则跳到work_pending（进行调度，信号处理）
	testl $_TIF_WORK_SYSCALL_EXIT, %ecx
	jz work_pending
	TRACE_IRQS_ON
	ENABLE_INTERRUPTS(CLBR_ANY)	# could let syscall_trace_leave() call
					# schedule() instead
	movl %esp, %eax
	call syscall_trace_leave
	jmp resume_userspace
END(syscall_exit_work)
	CFI_ENDPROC

	RING0_INT_FRAME			# can't unwind into user space anyway
syscall_fault:
	GET_THREAD_INFO(%ebp)
	movl $-EFAULT,PT_EAX(%esp)
	jmp resume_userspace
END(syscall_fault)

syscall_badsys:
	movl $-ENOSYS,%eax
	jmp syscall_after_call
END(syscall_badsys)

sysenter_badsys:
	movl $-ENOSYS,%eax
	jmp sysenter_after_call
END(sysenter_badsys)
	CFI_ENDPROC

/*
 * System calls that need a pt_regs pointer.
 */
#define PTREGSCALL(name) \
	ALIGN; \
ptregs_##name: \
	leal 4(%esp),%eax; \
	jmp sys_##name;

PTREGSCALL(iopl)
PTREGSCALL(fork)
PTREGSCALL(clone)
PTREGSCALL(vfork)
PTREGSCALL(execve)
PTREGSCALL(sigaltstack)
PTREGSCALL(sigreturn)
PTREGSCALL(rt_sigreturn)
PTREGSCALL(vm86)
PTREGSCALL(vm86old)

.macro FIXUP_ESPFIX_STACK
/*
 * Switch back for ESPFIX stack to the normal zerobased stack
 *
 * We can't call C functions using the ESPFIX stack. This code reads
 * the high word of the segment base from the GDT and swiches to the
 * normal stack and adjusts ESP with the matching offset.
 */
#ifdef CONFIG_X86_ESPFIX32
	/* fixup the stack */
	PER_CPU(gdt_page, %ebx)
	mov GDT_ENTRY_ESPFIX_SS * 8 + 4(%ebx), %al /* bits 16..23 */
	mov GDT_ENTRY_ESPFIX_SS * 8 + 7(%ebx), %ah /* bits 24..31 */
	shl $16, %eax
	addl %esp, %eax			/* the adjusted stack pointer */
	pushl $__KERNEL_DS
	CFI_ADJUST_CFA_OFFSET 4
	pushl %eax
	CFI_ADJUST_CFA_OFFSET 4
	lss (%esp), %esp		/* switch to the normal stack segment */
	CFI_ADJUST_CFA_OFFSET -8
#endif
.endm
.macro UNWIND_ESPFIX_STACK
#ifdef CONFIG_X86_ESPFIX32
	movl %ss, %eax
	/* see if on espfix stack */
	cmpw $__ESPFIX_SS, %ax
	jne 27f
	movl $__KERNEL_DS, %eax
	movl %eax, %ds
	movl %eax, %es
	/* switch to normal stack */
	FIXUP_ESPFIX_STACK
27:
#endif
.endm

/*
 * http://blog.csdn.net/yunsongice/article/details/5306207
 *
 * 当CPU执行了一条指令后，CS:EIP指向了下一条要执行的指令的逻辑地址。
 * 在处理那条指令之前，控制单元会检查运行前一条指令时是否已经发生了一个中断或异常。
 * 如果发生了，执行下列操作：
     1. 确定与中断或异常相关的向量i（0<=i<=255）
     2. 读由idtr指向的IDT表中的第i项（包含的是一个中断门或陷阱门）
     3. 从gdtr获得GDT表的基地址，并在GDT中查找，以读取IDT表项中的选择符所标识的段描述符。
        这个描述符指定中断或异常处理程序所在段的基地址
     4. 确认中断时由授权的中断发生源发出的。
        首先将当前特权级CPL（存放在CS寄存器的低两位）与段描述符（存放在GDT中）的描述符特权级DPL比较。
        如果CPL小于DPL，就产生“General protection”异常，因为中断处理程序的特权不能低于引起中断的程序的特权。
        对于编程异常，则做进一步的检测：比较CPL与处于IDT中的门描述符的DPL。
        这最后一个检查可以避免用户应用程序访问特殊的陷阱门或中断门
     5. 检查特权级是否发生变化，也就是CPL是否不同于DPL
        如果是，控制单元必须开始使用与新的特权级相关的栈。
        通过执行以下步骤：
        a. 读tr寄存器，以访问运行进程的TSS段
        b. 用与新特权级相关的栈段和栈指针的正确值装载SS和ESP寄存器，这些值可以在TSS中找到
        c. 在新的栈中保存SS和ESP以前的值，这些值定义了与旧的特权级相关的栈的逻辑地址
     6. 如果故障已发生，用引起异常的指令地址装载CS和EIP，从而使得这条指令能再次被执行。
     7. 在栈中保存EFLAGS、CS和EIP的内容
     8. 如果异常产生了一个硬件出错码，则将它保存在栈中。
     9. 装载CS和EIP，其值分别是IDT表中第i项门描述符的段选择符和偏移量字段
  * 中断或异常处理完后，相应的处理程序必须产生一条iret指令，把控制器转交给被中断的进程。
    这迫使控制单元：
     1. 用保存在栈中的值装载CS、EIP和EFLAGS
     2. 检查处理程序的CPL是否等于CS中最低两位的值（意味着被中断的进程与处理程序是否在同一特权级）
        如果是，iret终止执行，否则转入下一步。
     3. 从栈中装载SS和ESP，因此，返回到与旧特权级相关的栈。
     4. 检查DS、ES、FS和GS段寄存器的内容。
        如果其中一个寄存器包含的选择符是一个段描述符，并且其DPL值小于CPL，那么清相应段的寄存器。
        这么做是为了禁止用户态的程序利用内核以前所用的段寄存器。
        如果不清这些寄存器，恶意的用户态程序可能利用它们来访问内核空间地址。
 */

/*
 * http://blog.csdn.net/phenix_lord/article/details/45116689
 *
 * 在内核代码中，x86平台中断的执行过程：
 * 1. 通过IDT中的中断描述符，调用common_interrupt
 * 2. 通过common_interrupt，调用do_IRQ，完成vector到irq_desc的转换，进入Generic interrupt layer
 *    调用处理函数generic_handle_irq_desc
 * 3. 调用在中断初始化的时候按照中断特性（level触发，edge触发，simple等）初始化的irq_desc::handle_irq，
 *    执行不同的通用处理接口
 * 4. 这些通用处理接口会调用中断初始化的时候注册的外部中断处理函数，完成EOI等硬件相关操作，
 *    并完成中断处理的相关控制
 */
/*
 * Build the entry stubs and pointer table with some assembler magic.
 * We pack 7 stubs into a single 32-byte chunk, which will fit in a
 * single cache line on all modern x86 implementations.
 */
.section .init.rodata,"a"
ENTRY(interrupt)
.text
	.p2align 5
	.p2align CONFIG_X86_L1_CACHE_SHIFT
ENTRY(irq_entries_start)
	RING0_INT_FRAME
vector=FIRST_EXTERNAL_VECTOR
.rept (NR_VECTORS-FIRST_EXTERNAL_VECTOR+6)/7
	.balign 32
  .rept	7
    .if vector < NR_VECTORS
      .if vector <> FIRST_EXTERNAL_VECTOR
	CFI_ADJUST_CFA_OFFSET -4
      .endif
1:	pushl $(~vector+0x80)	/* Note: always in signed byte range */
	CFI_ADJUST_CFA_OFFSET 4
      .if ((vector-FIRST_EXTERNAL_VECTOR)%7) <> 6
	jmp 2f
      .endif
      .previous
	.long 1b
      .text
vector=vector+1
    .endif
  .endr
2:	jmp common_interrupt
.endr
END(irq_entries_start)

.previous
END(interrupt)
.previous

/*
 * the CPU automatically disables interrupts when executing an IRQ vector,
 * so IRQ-flags tracing has to follow that:
 */
	.p2align CONFIG_X86_L1_CACHE_SHIFT
common_interrupt:
	addl $-0x80,(%esp)	/* Adjust vector into the [-256,-1] range */
	SAVE_ALL
	TRACE_IRQS_OFF
   /* 保存寄存器以后，栈顶的地址被存放到EAX中，
    * 然后调用do_IRQ函数。到执行到do_IRQ的ret指令时，控制转到ret_from_intr()（从中断和异常返回）
    */
	movl %esp,%eax
	call do_IRQ
	jmp ret_from_intr
ENDPROC(common_interrupt)
	CFI_ENDPROC

#define BUILD_INTERRUPT3(name, nr, fn)	\
ENTRY(name)				\
	RING0_INT_FRAME;		\
	pushl $~(nr);			\
	CFI_ADJUST_CFA_OFFSET 4;	\
	SAVE_ALL;			\
	TRACE_IRQS_OFF			\
	movl %esp,%eax;			\
	call fn;			\
	jmp ret_from_intr;		\
	CFI_ENDPROC;			\
ENDPROC(name)

#define BUILD_INTERRUPT(name, nr)	BUILD_INTERRUPT3(name, nr, smp_##name)

/* The include is where all of the SMP etc. interrupts come from */
#include <asm/entry_arch.h>

ENTRY(coprocessor_error)
	RING0_INT_FRAME
	pushl $0
	CFI_ADJUST_CFA_OFFSET 4
	pushl $do_coprocessor_error
	CFI_ADJUST_CFA_OFFSET 4
	jmp error_code
	CFI_ENDPROC
END(coprocessor_error)

ENTRY(simd_coprocessor_error)
	RING0_INT_FRAME
	pushl $0
	CFI_ADJUST_CFA_OFFSET 4
	pushl $do_simd_coprocessor_error
	CFI_ADJUST_CFA_OFFSET 4
	jmp error_code
	CFI_ENDPROC
END(simd_coprocessor_error)

ENTRY(device_not_available)
	RING0_INT_FRAME
	pushl $-1			# mark this as an int
	CFI_ADJUST_CFA_OFFSET 4
	pushl $do_device_not_available
	CFI_ADJUST_CFA_OFFSET 4
	jmp error_code
	CFI_ENDPROC
END(device_not_available)

#ifdef CONFIG_PARAVIRT
ENTRY(native_iret)
	iret
.section __ex_table,"a"
	.align 4
	.long native_iret, iret_exc
.previous
END(native_iret)

ENTRY(native_irq_enable_sysexit)
	sti
	sysexit
END(native_irq_enable_sysexit)
#endif

ENTRY(overflow)
	RING0_INT_FRAME
	pushl $0
	CFI_ADJUST_CFA_OFFSET 4
	pushl $do_overflow
	CFI_ADJUST_CFA_OFFSET 4
	jmp error_code
	CFI_ENDPROC
END(overflow)

ENTRY(bounds)
	RING0_INT_FRAME
	pushl $0
	CFI_ADJUST_CFA_OFFSET 4
	pushl $do_bounds
	CFI_ADJUST_CFA_OFFSET 4
	jmp error_code
	CFI_ENDPROC
END(bounds)

ENTRY(invalid_op)
	RING0_INT_FRAME
	pushl $0
	CFI_ADJUST_CFA_OFFSET 4
	pushl $do_invalid_op
	CFI_ADJUST_CFA_OFFSET 4
	jmp error_code
	CFI_ENDPROC
END(invalid_op)

ENTRY(coprocessor_segment_overrun)
	RING0_INT_FRAME
	pushl $0
	CFI_ADJUST_CFA_OFFSET 4
	pushl $do_coprocessor_segment_overrun
	CFI_ADJUST_CFA_OFFSET 4
	jmp error_code
	CFI_ENDPROC
END(coprocessor_segment_overrun)

ENTRY(invalid_TSS)
	RING0_EC_FRAME
	pushl $do_invalid_TSS
	CFI_ADJUST_CFA_OFFSET 4
	jmp error_code
	CFI_ENDPROC
END(invalid_TSS)

ENTRY(segment_not_present)
	RING0_EC_FRAME
	pushl $do_segment_not_present
	CFI_ADJUST_CFA_OFFSET 4
	jmp error_code
	CFI_ENDPROC
END(segment_not_present)

ENTRY(stack_segment)
	RING0_EC_FRAME
	pushl $do_stack_segment
	CFI_ADJUST_CFA_OFFSET 4
	jmp error_code
	CFI_ENDPROC
END(stack_segment)

ENTRY(alignment_check)
	RING0_EC_FRAME
	pushl $do_alignment_check
	CFI_ADJUST_CFA_OFFSET 4
	jmp error_code
	CFI_ENDPROC
END(alignment_check)

ENTRY(divide_error)
	RING0_INT_FRAME
   # 当异常发生时，如果控制单元没有自动把一个硬件错误代码插入到栈中，
   # 相应的汇编语言就会包含一条pushl $0的指令，在栈中（内核栈）垫上一个空值
	pushl $0			# no error code
	CFI_ADJUST_CFA_OFFSET 4
   # 把高级C函数的地址压入栈中
	pushl $do_divide_error
	CFI_ADJUST_CFA_OFFSET 4
	jmp error_code
	CFI_ENDPROC
END(divide_error)

#ifdef CONFIG_X86_MCE
ENTRY(machine_check)
	RING0_INT_FRAME
	pushl $0
	CFI_ADJUST_CFA_OFFSET 4
	pushl machine_check_vector
	CFI_ADJUST_CFA_OFFSET 4
	jmp error_code
	CFI_ENDPROC
END(machine_check)
#endif

ENTRY(spurious_interrupt_bug)
	RING0_INT_FRAME
	pushl $0
	CFI_ADJUST_CFA_OFFSET 4
	pushl $do_spurious_interrupt_bug
	CFI_ADJUST_CFA_OFFSET 4
	jmp error_code
	CFI_ENDPROC
END(spurious_interrupt_bug)

ENTRY(kernel_thread_helper)
	pushl $0		# fake return address for unwinder
	CFI_STARTPROC
	movl %edx,%eax
	push %edx
	CFI_ADJUST_CFA_OFFSET 4
	call *%ebx
	push %eax
	CFI_ADJUST_CFA_OFFSET 4
	call do_exit
	ud2			# padding for call trace
	CFI_ENDPROC
ENDPROC(kernel_thread_helper)

#ifdef CONFIG_XEN
/* Xen doesn't set %esp to be precisely what the normal sysenter
   entrypoint expects, so fix it up before using the normal path. */
ENTRY(xen_sysenter_target)
	RING0_INT_FRAME
	addl $5*4, %esp		/* remove xen-provided frame */
	CFI_ADJUST_CFA_OFFSET -5*4
	jmp sysenter_past_esp
	CFI_ENDPROC

ENTRY(xen_hypervisor_callback)
	CFI_STARTPROC
	pushl $0
	CFI_ADJUST_CFA_OFFSET 4
	SAVE_ALL
	TRACE_IRQS_OFF

	/* Check to see if we got the event in the critical
	   region in xen_iret_direct, after we've reenabled
	   events and checked for pending events.  This simulates
	   iret instruction's behaviour where it delivers a
	   pending interrupt when enabling interrupts. */
	movl PT_EIP(%esp),%eax
	cmpl $xen_iret_start_crit,%eax
	jb   1f
	cmpl $xen_iret_end_crit,%eax
	jae  1f

	jmp  xen_iret_crit_fixup

ENTRY(xen_do_upcall)
1:	mov %esp, %eax
	call xen_evtchn_do_upcall
	jmp  ret_from_intr
	CFI_ENDPROC
ENDPROC(xen_hypervisor_callback)

# Hypervisor uses this for application faults while it executes.
# We get here for two reasons:
#  1. Fault while reloading DS, ES, FS or GS
#  2. Fault while executing IRET
# Category 1 we fix up by reattempting the load, and zeroing the segment
# register if the load fails.
# Category 2 we fix up by jumping to do_iret_error. We cannot use the
# normal Linux return path in this case because if we use the IRET hypercall
# to pop the stack frame we end up in an infinite loop of failsafe callbacks.
# We distinguish between categories by maintaining a status value in EAX.
ENTRY(xen_failsafe_callback)
	CFI_STARTPROC
	pushl %eax
	CFI_ADJUST_CFA_OFFSET 4
	movl $1,%eax
1:	mov 4(%esp),%ds
2:	mov 8(%esp),%es
3:	mov 12(%esp),%fs
4:	mov 16(%esp),%gs
	testl %eax,%eax
	popl %eax
	CFI_ADJUST_CFA_OFFSET -4
	lea 16(%esp),%esp
	CFI_ADJUST_CFA_OFFSET -16
	jz 5f
	addl $16,%esp
	jmp iret_exc		# EAX != 0 => Category 2 (Bad IRET)
5:	pushl $0		# EAX == 0 => Category 1 (Bad segment)
	CFI_ADJUST_CFA_OFFSET 4
	SAVE_ALL
	jmp ret_from_exception
	CFI_ENDPROC

.section .fixup,"ax"
6:	xorl %eax,%eax
	movl %eax,4(%esp)
	jmp 1b
7:	xorl %eax,%eax
	movl %eax,8(%esp)
	jmp 2b
8:	xorl %eax,%eax
	movl %eax,12(%esp)
	jmp 3b
9:	xorl %eax,%eax
	movl %eax,16(%esp)
	jmp 4b
.previous
.section __ex_table,"a"
	.align 4
	.long 1b,6b
	.long 2b,7b
	.long 3b,8b
	.long 4b,9b
.previous
ENDPROC(xen_failsafe_callback)

#endif	/* CONFIG_XEN */

#ifdef CONFIG_FUNCTION_TRACER
#ifdef CONFIG_DYNAMIC_FTRACE

ENTRY(mcount)
	ret
END(mcount)

ENTRY(ftrace_caller)
	cmpl $0, function_trace_stop
	jne  ftrace_stub

	pushl %eax
	pushl %ecx
	pushl %edx
	movl 0xc(%esp), %eax
	movl 0x4(%ebp), %edx
	subl $MCOUNT_INSN_SIZE, %eax

.globl ftrace_call
ftrace_call:
	call ftrace_stub

	popl %edx
	popl %ecx
	popl %eax
#ifdef CONFIG_FUNCTION_GRAPH_TRACER
.globl ftrace_graph_call
ftrace_graph_call:
	jmp ftrace_stub
#endif

.globl ftrace_stub
ftrace_stub:
	ret
END(ftrace_caller)

#else /* ! CONFIG_DYNAMIC_FTRACE */

ENTRY(mcount)
	cmpl $0, function_trace_stop
	jne  ftrace_stub

	cmpl $ftrace_stub, ftrace_trace_function
	jnz trace
#ifdef CONFIG_FUNCTION_GRAPH_TRACER
	cmpl $ftrace_stub, ftrace_graph_return
	jnz ftrace_graph_caller

	cmpl $ftrace_graph_entry_stub, ftrace_graph_entry
	jnz ftrace_graph_caller
#endif
.globl ftrace_stub
ftrace_stub:
	ret

	/* taken from glibc */
trace:
	pushl %eax
	pushl %ecx
	pushl %edx
	movl 0xc(%esp), %eax
	movl 0x4(%ebp), %edx
	subl $MCOUNT_INSN_SIZE, %eax

	call *ftrace_trace_function

	popl %edx
	popl %ecx
	popl %eax
	jmp ftrace_stub
END(mcount)
#endif /* CONFIG_DYNAMIC_FTRACE */
#endif /* CONFIG_FUNCTION_TRACER */

#ifdef CONFIG_FUNCTION_GRAPH_TRACER
ENTRY(ftrace_graph_caller)
	cmpl $0, function_trace_stop
	jne ftrace_stub

	pushl %eax
	pushl %ecx
	pushl %edx
	movl 0xc(%esp), %edx
	lea 0x4(%ebp), %eax
	movl (%ebp), %ecx
	subl $MCOUNT_INSN_SIZE, %edx
	call prepare_ftrace_return
	popl %edx
	popl %ecx
	popl %eax
	ret
END(ftrace_graph_caller)

.globl return_to_handler
return_to_handler:
	pushl $0
	pushl %eax
	pushl %ecx
	pushl %edx
	movl %ebp, %eax
	call ftrace_return_to_handler
	movl %eax, 0xc(%esp)
	popl %edx
	popl %ecx
	popl %eax
	ret
#endif

.section .rodata,"a"
#include "syscall_table_32.S"

syscall_table_size=(.-sys_call_table)

/*
 * Some functions should be protected against kprobes
 */
	.pushsection .kprobes.text, "ax"

ENTRY(page_fault)
	RING0_EC_FRAME
	pushl $do_page_fault
	CFI_ADJUST_CFA_OFFSET 4
	ALIGN
/*
 * http://blog.csdn.net/yunsongice/article/details/5306406
 *
 * 标号error_code的汇编语言片段对所有的异常处理程序都是相同的，除了“Device not available”这一个异常。
 * 这段代码执行下列步骤：
     1. 把高级C函数可能用到的寄存器保存在栈中
     2. 产生一条cld指令来清EFLAGS的方向标志DF，确保调用字符串指令时会自动增加EDI和ESI的值
     3. 把栈中位于ESP+36的硬件出错码拷贝到EDX中，给栈中这一位置存上-1，这个值用来把0x80异常与其他异常隔离开
     4. 把栈中位于ESP+32的do_handler_name高级C函数的地址装入EDI，然后在栈的这个位置写入ES的值
     5. 把内核栈的当前栈顶拷贝到EAX。
        这个地址表示内存单元的地址，在这个单元中存放的是第1步所保存的最后一个寄存器的值
     6. 把用户数据段的选择符拷贝到DS和ES
     7. 调用地址在EDI的高级C函数
        被调用的函数从EAX和EDX寄存器而不是从栈中接收参数。
        执行进程切换的主要函数__switch_to()就是一个从CPU寄存器获取参数的函数
 */
error_code:
	/* the function address is in %gs's slot on the stack */
	pushl %fs
	CFI_ADJUST_CFA_OFFSET 4
	/*CFI_REL_OFFSET fs, 0*/
	pushl %es
	CFI_ADJUST_CFA_OFFSET 4
	/*CFI_REL_OFFSET es, 0*/
	pushl %ds
	CFI_ADJUST_CFA_OFFSET 4
	/*CFI_REL_OFFSET ds, 0*/
	pushl %eax
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET eax, 0
	pushl %ebp
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET ebp, 0
	pushl %edi
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET edi, 0
	pushl %esi
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET esi, 0
	pushl %edx
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET edx, 0
	pushl %ecx
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET ecx, 0
	pushl %ebx
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET ebx, 0
	cld
	movl $(__KERNEL_PERCPU), %ecx
	movl %ecx, %fs
	UNWIND_ESPFIX_STACK
	GS_TO_REG %ecx
	movl PT_GS(%esp), %edi		# get the function address
	movl PT_ORIG_EAX(%esp), %edx	# get the error code
	movl $-1, PT_ORIG_EAX(%esp)	# no syscall to restart
	REG_TO_PTGS %ecx
	SET_KERNEL_GS %ecx
	movl $(__USER_DS), %ecx
	movl %ecx, %ds
	movl %ecx, %es
	TRACE_IRQS_OFF
	movl %esp,%eax			# pt_regs pointer
	call *%edi
	jmp ret_from_exception
	CFI_ENDPROC
END(page_fault)

/*
 * Debug traps and NMI can happen at the one SYSENTER instruction
 * that sets up the real kernel stack. Check here, since we can't
 * allow the wrong stack to be used.
 *
 * "TSS_sysenter_sp0+12" is because the NMI/debug handler will have
 * already pushed 3 words if it hits on the sysenter instruction:
 * eflags, cs and eip.
 *
 * We just load the right stack, and push the three (known) values
 * by hand onto the new stack - while updating the return eip past
 * the instruction that would have done it for sysenter.
 */
.macro FIX_STACK offset ok label
	cmpw $__KERNEL_CS, 4(%esp)
	jne \ok
\label:
	movl TSS_sysenter_sp0 + \offset(%esp), %esp
	CFI_DEF_CFA esp, 0
	CFI_UNDEFINED eip
	pushfl
	CFI_ADJUST_CFA_OFFSET 4
	pushl $__KERNEL_CS
	CFI_ADJUST_CFA_OFFSET 4
	pushl $sysenter_past_esp
	CFI_ADJUST_CFA_OFFSET 4
	CFI_REL_OFFSET eip, 0
.endm

ENTRY(debug)
	RING0_INT_FRAME
	cmpl $ia32_sysenter_target,(%esp)
	jne debug_stack_correct
	FIX_STACK 12, debug_stack_correct, debug_esp_fix_insn
debug_stack_correct:
	pushl $-1			# mark this as an int
	CFI_ADJUST_CFA_OFFSET 4
	SAVE_ALL
	TRACE_IRQS_OFF
	xorl %edx,%edx			# error code 0
	movl %esp,%eax			# pt_regs pointer
	call do_debug
	jmp ret_from_exception
	CFI_ENDPROC
END(debug)

/*
 * NMI is doubly nasty. It can happen _while_ we're handling
 * a debug fault, and the debug fault hasn't yet been able to
 * clear up the stack. So we first check whether we got  an
 * NMI on the sysenter entry path, but after that we need to
 * check whether we got an NMI on the debug path where the debug
 * fault happened on the sysenter path.
 */
ENTRY(nmi)
	RING0_INT_FRAME
#ifdef CONFIG_X86_ESPFIX32
	pushl %eax
	CFI_ADJUST_CFA_OFFSET 4
	movl %ss, %eax
	cmpw $__ESPFIX_SS, %ax
	popl %eax
	CFI_ADJUST_CFA_OFFSET -4
	je nmi_espfix_stack
#endif
	cmpl $ia32_sysenter_target,(%esp)
	je nmi_stack_fixup
	pushl %eax
	CFI_ADJUST_CFA_OFFSET 4
	movl %esp,%eax
	/* Do not access memory above the end of our stack page,
	 * it might not exist.
	 */
	andl $(THREAD_SIZE-1),%eax
	cmpl $(THREAD_SIZE-20),%eax
	popl %eax
	CFI_ADJUST_CFA_OFFSET -4
	jae nmi_stack_correct
	cmpl $ia32_sysenter_target,12(%esp)
	je nmi_debug_stack_check
nmi_stack_correct:
	/* We have a RING0_INT_FRAME here */
	pushl %eax
	CFI_ADJUST_CFA_OFFSET 4
	SAVE_ALL
	xorl %edx,%edx		# zero error code
	movl %esp,%eax		# pt_regs pointer
	call do_nmi
	jmp restore_all_notrace
	CFI_ENDPROC

nmi_stack_fixup:
	RING0_INT_FRAME
	FIX_STACK 12, nmi_stack_correct, 1
	jmp nmi_stack_correct

nmi_debug_stack_check:
	/* We have a RING0_INT_FRAME here */
	cmpw $__KERNEL_CS,16(%esp)
	jne nmi_stack_correct
	cmpl $debug,(%esp)
	jb nmi_stack_correct
	cmpl $debug_esp_fix_insn,(%esp)
	ja nmi_stack_correct
	FIX_STACK 24, nmi_stack_correct, 1
	jmp nmi_stack_correct

#ifdef CONFIG_X86_ESPFIX32
nmi_espfix_stack:
	/* We have a RING0_INT_FRAME here.
	 *
	 * create the pointer to lss back
	 */
	pushl %ss
	CFI_ADJUST_CFA_OFFSET 4
	pushl %esp
	CFI_ADJUST_CFA_OFFSET 4
	addl $4, (%esp)
	/* copy the iret frame of 12 bytes */
	.rept 3
	pushl 16(%esp)
	CFI_ADJUST_CFA_OFFSET 4
	.endr
	pushl %eax
	CFI_ADJUST_CFA_OFFSET 4
	SAVE_ALL
	FIXUP_ESPFIX_STACK		# %eax == %esp
	xorl %edx,%edx			# zero error code
	call do_nmi
	RESTORE_REGS
	lss 12+4(%esp), %esp		# back to espfix stack
	CFI_ADJUST_CFA_OFFSET -24
	jmp irq_return
#endif
	CFI_ENDPROC
END(nmi)

ENTRY(int3)
	RING0_INT_FRAME
	pushl $-1			# mark this as an int
	CFI_ADJUST_CFA_OFFSET 4
	SAVE_ALL
	TRACE_IRQS_OFF
	xorl %edx,%edx		# zero error code
	movl %esp,%eax		# pt_regs pointer
	call do_int3
	jmp ret_from_exception
	CFI_ENDPROC
END(int3)

ENTRY(general_protection)
	RING0_EC_FRAME
	pushl $do_general_protection
	CFI_ADJUST_CFA_OFFSET 4
	jmp error_code
	CFI_ENDPROC
END(general_protection)

/*
 * End of kprobes section
 */
	.popsection
