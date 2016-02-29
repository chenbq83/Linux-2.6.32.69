#include <linux/linkage.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/timex.h>
#include <linux/slab.h>
#include <linux/random.h>
#include <linux/kprobes.h>
#include <linux/init.h>
#include <linux/kernel_stat.h>
#include <linux/sysdev.h>
#include <linux/bitops.h>
#include <linux/acpi.h>
#include <linux/io.h>
#include <linux/delay.h>

#include <asm/atomic.h>
#include <asm/system.h>
#include <asm/timer.h>
#include <asm/hw_irq.h>
#include <asm/pgtable.h>
#include <asm/desc.h>
#include <asm/apic.h>
#include <asm/setup.h>
#include <asm/i8259.h>
#include <asm/traps.h>

/*
 * ISA PIC or low IO-APIC triggered (INTA-cycle or APIC) interrupts:
 * (these are usually mapped to vectors 0x30-0x3f)
 */

/*
 * The IO-APIC gives us many more interrupt sources. Most of these
 * are unused but an SMP system is supposed to have enough memory ...
 * sometimes (mostly wrt. hw bugs) we get corrupted vectors all
 * across the spectrum, so we really want to be prepared to get all
 * of these. Plus, more powerful systems might have more than 64
 * IO-APIC registers.
 *
 * (these are usually mapped into the 0x30-0xff vector range)
 */

#ifdef CONFIG_X86_32
/*
 * Note that on a 486, we don't want to do a SIGFPE on an irq13
 * as the irq is unreliable, and exception 16 works correctly
 * (ie as explained in the intel literature). On a 386, you
 * can't use exception 16 due to bad IBM design, so we have to
 * rely on the less exact irq13.
 *
 * Careful.. Not only is IRQ13 unreliable, but it is also
 * leads to races. IBM designers who came up with it should
 * be shot.
 */

static irqreturn_t math_error_irq(int cpl, void *dev_id)
{
	outb(0, 0xF0);
	if (ignore_fpu_irq || !boot_cpu_data.hard_math)
		return IRQ_NONE;
	math_error((void __user *)get_irq_regs()->ip);
	return IRQ_HANDLED;
}

/*
 * New motherboards sometimes make IRQ 13 be a PCI interrupt,
 * so allow interrupt sharing.
 */
static struct irqaction fpu_irq = {
	.handler = math_error_irq,
	.name = "fpu",
};
#endif

/*
 * IRQ2 is cascade interrupt to second interrupt controller
 */
static struct irqaction irq2 = {
	.handler = no_action,
	.name = "cascade",
};

DEFINE_PER_CPU(vector_irq_t, vector_irq) = {
	[0 ... IRQ0_VECTOR - 1] = -1,
	[IRQ0_VECTOR] = 0,
	[IRQ1_VECTOR] = 1,
	[IRQ2_VECTOR] = 2,
	[IRQ3_VECTOR] = 3,
	[IRQ4_VECTOR] = 4,
	[IRQ5_VECTOR] = 5,
	[IRQ6_VECTOR] = 6,
	[IRQ7_VECTOR] = 7,
	[IRQ8_VECTOR] = 8,
	[IRQ9_VECTOR] = 9,
	[IRQ10_VECTOR] = 10,
	[IRQ11_VECTOR] = 11,
	[IRQ12_VECTOR] = 12,
	[IRQ13_VECTOR] = 13,
	[IRQ14_VECTOR] = 14,
	[IRQ15_VECTOR] = 15,
	[IRQ15_VECTOR + 1 ... NR_VECTORS - 1] = -1
};

int vector_used_by_percpu_irq(unsigned int vector)
{
	int cpu;

	for_each_online_cpu(cpu) {
		if (per_cpu(vector_irq, cpu)[vector] != -1)
			return 1;
	}

	return 0;
}

void __init init_ISA_irqs(void)
{
	int i;

#if defined(CONFIG_X86_64) || defined(CONFIG_X86_LOCAL_APIC)
	init_bsp_APIC();
#endif
	init_8259A(0);

	/*
	 * 16 old-style INTA-cycle interrupts:
	 */
	for (i = 0; i < NR_IRQS_LEGACY; i++) {
		struct irq_desc *desc = irq_to_desc(i);

		desc->status = IRQ_DISABLED;
		desc->action = NULL;
		desc->depth = 1;

		set_irq_chip_and_handler_name(i, &i8259A_chip,
					      handle_level_irq, "XT");
	}
}

void __init init_IRQ(void)
{
   // 调用native_init_IRQ()
	x86_init.irqs.intr_init();
}

static void __init smp_intr_init(void)
{
#ifdef CONFIG_SMP
#if defined(CONFIG_X86_64) || defined(CONFIG_X86_LOCAL_APIC)
	/*
	 * The reschedule interrupt is a CPU-to-CPU reschedule-helper
	 * IPI, driven by wakeup.
	 */
	alloc_intr_gate(RESCHEDULE_VECTOR, reschedule_interrupt);

	/* IPIs for invalidation */
	alloc_intr_gate(INVALIDATE_TLB_VECTOR_START+0, invalidate_interrupt0);
	alloc_intr_gate(INVALIDATE_TLB_VECTOR_START+1, invalidate_interrupt1);
	alloc_intr_gate(INVALIDATE_TLB_VECTOR_START+2, invalidate_interrupt2);
	alloc_intr_gate(INVALIDATE_TLB_VECTOR_START+3, invalidate_interrupt3);
	alloc_intr_gate(INVALIDATE_TLB_VECTOR_START+4, invalidate_interrupt4);
	alloc_intr_gate(INVALIDATE_TLB_VECTOR_START+5, invalidate_interrupt5);
	alloc_intr_gate(INVALIDATE_TLB_VECTOR_START+6, invalidate_interrupt6);
	alloc_intr_gate(INVALIDATE_TLB_VECTOR_START+7, invalidate_interrupt7);

	/* IPI for generic function call */
	alloc_intr_gate(CALL_FUNCTION_VECTOR, call_function_interrupt);

	/* IPI for generic single function call */
	alloc_intr_gate(CALL_FUNCTION_SINGLE_VECTOR,
			call_function_single_interrupt);

	/* Low priority IPI to cleanup after moving an irq */
	set_intr_gate(IRQ_MOVE_CLEANUP_VECTOR, irq_move_cleanup_interrupt);
	set_bit(IRQ_MOVE_CLEANUP_VECTOR, used_vectors);

	/* IPI used for rebooting/stopping */
	alloc_intr_gate(REBOOT_VECTOR, reboot_interrupt);
#endif
#endif /* CONFIG_SMP */
}

static void __init apic_intr_init(void)
{
	smp_intr_init();

#ifdef CONFIG_X86_THERMAL_VECTOR
	alloc_intr_gate(THERMAL_APIC_VECTOR, thermal_interrupt);
#endif
#ifdef CONFIG_X86_MCE_THRESHOLD
	alloc_intr_gate(THRESHOLD_APIC_VECTOR, threshold_interrupt);
#endif
#if defined(CONFIG_X86_MCE) && defined(CONFIG_X86_LOCAL_APIC)
	alloc_intr_gate(MCE_SELF_VECTOR, mce_self_interrupt);
#endif

#if defined(CONFIG_X86_64) || defined(CONFIG_X86_LOCAL_APIC)
	/* self generated IPI for local APIC timer */
	alloc_intr_gate(LOCAL_TIMER_VECTOR, apic_timer_interrupt);

	/* generic IPI for platform specific use */
	alloc_intr_gate(GENERIC_INTERRUPT_VECTOR, generic_interrupt);

	/* IPI vectors for APIC spurious and error interrupts */
	alloc_intr_gate(SPURIOUS_APIC_VECTOR, spurious_interrupt);
	alloc_intr_gate(ERROR_APIC_VECTOR, error_interrupt);

	/* Performance monitoring interrupts: */
# ifdef CONFIG_PERF_EVENTS
	alloc_intr_gate(LOCAL_PENDING_VECTOR, perf_pending_interrupt);
# endif

#endif
}

/*
 * http://www.cnblogs.com/hustcat/archive/2009/08/11/1543889.html
 *
 * Linux对256个中断向量的分配如下：
 * 0 - 31：异常和非屏蔽中断，实际上被Intel保留
 * 32 - 47：可屏蔽中断
 * 48 - 255：用来标识软中断。Linux只用了其中一个，即128(0x80)，用来实现系统调用
 *
 * 可屏蔽中断：
 * x86通过两个级联的8259A中断控制器来管理15个外部中断源
 * 外部设备要使用中断线，首先要申请中断号（IRQ），每条中断线的中断号IRQn对应的中断向量为n+32，
 * IRQ和向量之间的映射可以通过中断控制器端口来修改。
 * x86下8259A的初始化工作及IRQ与向量的映射关系在函数init_8259A()完成
 */
void __init native_init_IRQ(void)
{
	int i;

	/* Execute any quirks before the call gates are initialised: */
   // 初始化8259A PIC中断控制器相关的数据结构
	x86_init.irqs.pre_vector_init();

   // 初始化SMP的APIC中断门描述符
	apic_intr_init();

   // 在这里进行遍历所有的中断门，将所有还没有配置的中断门进行统一配置。
   // interrupt函数指针数组，当发生中断的时候将会触发这个interrupt函数，
   // 然后所有的interrupt都会调用do_IRQ，从而触发真正的中断服务程序
	/*
	 * Cover the whole vector space, no vector can escape
	 * us. (some of these will be overridden and become
	 * 'special' SMP interrupts)
	 */
   // 更新外部中断（IRQ）的IDT表项
   // FIRST_EXTERNAL_VECTOR=0x20, NR_VECTORS=256
	for (i = FIRST_EXTERNAL_VECTOR; i < NR_VECTORS; i++) {
		/* IA32_SYSCALL_VECTOR could be used in trap_init already. */
      // 跳过系统调用（trap）使用过的槽位
		if (!test_bit(i, used_vectors))
         // 在IDT的第i个表项插入一个中断门。
         // 门中的段选择符设置为内核代码的段选择符，基偏移量为中断处理程序的地址
         // 即第二个参数
         // interrupt数组在entry_32.S中定义，它本质上都会跳转到common_interrupt
			set_intr_gate(i, interrupt[i-FIRST_EXTERNAL_VECTOR]);
	}

	if (!acpi_ioapic)
		setup_irq(2, &irq2);

#ifdef CONFIG_X86_32
	/*
	 * External FPU? Set up irq13 if so, for
	 * original braindamaged IBM FERR coupling.
	 */
	if (boot_cpu_data.hard_math && !cpu_has_fpu)
		setup_irq(FPU_IRQ, &fpu_irq);

	irq_ctx_init(smp_processor_id());
#endif
}
