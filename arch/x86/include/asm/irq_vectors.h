#ifndef _ASM_X86_IRQ_VECTORS_H
#define _ASM_X86_IRQ_VECTORS_H

/*
 * http://blog.csdn.net/yunsongice/article/details/5353715
 *
 * 中断处理比异常处理要复杂很多，因为：
 * 1. 中断的发生与正在运行的进程无关，被调用的中断处理函数叫做中断服务程序。
 *    它运行在内核态并处于系统上下文中（使用内核页表，其为内核代码和内核数据结构）
 *    所以中断处理程序不允许被阻塞
 * 2. 由于硬件资源的限制，一根IRQ信号线需要被几个外部设备共享
 *    因此当一个IRQ信号产生时CPU不可能预先知道是哪一个外部设备发出的中断请求，
 *    故要求内核必须提供一个能够为共享IRQ的几个设备提供服务的中断处理程序
 * 3. 由于执行中断处理程序时不允许被阻塞，因此要求中断处理程序的代码尽可能短，
 *    以求快速执行完毕而不影响整个系统的性能
 *
 * 当中断发生时，并不是与中断相关的所有操作都具有同等的紧迫性。
 * Linux把中断发生后需要CPU执行的操作分为“快速中断处理”和“慢速中断处理”
 *
 * 快速中断处理：
 * 又分为两类，一类是需要紧急处理的操作，如修改由设备和数据结构同时访问的数据结构、对中断控制器
 * 或设备控制器重新编程等情况。这类处理动作需要在关中断的情况下执行。另一类是一些不是那么紧急
 * 但不能延迟的操作，比如键盘上某个按键被按下了需要读扫描码。这类中断处理是在开中断的情况下进行的，
 * 因为这类中断操作只修改CPU才会访问到的数据
 *
 * 慢速中断处理：
 * 不是太紧急同时在一定时间范围内可延迟的中断操作，比如把一个关中断期间拷贝到某缓冲区的内容
 * 复制到进程的地址空间，需要这些数据的进程会“耐心”等待，因此这类操作可以延迟较长时间，由内核
 * 在适当的时候调度他们运行。这类慢速处理动作用到的就是著名的“下半部分”函数。
 */

/*
 * Linux IRQ vector layout.
 * 中断向量：
 *
 * There are 256 IDT entries (per CPU - each entry is 8 bytes) which can
 * be defined by Linux. They are used as a jump table by the CPU when a
 * given vector is triggered - by a CPU-external, CPU-internal or
 * software-triggered event.
 *
 * Linux sets the kernel code address each entry jumps to early during
 * bootup, and never changes them. This is the general layout of the
 * IDT entries:
 *
 *  Vectors   0 ...  31 : system traps and exceptions - hardcoded events
 *  Vectors  32 ... 127 : device interrupts
 *  Vector  128         : legacy int80 syscall interface
 *  Vectors 129 ... 237 : device interrupts
 *  Vectors 238 ... 255 : special interrupts
 *
 * 64-bit x86 has per CPU IDT tables, 32-bit has one shared IDT table.
 *
 * This file enumerates the exact layout of them:
 */

#define NMI_VECTOR			0x02
#define MCE_VECTOR			0x12

/*
 * IDT vectors usable for external interrupt sources start
 * at 0x20:
 */
#define FIRST_EXTERNAL_VECTOR		0x20

#ifdef CONFIG_X86_32
# define SYSCALL_VECTOR			0x80
# define IA32_SYSCALL_VECTOR		0x80
#else
# define IA32_SYSCALL_VECTOR		0x80
#endif

/*
 * Reserve the lowest usable priority level 0x20 - 0x2f for triggering
 * cleanup after irq migration.
 */
#define IRQ_MOVE_CLEANUP_VECTOR		FIRST_EXTERNAL_VECTOR

/*
 * Vectors 0x30-0x3f are used for ISA interrupts.
 */
#define IRQ0_VECTOR			(FIRST_EXTERNAL_VECTOR + 0x10)

#define IRQ1_VECTOR			(IRQ0_VECTOR +  1)
#define IRQ2_VECTOR			(IRQ0_VECTOR +  2)
#define IRQ3_VECTOR			(IRQ0_VECTOR +  3)
#define IRQ4_VECTOR			(IRQ0_VECTOR +  4)
#define IRQ5_VECTOR			(IRQ0_VECTOR +  5)
#define IRQ6_VECTOR			(IRQ0_VECTOR +  6)
#define IRQ7_VECTOR			(IRQ0_VECTOR +  7)
#define IRQ8_VECTOR			(IRQ0_VECTOR +  8)
#define IRQ9_VECTOR			(IRQ0_VECTOR +  9)
#define IRQ10_VECTOR			(IRQ0_VECTOR + 10)
#define IRQ11_VECTOR			(IRQ0_VECTOR + 11)
#define IRQ12_VECTOR			(IRQ0_VECTOR + 12)
#define IRQ13_VECTOR			(IRQ0_VECTOR + 13)
#define IRQ14_VECTOR			(IRQ0_VECTOR + 14)
#define IRQ15_VECTOR			(IRQ0_VECTOR + 15)

/*
 * Special IRQ vectors used by the SMP architecture, 0xf0-0xff
 *
 *  some of the following vectors are 'rare', they are merged
 *  into a single vector (CALL_FUNCTION_VECTOR) to save vector space.
 *  TLB, reschedule and local APIC vectors are performance-critical.
 */

#define SPURIOUS_APIC_VECTOR		0xff
/*
 * Sanity check
 */
#if ((SPURIOUS_APIC_VECTOR & 0x0F) != 0x0F)
# error SPURIOUS_APIC_VECTOR definition error
#endif

#define ERROR_APIC_VECTOR		0xfe
#define RESCHEDULE_VECTOR		0xfd
#define CALL_FUNCTION_VECTOR		0xfc
#define CALL_FUNCTION_SINGLE_VECTOR	0xfb
#define THERMAL_APIC_VECTOR		0xfa
#define THRESHOLD_APIC_VECTOR		0xf9
#define REBOOT_VECTOR			0xf8

/* f0-f7 used for spreading out TLB flushes: */
#define INVALIDATE_TLB_VECTOR_END	0xf7
#define INVALIDATE_TLB_VECTOR_START	0xf0
#define NUM_INVALIDATE_TLB_VECTORS	   8

/*
 * Local APIC timer IRQ vector is on a different priority level,
 * to work around the 'lost local interrupt if more than 2 IRQ
 * sources per level' errata.
 */
#define LOCAL_TIMER_VECTOR		0xef

/*
 * Generic system vector for platform specific use
 */
#define GENERIC_INTERRUPT_VECTOR	0xed

/*
 * Performance monitoring pending work vector:
 */
#define LOCAL_PENDING_VECTOR		0xec

#define UV_BAU_MESSAGE			0xea

/*
 * Self IPI vector for machine checks
 */
#define MCE_SELF_VECTOR			0xeb

/*
 * First APIC vector available to drivers: (vectors 0x30-0xee) we
 * start at 0x31(0x41) to spread out vectors evenly between priority
 * levels. (0x80 is the syscall vector)
 */
#define FIRST_DEVICE_VECTOR		(IRQ15_VECTOR + 2)

#define NR_VECTORS			 256

#define FPU_IRQ				  13

#define	FIRST_VM86_IRQ			   3
#define LAST_VM86_IRQ			  15

#ifndef __ASSEMBLY__
static inline int invalid_vm86_irq(int irq)
{
	return irq < FIRST_VM86_IRQ || irq > LAST_VM86_IRQ;
}
#endif

/*
 * Size the maximum number of interrupts.
 *
 * If the irq_desc[] array has a sparse layout, we can size things
 * generously - it scales up linearly with the maximum number of CPUs,
 * and the maximum number of IO-APICs, whichever is higher.
 *
 * In other cases we size more conservatively, to not create too large
 * static arrays.
 */

#define NR_IRQS_LEGACY			  16

#define CPU_VECTOR_LIMIT		(  8 * NR_CPUS      )
#define IO_APIC_VECTOR_LIMIT		( 32 * MAX_IO_APICS )

#ifdef CONFIG_X86_IO_APIC
# ifdef CONFIG_SPARSE_IRQ
#  define NR_IRQS					\
	(CPU_VECTOR_LIMIT > IO_APIC_VECTOR_LIMIT ?	\
		(NR_VECTORS + CPU_VECTOR_LIMIT)  :	\
		(NR_VECTORS + IO_APIC_VECTOR_LIMIT))
# else
#  if NR_CPUS < MAX_IO_APICS
#   define NR_IRQS 			(NR_VECTORS + 4*CPU_VECTOR_LIMIT)
#  else
#   define NR_IRQS			(NR_VECTORS + IO_APIC_VECTOR_LIMIT)
#  endif
# endif
#else /* !CONFIG_X86_IO_APIC: */
# define NR_IRQS			NR_IRQS_LEGACY
#endif

#endif /* _ASM_X86_IRQ_VECTORS_H */
