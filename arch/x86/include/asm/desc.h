#ifndef _ASM_X86_DESC_H
#define _ASM_X86_DESC_H

#include <asm/desc_defs.h>
#include <asm/ldt.h>
#include <asm/mmu.h>
#include <linux/smp.h>

static inline void fill_ldt(struct desc_struct *desc,
			    const struct user_desc *info)
{
	desc->limit0 = info->limit & 0x0ffff;
	desc->base0 = info->base_addr & 0x0000ffff;

	desc->base1 = (info->base_addr & 0x00ff0000) >> 16;
	desc->type = (info->read_exec_only ^ 1) << 1;
	desc->type |= info->contents << 2;
	desc->s = 1;
	desc->dpl = 0x3;
	desc->p = info->seg_not_present ^ 1;
	desc->limit = (info->limit & 0xf0000) >> 16;
	desc->avl = info->useable;
	desc->d = info->seg_32bit;
	desc->g = info->limit_in_pages;
	desc->base2 = (info->base_addr & 0xff000000) >> 24;
	/*
	 * Don't allow setting of the lm bit. It is useless anyway
	 * because 64bit system calls require __USER_CS:
	 */
	desc->l = 0;
}

extern struct desc_ptr idt_descr;
extern gate_desc idt_table[];

struct gdt_page {
	struct desc_struct gdt[GDT_ENTRIES];
} __attribute__((aligned(PAGE_SIZE)));
DECLARE_PER_CPU_PAGE_ALIGNED(struct gdt_page, gdt_page);

static inline struct desc_struct *get_cpu_gdt_table(unsigned int cpu)
{
	return per_cpu(gdt_page, cpu).gdt;
}

#ifdef CONFIG_X86_64

static inline void pack_gate(gate_desc *gate, unsigned type, unsigned long func,
			     unsigned dpl, unsigned ist, unsigned seg)
{
	gate->offset_low = PTR_LOW(func);
	gate->segment = __KERNEL_CS;
	gate->ist = ist;
	gate->p = 1;
	gate->dpl = dpl;
	gate->zero0 = 0;
	gate->zero1 = 0;
	gate->type = type;
	gate->offset_middle = PTR_MIDDLE(func);
	gate->offset_high = PTR_HIGH(func);
}

#else
static inline void pack_gate(gate_desc *gate, unsigned char type,
			     unsigned long base, unsigned dpl, unsigned flags,
			     unsigned short seg)
{
	gate->a = (seg << 16) | (base & 0xffff);
	gate->b = (base & 0xffff0000) |
		  (((0x80 | type | (dpl << 5)) & 0xff) << 8);
}

#endif

static inline int desc_empty(const void *ptr)
{
	const u32 *desc = ptr;
	return !(desc[0] | desc[1]);
}

#ifdef CONFIG_PARAVIRT
#include <asm/paravirt.h>
#else
#define load_TR_desc() native_load_tr_desc()
#define load_gdt(dtr) native_load_gdt(dtr)
#define load_idt(dtr) native_load_idt(dtr)
#define load_tr(tr) asm volatile("ltr %0"::"m" (tr))
#define load_ldt(ldt) asm volatile("lldt %0"::"m" (ldt))

#define store_gdt(dtr) native_store_gdt(dtr)
#define store_idt(dtr) native_store_idt(dtr)
#define store_tr(tr) (tr = native_store_tr())

#define load_TLS(t, cpu) native_load_tls(t, cpu)
#define set_ldt native_set_ldt

#define write_ldt_entry(dt, entry, desc)	\
	native_write_ldt_entry(dt, entry, desc)
#define write_gdt_entry(dt, entry, desc, type)		\
	native_write_gdt_entry(dt, entry, desc, type)
#define write_idt_entry(dt, entry, g)		\
	native_write_idt_entry(dt, entry, g)

static inline void paravirt_alloc_ldt(struct desc_struct *ldt, unsigned entries)
{
}

static inline void paravirt_free_ldt(struct desc_struct *ldt, unsigned entries)
{
}
#endif	/* CONFIG_PARAVIRT */

#define store_ldt(ldt) asm("sldt %0" : "=m"(ldt))

static inline void native_write_idt_entry(gate_desc *idt, int entry,
					  const gate_desc *gate)
{
	memcpy(&idt[entry], gate, sizeof(*gate));
}

static inline void native_write_ldt_entry(struct desc_struct *ldt, int entry,
					  const void *desc)
{
	memcpy(&ldt[entry], desc, 8);
}

static inline void native_write_gdt_entry(struct desc_struct *gdt, int entry,
					  const void *desc, int type)
{
	unsigned int size;
	switch (type) {
	case DESC_TSS:
		size = sizeof(tss_desc);
		break;
	case DESC_LDT:
		size = sizeof(ldt_desc);
		break;
	default:
		size = sizeof(struct desc_struct);
		break;
	}
	memcpy(&gdt[entry], desc, size);
}

static inline void pack_descriptor(struct desc_struct *desc, unsigned long base,
				   unsigned long limit, unsigned char type,
				   unsigned char flags)
{
	desc->a = ((base & 0xffff) << 16) | (limit & 0xffff);
	desc->b = (base & 0xff000000) | ((base & 0xff0000) >> 16) |
		(limit & 0x000f0000) | ((type & 0xff) << 8) |
		((flags & 0xf) << 20);
	desc->p = 1;
}


static inline void set_tssldt_descriptor(void *d, unsigned long addr,
					 unsigned type, unsigned size)
{
#ifdef CONFIG_X86_64
	struct ldttss_desc64 *desc = d;
	memset(desc, 0, sizeof(*desc));
	desc->limit0 = size & 0xFFFF;
	desc->base0 = PTR_LOW(addr);
	desc->base1 = PTR_MIDDLE(addr) & 0xFF;
	desc->type = type;
	desc->p = 1;
	desc->limit1 = (size >> 16) & 0xF;
	desc->base2 = (PTR_MIDDLE(addr) >> 8) & 0xFF;
	desc->base3 = PTR_HIGH(addr);
#else
	pack_descriptor((struct desc_struct *)d, addr, size, 0x80 | type, 0);
#endif
}

static inline void __set_tss_desc(unsigned cpu, unsigned int entry, void *addr)
{
   // 设置TSS段描述符，将GDT中TSS段描述符中的base地址设置为指定的addr，
   // 即让TSS段指向指定的addr
   
   // 获取per-CPU的gdt（全局描述符表）
	struct desc_struct *d = get_cpu_gdt_table(cpu);
	tss_desc tss;

	/*
	 * sizeof(unsigned long) coming from an extra "long" at the end
	 * of the iobitmap. See tss_struct definition in processor.h
	 *
	 * -1? seg base+limit should be pointing to the address of the
	 * last valid byte
	 */
	set_tssldt_descriptor(&tss, (unsigned long)addr, DESC_TSS,
			      IO_BITMAP_OFFSET + IO_BITMAP_BYTES +
			      sizeof(unsigned long) - 1);
	write_gdt_entry(d, entry, &tss, DESC_TSS);
}

#define set_tss_desc(cpu, addr) __set_tss_desc(cpu, GDT_ENTRY_TSS, addr)

static inline void native_set_ldt(const void *addr, unsigned int entries)
{
	if (likely(entries == 0))
		asm volatile("lldt %w0"::"q" (0));
	else {
		unsigned cpu = smp_processor_id();
		ldt_desc ldt;

		set_tssldt_descriptor(&ldt, (unsigned long)addr, DESC_LDT,
				      entries * LDT_ENTRY_SIZE - 1);
		write_gdt_entry(get_cpu_gdt_table(cpu), GDT_ENTRY_LDT,
				&ldt, DESC_LDT);
		asm volatile("lldt %w0"::"q" (GDT_ENTRY_LDT*8));
	}
}

static inline void native_load_tr_desc(void)
{
	asm volatile("ltr %w0"::"q" (GDT_ENTRY_TSS*8));
}

static inline void native_load_gdt(const struct desc_ptr *dtr)
{
	asm volatile("lgdt %0"::"m" (*dtr));
}

static inline void native_load_idt(const struct desc_ptr *dtr)
{
	asm volatile("lidt %0"::"m" (*dtr));
}

static inline void native_store_gdt(struct desc_ptr *dtr)
{
	asm volatile("sgdt %0":"=m" (*dtr));
}

static inline void native_store_idt(struct desc_ptr *dtr)
{
	asm volatile("sidt %0":"=m" (*dtr));
}

static inline unsigned long native_store_tr(void)
{
	unsigned long tr;
	asm volatile("str %0":"=r" (tr));
	return tr;
}

static inline void native_load_tls(struct thread_struct *t, unsigned int cpu)
{
	unsigned int i;
	struct desc_struct *gdt = get_cpu_gdt_table(cpu);

	for (i = 0; i < GDT_ENTRY_TLS_ENTRIES; i++)
		gdt[GDT_ENTRY_TLS_MIN + i] = t->tls_array[i];
}

/* This intentionally ignores lm, since 32-bit apps don't have that field. */
#define LDT_empty(info)					\
	((info)->base_addr		== 0	&&	\
	 (info)->limit			== 0	&&	\
	 (info)->contents		== 0	&&	\
	 (info)->read_exec_only		== 1	&&	\
	 (info)->seg_32bit		== 0	&&	\
	 (info)->limit_in_pages		== 0	&&	\
	 (info)->seg_not_present	== 1	&&	\
	 (info)->useable		== 0)

/* Lots of programs expect an all-zero user_desc to mean "no segment at all". */
static inline bool LDT_zero(const struct user_desc *info)
{
	return (info->base_addr		== 0 &&
		info->limit		== 0 &&
		info->contents		== 0 &&
		info->read_exec_only	== 0 &&
		info->seg_32bit		== 0 &&
		info->limit_in_pages	== 0 &&
		info->seg_not_present	== 0 &&
		info->useable		== 0);
}

static inline void clear_LDT(void)
{
	set_ldt(NULL, 0);
}

/*
 * load one particular LDT into the current CPU
 */
static inline void load_LDT_nolock(mm_context_t *pc)
{
	set_ldt(pc->ldt, pc->size);
}

static inline void load_LDT(mm_context_t *pc)
{
	preempt_disable();
	load_LDT_nolock(pc);
	preempt_enable();
}

static inline unsigned long get_desc_base(const struct desc_struct *desc)
{
	return (unsigned)(desc->base0 | ((desc->base1) << 16) | ((desc->base2) << 24));
}

static inline void set_desc_base(struct desc_struct *desc, unsigned long base)
{
	desc->base0 = base & 0xffff;
	desc->base1 = (base >> 16) & 0xff;
	desc->base2 = (base >> 24) & 0xff;
}

static inline unsigned long get_desc_limit(const struct desc_struct *desc)
{
	return desc->limit0 | (desc->limit << 16);
}

static inline void set_desc_limit(struct desc_struct *desc, unsigned long limit)
{
	desc->limit0 = limit & 0xffff;
	desc->limit = (limit >> 16) & 0xf;
}

// 设置中断描述符表idt_table中的第gate项
static inline void _set_gate(int gate, unsigned type, void *addr,
			     unsigned dpl, unsigned ist, unsigned seg)
{
	gate_desc s;
	pack_gate(&s, type, (unsigned long)addr, dpl, ist, seg);
	/*
	 * does not need to be atomic because it is only done once at
	 * setup time
	 */
	write_idt_entry(idt_table, gate, &s);
}

/*
 * This needs to use 'idt_table' rather than 'idt', and
 * thus use the _nonmapped_ version of the IDT, as the
 * Pentium F0 0F bugfix can have resulted in the mapped
 * IDT being write-protected.
 */
static inline void set_intr_gate(unsigned int n, void *addr)
{
	BUG_ON((unsigned)n > 0xFF);
	_set_gate(n, GATE_INTERRUPT, addr, 0, 0, __KERNEL_CS);
}

extern int first_system_vector;
/* used_vectors is BITMAP for irq is not managed by percpu vector_irq */
extern unsigned long used_vectors[];

static inline void alloc_system_vector(int vector)
{
	if (!test_bit(vector, used_vectors)) {
		set_bit(vector, used_vectors);
		if (first_system_vector > vector)
			first_system_vector = vector;
	} else
		BUG();
}

static inline void alloc_intr_gate(unsigned int n, void *addr)
{
	alloc_system_vector(n);
	set_intr_gate(n, addr);
}

/*
 * This routine sets up an interrupt gate at directory privilege level 3.
 * 以下函数设置中断描述符表idt_table的第n项
 * _set_gate的第二个参数对应于中断门或陷阱门格式中的D标志位加上类型位段。
 * 第四个参数对应于DPL位段。
 *
 * 除了任务门（Linux用不到），其他的门的段描述符都设置成内核代码段的选择符，
 * 偏移地址设为addr
 */
static inline void set_system_intr_gate(unsigned int n, void *addr)
{
   /*
    * 系统门，用户态进程可以访问Intel系统门（DPL=3）
    * 通过系统门来激活三个Linux异常处理程序，它们的向量是4，5和128
    * 因此，在用户态，可以发布into、bound和int $0x80三条汇编语言指令
    */

   // 中断门，包含段选择符和中断或异常处理程序的段内偏移量。
   // 当控制权转移到一个适当的段时，处理器清IF标志，从而关闭将来会发生的可屏蔽中断
	BUG_ON((unsigned)n > 0xFF);
   // GATE_INTERRUPT（0xE）表示D标志位为1，类型为110，所以设置的是中断门
   // DPL设置为3.
   // 当中断时由外部产生或者CPU异常产生的，中断门的DPL是被忽略不顾的，所以总能穿过该中断门
	_set_gate(n, GATE_INTERRUPT, addr, 0x3, 0, __KERNEL_CS);
}

static inline void set_system_trap_gate(unsigned int n, void *addr)
{
   /*
    * int指令允许用户态进程发出一个中断信号，其值可以是0-255的任意一个向量。
    * 因此，为了防止用户通过int指令模拟非法的中断和异常，IDT的初始化必须非常小心。
    * 这可以通过把中断门或陷阱门的DPL字段设置为0来实现
    * 
    * 这里只允许0x80的中断穿过陷阱门
    * 从用户态发出的其他中断则不允许穿过陷阱门
    */

   // 陷阱门，与中断门相似，只是控制权转移到一个适当的段时处理器不清除IF标志
	BUG_ON((unsigned)n > 0xFF);
   // DPL设置为3
	_set_gate(n, GATE_TRAP, addr, 0x3, 0, __KERNEL_CS);
}

static inline void set_trap_gate(unsigned int n, void *addr)
{
   /*
    * 陷阱门，用户态程序不能访问的一个Intel陷阱门（DPL=0）
    * 大部分Linux异常处理程序都通过陷阱门来激活
    */

	BUG_ON((unsigned)n > 0xFF);
	_set_gate(n, GATE_TRAP, addr, 0, 0, __KERNEL_CS);
}

static inline void set_task_gate(unsigned int n, unsigned int gdt_entry)
{
   /*
    * 任务门，不能被用户态进程访问的Intel任务门（DPL=0）
    * Linux对“double fault”异常的处理程序时由任务门激活的
    */

   // 任务门，当中断信号发生时，必须取代当前进程的那个进程的TSS选择符存放在任务门中
	BUG_ON((unsigned)n > 0xFF);
	_set_gate(n, GATE_TASK, (void *)0, 0, 0, (gdt_entry<<3));
}

static inline void set_intr_gate_ist(int n, void *addr, unsigned ist)
{
   /*
    * 中断门，用户态的进程不能访问Intel中断门（DPL=0）
    * 所有的Linux中断处理程序都通过中断门激活，并全部限制在内核态
    */

	BUG_ON((unsigned)n > 0xFF);
	_set_gate(n, GATE_INTERRUPT, addr, 0, ist, __KERNEL_CS);
}

static inline void set_system_intr_gate_ist(int n, void *addr, unsigned ist)
{
   /*
    * 系统中断门，能够被用户态进程访问的Intel中断门（DPL=3）
    * 与向量3相关的异常处理程序是由系统中断门激活的。因此，在用户态可以使用汇编语言指令int3
    */

	BUG_ON((unsigned)n > 0xFF);
	_set_gate(n, GATE_INTERRUPT, addr, 0x3, ist, __KERNEL_CS);
}

#endif /* _ASM_X86_DESC_H */
