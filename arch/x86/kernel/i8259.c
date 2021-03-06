#include <linux/linkage.h>
#include <linux/errno.h>
#include <linux/signal.h>
#include <linux/sched.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/timex.h>
#include <linux/slab.h>
#include <linux/random.h>
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
#include <asm/i8259.h>

/*
 * http://blog.csdn.net/yunsongice/article/details/5306207
 *
 * 每个能够发出中断请求的硬件设备控制器都有一条名为IRQ的输出线。
 * 所有现有的IRQ线都与一个可编程中断控制器PIC的硬件电路的输入引脚相连。
 *
 * PIC执行下列动作：
 * 1. 监视IRQ线，检查产生的信号。如果两条或两条以上的IRQ线上产生信号，选择引脚编号较小的IRQ线
 * 2. 如果有信号出现在IRQ线上：
 *   a. 把接收到的引发信号转换成对应的向量号
 *   b. 把这个向量存放在中断控制器的一个I/O端口（0x20，0x21），从而运行CPU通过数据总线读此向量
 *   c. 把引发信号发送到处理器的INTR引脚，即产生一个中断
 *   d. 等待，直到CPU通过把这个中断信号写进PIC的一个I/O端口来确认它；
 *      当这种情况发生时，请INTR线
 * 3. 返回第1步
 */

/*
 * This is the 'legacy' 8259A Programmable Interrupt Controller,
 * present in the majority of PC/AT boxes.
 * plus some generic x86 specific things if generic specifics makes
 * any sense at all.
 */

static int i8259A_auto_eoi;
DEFINE_SPINLOCK(i8259A_lock);
static void mask_and_ack_8259A(unsigned int);

// 8259A中断控制器
struct irq_chip i8259A_chip = {
	.name		= "XT-PIC",
	.mask		= disable_8259A_irq,
	.disable	= disable_8259A_irq,
	.unmask		= enable_8259A_irq,
	.mask_ack	= mask_and_ack_8259A,
};

/*
 * 8259A PIC functions to handle ISA devices:
 */

/*
 * This contains the irq mask for both 8259A irq controllers,
 */
unsigned int cached_irq_mask = 0xffff;

/*
 * Not all IRQs can be routed through the IO-APIC, eg. on certain (older)
 * boards the timer interrupt is not really connected to any IO-APIC pin,
 * it's fed to the master 8259A's IR0 line only.
 *
 * Any '1' bit in this mask means the IRQ is routed through the IO-APIC.
 * this 'mixed mode' IRQ handling costs nothing because it's only used
 * at IRQ setup time.
 */
unsigned long io_apic_irqs;

void disable_8259A_irq(unsigned int irq)
{
	unsigned int mask = 1 << irq;
	unsigned long flags;

	spin_lock_irqsave(&i8259A_lock, flags);
	cached_irq_mask |= mask;
	if (irq & 8)
		outb(cached_slave_mask, PIC_SLAVE_IMR);
	else
		outb(cached_master_mask, PIC_MASTER_IMR);
	spin_unlock_irqrestore(&i8259A_lock, flags);
}

void enable_8259A_irq(unsigned int irq)
{
	unsigned int mask = ~(1 << irq);
	unsigned long flags;

	spin_lock_irqsave(&i8259A_lock, flags);
	cached_irq_mask &= mask;
	if (irq & 8)
		outb(cached_slave_mask, PIC_SLAVE_IMR);
	else
		outb(cached_master_mask, PIC_MASTER_IMR);
	spin_unlock_irqrestore(&i8259A_lock, flags);
}

int i8259A_irq_pending(unsigned int irq)
{
	unsigned int mask = 1<<irq;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&i8259A_lock, flags);
	if (irq < 8)
		ret = inb(PIC_MASTER_CMD) & mask;
	else
		ret = inb(PIC_SLAVE_CMD) & (mask >> 8);
	spin_unlock_irqrestore(&i8259A_lock, flags);

	return ret;
}

void make_8259A_irq(unsigned int irq)
{
	disable_irq_nosync(irq);
	io_apic_irqs &= ~(1<<irq);
   // 8259A中断控制器的电流处理函数被注册为handle_level_irq，即为电平触发中断。
   // 对应的边沿触发中断的处理函数是handle_edge_irq
	set_irq_chip_and_handler_name(irq, &i8259A_chip, handle_level_irq,
				      "XT");
	enable_irq(irq);
}

/*
 * This function assumes to be called rarely. Switching between
 * 8259A registers is slow.
 * This has to be protected by the irq controller spinlock
 * before being called.
 */
static inline int i8259A_irq_real(unsigned int irq)
{
	int value;
	int irqmask = 1<<irq;

	if (irq < 8) {
		outb(0x0B, PIC_MASTER_CMD);	/* ISR register */
		value = inb(PIC_MASTER_CMD) & irqmask;
		outb(0x0A, PIC_MASTER_CMD);	/* back to the IRR register */
		return value;
	}
	outb(0x0B, PIC_SLAVE_CMD);	/* ISR register */
	value = inb(PIC_SLAVE_CMD) & (irqmask >> 8);
	outb(0x0A, PIC_SLAVE_CMD);	/* back to the IRR register */
	return value;
}

/*
 * Careful! The 8259A is a fragile beast, it pretty
 * much _has_ to be done exactly like this (mask it
 * first, _then_ send the EOI, and the order of EOI
 * to the two 8259s is important!
 */
static void mask_and_ack_8259A(unsigned int irq)
{
	unsigned int irqmask = 1 << irq;
	unsigned long flags;

	spin_lock_irqsave(&i8259A_lock, flags);
	/*
	 * Lightweight spurious IRQ detection. We do not want
	 * to overdo spurious IRQ handling - it's usually a sign
	 * of hardware problems, so we only do the checks we can
	 * do without slowing down good hardware unnecessarily.
	 *
	 * Note that IRQ7 and IRQ15 (the two spurious IRQs
	 * usually resulting from the 8259A-1|2 PICs) occur
	 * even if the IRQ is masked in the 8259A. Thus we
	 * can check spurious 8259A IRQs without doing the
	 * quite slow i8259A_irq_real() call for every IRQ.
	 * This does not cover 100% of spurious interrupts,
	 * but should be enough to warn the user that there
	 * is something bad going on ...
	 */
	if (cached_irq_mask & irqmask)
		goto spurious_8259A_irq;
	cached_irq_mask |= irqmask;

handle_real_irq:
	if (irq & 8) {
		inb(PIC_SLAVE_IMR);	/* DUMMY - (do we need this?) */
		outb(cached_slave_mask, PIC_SLAVE_IMR);
		/* 'Specific EOI' to slave */
		outb(0x60+(irq&7), PIC_SLAVE_CMD);
		 /* 'Specific EOI' to master-IRQ2 */
		outb(0x60+PIC_CASCADE_IR, PIC_MASTER_CMD);
	} else {
		inb(PIC_MASTER_IMR);	/* DUMMY - (do we need this?) */
		outb(cached_master_mask, PIC_MASTER_IMR);
		outb(0x60+irq, PIC_MASTER_CMD);	/* 'Specific EOI to master */
	}
	spin_unlock_irqrestore(&i8259A_lock, flags);
	return;

spurious_8259A_irq:
	/*
	 * this is the slow path - should happen rarely.
	 */
	if (i8259A_irq_real(irq))
		/*
		 * oops, the IRQ _is_ in service according to the
		 * 8259A - not spurious, go handle it.
		 */
		goto handle_real_irq;

	{
		static int spurious_irq_mask;
		/*
		 * At this point we can be sure the IRQ is spurious,
		 * lets ACK and report it. [once per IRQ]
		 */
		if (!(spurious_irq_mask & irqmask)) {
			printk(KERN_DEBUG
			       "spurious 8259A interrupt: IRQ%d.\n", irq);
			spurious_irq_mask |= irqmask;
		}
		atomic_inc(&irq_err_count);
		/*
		 * Theoretically we do not have to handle this IRQ,
		 * but in Linux this does not cause problems and is
		 * simpler for us.
		 */
		goto handle_real_irq;
	}
}

static char irq_trigger[2];
/**
 * ELCR registers (0x4d0, 0x4d1) control edge/level of IRQ
 */
static void restore_ELCR(char *trigger)
{
	outb(trigger[0], 0x4d0);
	outb(trigger[1], 0x4d1);
}

static void save_ELCR(char *trigger)
{
	/* IRQ 0,1,2,8,13 are marked as reserved */
	trigger[0] = inb(0x4d0) & 0xF8;
	trigger[1] = inb(0x4d1) & 0xDE;
}

static int i8259A_resume(struct sys_device *dev)
{
	init_8259A(i8259A_auto_eoi);
	restore_ELCR(irq_trigger);
	return 0;
}

static int i8259A_suspend(struct sys_device *dev, pm_message_t state)
{
	save_ELCR(irq_trigger);
	return 0;
}

static int i8259A_shutdown(struct sys_device *dev)
{
	/* Put the i8259A into a quiescent state that
	 * the kernel initialization code can get it
	 * out of.
	 */
	outb(0xff, PIC_MASTER_IMR);	/* mask all of 8259A-1 */
	outb(0xff, PIC_SLAVE_IMR);	/* mask all of 8259A-1 */
	return 0;
}

static struct sysdev_class i8259_sysdev_class = {
	.name = "i8259",
	.suspend = i8259A_suspend,
	.resume = i8259A_resume,
	.shutdown = i8259A_shutdown,
};

static struct sys_device device_i8259A = {
	.id	= 0,
	.cls	= &i8259_sysdev_class,
};

static int __init i8259A_init_sysfs(void)
{
	int error = sysdev_class_register(&i8259_sysdev_class);
	if (!error)
		error = sysdev_register(&device_i8259A);
	return error;
}

device_initcall(i8259A_init_sysfs);

void mask_8259A(void)
{
	unsigned long flags;

	spin_lock_irqsave(&i8259A_lock, flags);

	outb(0xff, PIC_MASTER_IMR);	/* mask all of 8259A-1 */
	outb(0xff, PIC_SLAVE_IMR);	/* mask all of 8259A-2 */

	spin_unlock_irqrestore(&i8259A_lock, flags);
}

void unmask_8259A(void)
{
	unsigned long flags;

	spin_lock_irqsave(&i8259A_lock, flags);

	outb(cached_master_mask, PIC_MASTER_IMR); /* restore master IRQ mask */
	outb(cached_slave_mask, PIC_SLAVE_IMR);	  /* restore slave IRQ mask */

	spin_unlock_irqrestore(&i8259A_lock, flags);
}

void init_8259A(int auto_eoi)
{
   /*
    * 8259A内部的四个中断命令字（ICW）寄存器，都是用来启动初始化编程的：
    * ICW1：初始化命令字
    * ICW2：中断向量寄存器
    * ICW3：8259的级联命令字，用来区分主片和从片
    * ICW4：指定中断嵌套方式、数据缓冲选择、中断结束方式和CPU类型
    */
	unsigned long flags;

	i8259A_auto_eoi = auto_eoi;

	spin_lock_irqsave(&i8259A_lock, flags);

   // 送数据到工作寄存器OCW1（中断屏蔽字）
   // 屏蔽所有外部中断，因此此时系统尚未初始化完成
	outb(0xff, PIC_MASTER_IMR);	/* mask all of 8259A-1 */
	outb(0xff, PIC_SLAVE_IMR);	/* mask all of 8259A-2 */

	/*
	 * outb_pic - this has to work on a wide range of PC hardware.
	 */
   // 送0x11到ICW1（通过端口0x20），启动初始化编程
   // 0x11表示外部中断请求信号为上升沿有效
   // 系统中有多片8259A级联，还表示要向ICW4送数据
	outb_pic(0x11, PIC_MASTER_CMD);	/* ICW1: select 8259A-1 init */

	/* ICW2: 8259A-1 IR0-7 mapped to 0x30-0x37 on x86-64,
	   to 0x20-0x27 on i386 */
	outb_pic(IRQ0_VECTOR, PIC_MASTER_IMR);

	/* 8259A-1 (the master) has a slave on IR2 */
	outb_pic(1U << PIC_CASCADE_IR, PIC_MASTER_IMR);

	if (auto_eoi)	/* master does Auto EOI */
		outb_pic(MASTER_ICW4_DEFAULT | PIC_ICW4_AEOI, PIC_MASTER_IMR);
	else		/* master expects normal EOI */
		outb_pic(MASTER_ICW4_DEFAULT, PIC_MASTER_IMR);

	outb_pic(0x11, PIC_SLAVE_CMD);	/* ICW1: select 8259A-2 init */

	/* ICW2: 8259A-2 IR0-7 mapped to IRQ8_VECTOR */
	outb_pic(IRQ8_VECTOR, PIC_SLAVE_IMR);
	/* 8259A-2 is a slave on master's IR2 */
	outb_pic(PIC_CASCADE_IR, PIC_SLAVE_IMR);
	/* (slave's support for AEOI in flat mode is to be investigated) */
	outb_pic(SLAVE_ICW4_DEFAULT, PIC_SLAVE_IMR);

	if (auto_eoi)
		/*
		 * In AEOI mode we just have to mask the interrupt
		 * when acking.
		 */
		i8259A_chip.mask_ack = disable_8259A_irq;
	else
		i8259A_chip.mask_ack = mask_and_ack_8259A;

	udelay(100);		/* wait for 8259A to initialize */

	outb(cached_master_mask, PIC_MASTER_IMR); /* restore master IRQ mask */
	outb(cached_slave_mask, PIC_SLAVE_IMR);	  /* restore slave IRQ mask */

	spin_unlock_irqrestore(&i8259A_lock, flags);
}
