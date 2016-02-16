#ifndef _ASM_X86_SYSTEM_H
#define _ASM_X86_SYSTEM_H

#include <asm/asm.h>
#include <asm/segment.h>
#include <asm/cpufeature.h>
#include <asm/cmpxchg.h>
#include <asm/nops.h>

#include <linux/kernel.h>
#include <linux/irqflags.h>

/* entries in ARCH_DLINFO: */
#ifdef CONFIG_IA32_EMULATION
# define AT_VECTOR_SIZE_ARCH 2
#else
# define AT_VECTOR_SIZE_ARCH 1
#endif

struct task_struct; /* one of the stranger aspects of C forward declarations */
struct task_struct *__switch_to(struct task_struct *prev,
				struct task_struct *next);
struct tss_struct;
void __switch_to_xtra(struct task_struct *prev_p, struct task_struct *next_p,
		      struct tss_struct *tss);

#ifdef CONFIG_X86_32

#ifdef CONFIG_CC_STACKPROTECTOR
#define __switch_canary							\
	"movl %P[task_canary](%[next]), %%ebx\n\t"			\
	"movl %%ebx, "__percpu_arg([stack_canary])"\n\t"
#define __switch_canary_oparam						\
	, [stack_canary] "=m" (per_cpu_var(stack_canary.canary))
#define __switch_canary_iparam						\
	, [task_canary] "i" (offsetof(struct task_struct, stack_canary))
#else	/* CC_STACKPROTECTOR */
#define __switch_canary
#define __switch_canary_oparam
#define __switch_canary_iparam
#endif	/* CC_STACKPROTECTOR */

/*
 * http://blog.chinaunix.net/uid-14528823-id-4739291.html
 *
 * 用户栈与内核栈的切换
 * x86硬件结构中，SS为堆栈段寄存器，ESP为堆栈指针寄存器，而这两个寄存器在每个CPU都是唯一的。
 * 但是Linux中有很多种栈（用户栈、内核栈、中断栈），如果都需要使用的话，那就必须在各个栈之间切换。
 *
 * 当CPU运行的特权级发送变化时，就需要切换相应的堆栈。
 * Linux中只使用了两个特权级：ring0和ring3。当发生内核态和用户态间的切换时，需要考虑堆栈的切换。
 * 通常的切换时机有：系统调用、中断和异常及其返回处。
 *
 * 如何进行堆栈的切换？
 * 需要用到TSS段。tss_struct中包括了内核栈指针sp0和内核堆栈段寄存器ss0
 *
 * （1）用户栈到内核栈的切换
 * x86硬件结构中，中断、异常和系统调用都是通过中断门或陷阱门实现的，在通过中断门或陷阱门时，
 * *硬件会自动利用TSS，完成堆栈切换的工作*。
 * 硬件完成的操作包括：
 *   1. 找到ISR的入口。具体包括：
 *      确定中断或异常相关的中断向量、读取IDTR寄存器获取IDT表地址，从IDT表中读取中断向量对应的项,
 *      读取GDTR寄存器获取GDT表地址，在GDT表中查找IDT表项中的段描述符标识的段描述符，
 *      该描述符中指定了中断或异常处理程序（ISR）所在的段基址，结合IDT表项中的段偏移地址，
 *      即可找到ISR的入口地址。
 *   2. 权限检查。
 *      确认中断的来源是否合法，主要对比当前特权级（CS寄存器的低两位）和段描述符（GDT中）的DPL比较。
 *      如果CPL小于DPL，就产生GP（通用保护）异常。
 *      对于异常，还需作进一步的安全检查：对比CPL和IDT中的门描述符的DPL，如果DPL小于CPL，也产生GP异常，
 *      由此可以避免用户应用程序访问特殊的陷阱门和中断门。
 *   3. 检查特权级的变化。
 *      如果ring发生了变化，则进行一下处理（堆栈切换）：
 *      a. 读TR寄存器，获取TSS段
 *      b. 读取TSS中的新特权级的堆栈段和堆栈指针，将其load到SS和ESP寄存器。
 *      c. 在新特权级的栈中保存原始的SS和ESP的值（上一步load之前保存在哪里了？）
 *   4. 如果发生的是异常，则将引起异常的指令地址装载CS和EIP寄存器，如此可以使这条指令在异常处理程序
 *      执行完成之后能被再次执行。
 *      这是中断和异常的主要区别之一。如果发生的是中断，则跳过此步骤。
 *   5. 在新栈中压入EFLAGS、CS和EIP
 *   6. 如果是异常且产生了硬件出错码，则将它压入栈中。
 *   7. 用之前通过IDT和GDT获得的ISR的入口地址和段选择符装载EIP和CS寄存器，即可开始执行ISR
 *
 * 再次强调，上述动作均由硬件自动完成。
 * 从上述的步骤3可以看出，当用户态切换到内核态时，用户栈切换到内核栈实质是由硬件自动完成的，
 * 软件需要做的就是预先设置好TSS中的相关内容（比如sp0和ss0）
 *
 * （2）内核栈到用户栈的切换
 * 该过程实际是“用户栈到内核栈的切换”的逆过程，发送的时机在系统调用、中断和异常的返回处，
 * 实际的切换过程还是由硬件自动完成的。
 * 中断或异常返回时，必然会执行iret指令，然后将控制权交回给之前被中断打断的进程。
 * 硬件自动完成以下操作：
 *   1. 从当前栈（内核栈）从弹出CS、EIP和EFLAGS，并load到相应的寄存器
 *      （如果之前有硬件错误码入栈，需要先弹出这个错误码）
 *   2. 权限检查
 *      对比ISR的CPL是否等于CS中的低两位。如果是，iret终止返回；否则转入下一步
 *   3. 从当前栈（内核栈）中弹出之前压入的用户态堆栈相关的SS和ESP，并load到相应寄存器。
 *      至此，即完成了从内核栈到用户栈的切换。
 *   4. 后续处理。
 *      主要包括：检查DS、ES、FS及GS段寄存器，如果其中一个寄存器包含的选择符是一个段描述符，
 *      并且其DPL值小于CPL，那么清相关的段寄存器。
 *      目的是为了防止用户态的程序利用内核以前所用的段寄存器，从而访问到内核地址空间。
 *
 * （3）进程上下文切换时的堆栈切换
 * 内核调度时，被选中的next进程不是current进程时，会发生上下文切换，此时必然涉及堆栈切换。
 * 这里涉及两个相关的问题：
 *   1. 从current进程的堆栈切换到next进程的堆栈
 *      由于调度肯定发生在内核态，那么进程上下文切换时，也必然发生在内核态，那么此时的堆栈
 *      切换实质为current进程的内核栈到next进程的内核栈的切换。
 *      相应工作在switch_to宏中用汇编实现，通过next进程的栈指针（内核态）装载到ESP来实现。
 *      "movl %[next_sp],%%esp\n\t"	
 *   2. TSS中内核栈（sp0）的切换
 *      由于Linux的具体实现中，TSS不是针对每个进程，而是针对每个CPU的，即每个CPU对应一个
 *      tss_struct，那么在进程上下文切换时，需要考虑当前CPU的TSS中的内容的更新，其实就是
 *      内核栈指针的更新。更新后，当新进程再次进入到内核态执行时，才能确保CPU硬件能从TSS
 *      中自动读取到正确的内核栈指针sp0的值，以保证从用户态切换到内核态时，相应的堆栈切换
 *      正常。
 *      相应的切换在__switch_to中由load_sp0()函数完成
 */

/*
 * http://www.docin.com/p-832688018.html
 *
 * Saving eflags is important. It switches not only IOPL between tasks,
 * it also protects other tasks from NT leaking through sysenter etc.
 */

/*
 * switch_to切换主要有以下三部分：
 * 1. 进程切换，即esp的切换 （从esp可以找到进程的描述符）
 * 2. 硬件上下文的切换，调用__switch_to
 * 3. 堆栈的切换，即ebp的切换 （ebp是栈底指针，它确定了当前变量空间属于哪个进程）
 *
 * 详细步骤：
 * step 1： 复制两个变量到寄存器
 *   [prev] "a" (prev)
 *   [next] "d" (next)
 *   即：
 *   eax <== prev_A or eax <== %p(%ebp_A)
 *   edx <== next_A or edx <== %n(%ebp_A)
 *   这里prev和next都是A进程的局部变量
 *
 * step 2：保存A进程的ebp和eflags
 *   pushfl
 *   pushl %ebp
 *   注意：因为现在esp还在A的堆栈中，所以这两个东西被保存到进程A的内核堆栈中。
 *
 * step 3：保存当前esp到进程A的内核描述符中
 *   movl %%esp,%[prev_sp]\n\t
 *   它可以表示成 prev_A->thread.sp <== esp_A
 *   在调用switch_to时，prev是指向进程A自己的进程描述符的
 *
 * step 4：从next（B进程）的描述符中取出之前从B被切换时保存的esp_B
 *   movl %[next_sp],%%esp\n\t
 *   它可以表示成 exp_B <== next_A->thread.sp
 *   注意：在A进程中的next是指向B的进程描述符的。
 *   从这个时候开始，CPU当前执行的进程以及是B进程了，因为esp已经指向了B的内核堆栈。
 *   但是，现在的ebp仍然指向A进程的内核堆栈，所以所有局部变量仍然是A中的局部变量，
 *   比如next实质上是%n(%ebp_A)，也就是next_A，即指向B的进程描述符。
 *
 * step 5：把标号为1的指令地址保存到A进程描述符的ip域
 *   movl %1f,%[prev_ip\n\t
 *   它可以表示成 prev_A->thread.ip <== %1f
 *   当A进程下次被switch_to回来的时候，会从这条指令开始执行。
 *
 * step 6：将返回地址保存到堆栈，然后调用__switch_to函数，完成硬件上下文切换
 *   pushl %[next_ip]\n\t
 *   jmp __switch_to\n
 *   这里，如果之前B被switch_to出去过，那么[next_ip]里存的就是下面这个if的标号，
 *   但是如果B进程刚刚被创建，之前没有被切换出去过，那么[next_ip]里存的将是ret_from_fork（参考copy_thread）
 *   这就是为什么不用call __switch_to而是用jmp __switch_to
 *   因为call会导致自动把下面这句话的地址（也就是1:）压栈，然后__switch_to就比如只能ret到这里，
 *   而无法根据需要ret到ret_from_fork。
 *   
 * step 7：从__switch_to返回后继续从1:标号后面开始执行，修改ebp到B的内核堆栈，回复B的eflags
 *   popl %%ebp\n\t
 *   popfl\n
 *   如果从__switch_to返回后从这里继续运行，那么说明在此之前B肯定被switch_to调出去过，
 *   因此此前肯定备份了ebp_B和flags_B，这里执行回复操作。
 */
#define switch_to(prev, next, last)					\
do {									\
	/*								\
    *	prve：被替换的进程   \
    *	next：被调度的新进程 \
    *	last：当切换回原来的进程（prev）后，被替换的另外一个进程 \
    *	                     \
    *	上下文切换，在schedule中调用，current进程调度出去，当该进程被再次调度时，\
    *	重新从__switch_to后面开始执行 \
    *                      \
	 * Context-switching clobbers all registers, so we clobber	\
	 * them explicitly, via unused output variables.		\
	 * (EAX and EBP is not listed because EBP is saved/restored	\
	 * explicitly for wchan access and EAX is the return value of	\
	 * __switch_to())						\
	 */								\
	unsigned long ebx, ecx, edx, esi, edi;				\
									\
	asm volatile("pushfl\n\t"		/* save    flags */	\
		     "pushl %%ebp\n\t"		/* save    EBP   */	\
		     "movl %%esp,%[prev_sp]\n\t"	/* save    ESP   */ \
		     "movl %[next_sp],%%esp\n\t"	/* restore ESP   */ \
		     "movl $1f,%[prev_ip]\n\t"	/* save    EIP   */	\
		     "pushl %[next_ip]\n\t"	/* restore EIP   */	\
		     __switch_canary					\
		     "jmp __switch_to\n"	/* regparm call  */	\
		     "1:\t"						\
		     "popl %%ebp\n\t"		/* restore EBP   */	\
		     "popfl\n"			/* restore flags */	\
									\
		     /* output parameters */				\
		     : [prev_sp] "=m" (prev->thread.sp),		\
		       [prev_ip] "=m" (prev->thread.ip),		\
		       "=a" (last),					\
									\
		       /* clobbered output registers: */		\
		       "=b" (ebx), "=c" (ecx), "=d" (edx),		\
		       "=S" (esi), "=D" (edi)				\
		       							\
		       __switch_canary_oparam				\
									\
		       /* input parameters: */				\
		     : [next_sp]  "m" (next->thread.sp),		\
		       [next_ip]  "m" (next->thread.ip),		\
		       							\
		       /* regparm parameters for __switch_to(): */	\
		       [prev]     "a" (prev),				\
		       [next]     "d" (next)				\
									\
		       __switch_canary_iparam				\
									\
		     : /* reloaded segment registers */			\
			"memory");					\
} while (0)

/*
 * disable hlt during certain critical i/o operations
 */
#define HAVE_DISABLE_HLT
#else
#define __SAVE(reg, offset) "movq %%" #reg ",(14-" #offset ")*8(%%rsp)\n\t"
#define __RESTORE(reg, offset) "movq (14-" #offset ")*8(%%rsp),%%" #reg "\n\t"

/* frame pointer must be last for get_wchan */
#define SAVE_CONTEXT    "pushf ; pushq %%rbp ; movq %%rsi,%%rbp\n\t"
#define RESTORE_CONTEXT "movq %%rbp,%%rsi ; popq %%rbp ; popf\t"

#define __EXTRA_CLOBBER  \
	, "rcx", "rbx", "rdx", "r8", "r9", "r10", "r11", \
	  "r12", "r13", "r14", "r15"

#ifdef CONFIG_CC_STACKPROTECTOR
#define __switch_canary							  \
	"movq %P[task_canary](%%rsi),%%r8\n\t"				  \
	"movq %%r8,"__percpu_arg([gs_canary])"\n\t"
#define __switch_canary_oparam						  \
	, [gs_canary] "=m" (per_cpu_var(irq_stack_union.stack_canary))
#define __switch_canary_iparam						  \
	, [task_canary] "i" (offsetof(struct task_struct, stack_canary))
#else	/* CC_STACKPROTECTOR */
#define __switch_canary
#define __switch_canary_oparam
#define __switch_canary_iparam
#endif	/* CC_STACKPROTECTOR */

/* Save restore flags to clear handle leaking NT */
#define switch_to(prev, next, last) \
	asm volatile(SAVE_CONTEXT					  \
	     "movq %%rsp,%P[threadrsp](%[prev])\n\t" /* save RSP */	  \
	     "movq %P[threadrsp](%[next]),%%rsp\n\t" /* restore RSP */	  \
	     "call __switch_to\n\t"					  \
	     ".globl thread_return\n"					  \
	     "thread_return:\n\t"					  \
	     "movq "__percpu_arg([current_task])",%%rsi\n\t"		  \
	     __switch_canary						  \
	     "movq %P[thread_info](%%rsi),%%r8\n\t"			  \
	     "movq %%rax,%%rdi\n\t" 					  \
	     "testl  %[_tif_fork],%P[ti_flags](%%r8)\n\t"	  \
	     "jnz   ret_from_fork\n\t"					  \
	     RESTORE_CONTEXT						  \
	     : "=a" (last)					  	  \
	       __switch_canary_oparam					  \
	     : [next] "S" (next), [prev] "D" (prev),			  \
	       [threadrsp] "i" (offsetof(struct task_struct, thread.sp)), \
	       [ti_flags] "i" (offsetof(struct thread_info, flags)),	  \
	       [_tif_fork] "i" (_TIF_FORK),			  	  \
	       [thread_info] "i" (offsetof(struct task_struct, stack)),   \
	       [current_task] "m" (per_cpu_var(current_task))		  \
	       __switch_canary_iparam					  \
	     : "memory", "cc" __EXTRA_CLOBBER)
#endif

#ifdef __KERNEL__

extern void native_load_gs_index(unsigned);

/*
 * Load a segment. Fall back on loading the zero
 * segment if something goes wrong..
 */
#define loadsegment(seg, value)			\
	asm volatile("\n"			\
		     "1:\t"			\
		     "movl %k0,%%" #seg "\n"	\
		     "2:\n"			\
		     ".section .fixup,\"ax\"\n"	\
		     "3:\t"			\
		     "movl %k1, %%" #seg "\n\t"	\
		     "jmp 2b\n"			\
		     ".previous\n"		\
		     _ASM_EXTABLE(1b,3b)	\
		     : :"r" (value), "r" (0) : "memory")


/*
 * Save a segment register away
 */
#define savesegment(seg, value)				\
	asm("mov %%" #seg ",%0":"=r" (value) : : "memory")

/*
 * x86_32 user gs accessors.
 */
#ifdef CONFIG_X86_32
#ifdef CONFIG_X86_32_LAZY_GS
#define get_user_gs(regs)	(u16)({unsigned long v; savesegment(gs, v); v;})
#define set_user_gs(regs, v)	loadsegment(gs, (unsigned long)(v))
#define task_user_gs(tsk)	((tsk)->thread.gs)
#define lazy_save_gs(v)		savesegment(gs, (v))
#define lazy_load_gs(v)		loadsegment(gs, (v))
#else	/* X86_32_LAZY_GS */
#define get_user_gs(regs)	(u16)((regs)->gs)
#define set_user_gs(regs, v)	do { (regs)->gs = (v); } while (0)
#define task_user_gs(tsk)	(task_pt_regs(tsk)->gs)
#define lazy_save_gs(v)		do { } while (0)
#define lazy_load_gs(v)		do { } while (0)
#endif	/* X86_32_LAZY_GS */
#endif	/* X86_32 */

static inline unsigned long get_limit(unsigned long segment)
{
	unsigned long __limit;
	asm("lsll %1,%0" : "=r" (__limit) : "r" (segment));
	return __limit + 1;
}

static inline void native_clts(void)
{
	asm volatile("clts");
}

/*
 * Volatile isn't enough to prevent the compiler from reordering the
 * read/write functions for the control registers and messing everything up.
 * A memory clobber would solve the problem, but would prevent reordering of
 * all loads stores around it, which can hurt performance. Solution is to
 * use a variable and mimic reads and writes to it to enforce serialization
 */
static unsigned long __force_order;

static inline unsigned long native_read_cr0(void)
{
	unsigned long val;
	asm volatile("mov %%cr0,%0\n\t" : "=r" (val), "=m" (__force_order));
	return val;
}

static inline void native_write_cr0(unsigned long val)
{
	asm volatile("mov %0,%%cr0": : "r" (val), "m" (__force_order));
}

static inline unsigned long native_read_cr2(void)
{
	unsigned long val;
	asm volatile("mov %%cr2,%0\n\t" : "=r" (val), "=m" (__force_order));
	return val;
}

static inline void native_write_cr2(unsigned long val)
{
	asm volatile("mov %0,%%cr2": : "r" (val), "m" (__force_order));
}

static inline unsigned long native_read_cr3(void)
{
	unsigned long val;
	asm volatile("mov %%cr3,%0\n\t" : "=r" (val), "=m" (__force_order));
	return val;
}

static inline void native_write_cr3(unsigned long val)
{
	asm volatile("mov %0,%%cr3": : "r" (val), "m" (__force_order));
}

static inline unsigned long native_read_cr4(void)
{
	unsigned long val;
	asm volatile("mov %%cr4,%0\n\t" : "=r" (val), "=m" (__force_order));
	return val;
}

static inline unsigned long native_read_cr4_safe(void)
{
	unsigned long val;
	/* This could fault if %cr4 does not exist. In x86_64, a cr4 always
	 * exists, so it will never fail. */
#ifdef CONFIG_X86_32
	asm volatile("1: mov %%cr4, %0\n"
		     "2:\n"
		     _ASM_EXTABLE(1b, 2b)
		     : "=r" (val), "=m" (__force_order) : "0" (0));
#else
	val = native_read_cr4();
#endif
	return val;
}

static inline void native_write_cr4(unsigned long val)
{
	asm volatile("mov %0,%%cr4": : "r" (val), "m" (__force_order));
}

#ifdef CONFIG_X86_64
static inline unsigned long native_read_cr8(void)
{
	unsigned long cr8;
	asm volatile("movq %%cr8,%0" : "=r" (cr8));
	return cr8;
}

static inline void native_write_cr8(unsigned long val)
{
	asm volatile("movq %0,%%cr8" :: "r" (val) : "memory");
}
#endif

static inline void native_wbinvd(void)
{
	asm volatile("wbinvd": : :"memory");
}

#ifdef CONFIG_PARAVIRT
#include <asm/paravirt.h>
#else
#define read_cr0()	(native_read_cr0())
#define write_cr0(x)	(native_write_cr0(x))
#define read_cr2()	(native_read_cr2())
#define write_cr2(x)	(native_write_cr2(x))
#define read_cr3()	(native_read_cr3())
#define write_cr3(x)	(native_write_cr3(x))
#define read_cr4()	(native_read_cr4())
#define read_cr4_safe()	(native_read_cr4_safe())
#define write_cr4(x)	(native_write_cr4(x))
#define wbinvd()	(native_wbinvd())
#ifdef CONFIG_X86_64
#define read_cr8()	(native_read_cr8())
#define write_cr8(x)	(native_write_cr8(x))
#define load_gs_index   native_load_gs_index
#endif

/* Clear the 'TS' bit */
#define clts()		(native_clts())

#endif/* CONFIG_PARAVIRT */

#define stts() write_cr0(read_cr0() | X86_CR0_TS)

#endif /* __KERNEL__ */

static inline void clflush(volatile void *__p)
{
	asm volatile("clflush %0" : "+m" (*(volatile char __force *)__p));
}

#define nop() asm volatile ("nop")

void disable_hlt(void);
void enable_hlt(void);

void cpu_idle_wait(void);

extern unsigned long arch_align_stack(unsigned long sp);
extern void free_init_pages(char *what, unsigned long begin, unsigned long end);

void default_idle(void);

void stop_this_cpu(void *dummy);

/*
 * Force strict CPU ordering.
 * And yes, this is required on UP too when we're talking
 * to devices.
 */
#ifdef CONFIG_X86_32
/*
 * Some non-Intel clones support out of order store. wmb() ceases to be a
 * nop for these.
 */

/*
 * barrier()函数主要针对编译器的优化屏障，但是它不能阻止CPU重排指令时序。
 * 所以仅仅一个barrier函数还不够，内核还提供了以下几种机制。
 * 值得一提的是，使用了这些内存屏障，相当于停用了处理器或编译器提供的优化机制，
 * 结果是比如影响到程序的性能
 *
 * lock指令是一种前缀，它可与其他指令联合，用来维持总线的锁存信号直到其联合的指令执行完为止
 * 当CPU与其他处理器协同工作时，该指令可避免破坏有用的信息。
 * 它对中断没有任何影响，因为中断只能在指令之间产生
 * lock前缀的真正作用是保持对总线的控制权，直到整条指令执行完毕
 *
 * lfence指令：停止相关流水线，直到lfence之前对内存进行读取操作的指令全部完成
 * sfence指令：停止相关流水线，直到sfence之前对内存进行写入操作的指令全部完成
 * mfence指令：停止相关流水线，直到mfence之前对内存进行读取和写入操作的指令全部完成
 */

#define mb() alternative("lock; addl $0,0(%%esp)", "mfence", X86_FEATURE_XMM2)
#define rmb() alternative("lock; addl $0,0(%%esp)", "lfence", X86_FEATURE_XMM2)
#define wmb() alternative("lock; addl $0,0(%%esp)", "sfence", X86_FEATURE_XMM)
#else
#define mb() 	asm volatile("mfence":::"memory")
#define rmb()	asm volatile("lfence":::"memory")
#define wmb()	asm volatile("sfence" ::: "memory")
#endif

/**
 * read_barrier_depends - Flush all pending reads that subsequents reads
 * depend on.
 *
 * No data-dependent reads from memory-like regions are ever reordered
 * over this barrier.  All reads preceding this primitive are guaranteed
 * to access memory (but not necessarily other CPUs' caches) before any
 * reads following this primitive that depend on the data return by
 * any of the preceding reads.  This primitive is much lighter weight than
 * rmb() on most CPUs, and is never heavier weight than is
 * rmb().
 *
 * These ordering constraints are respected by both the local CPU
 * and the compiler.
 *
 * Ordering is not guaranteed by anything other than these primitives,
 * not even by data dependencies.  See the documentation for
 * memory_barrier() for examples and URLs to more information.
 *
 * For example, the following code would force ordering (the initial
 * value of "a" is zero, "b" is one, and "p" is "&a"):
 *
 * <programlisting>
 *	CPU 0				CPU 1
 *
 *	b = 2;
 *	memory_barrier();
 *	p = &b;				q = p;
 *					read_barrier_depends();
 *					d = *q;
 * </programlisting>
 *
 * because the read of "*q" depends on the read of "p" and these
 * two reads are separated by a read_barrier_depends().  However,
 * the following code, with the same initial values for "a" and "b":
 *
 * <programlisting>
 *	CPU 0				CPU 1
 *
 *	a = 2;
 *	memory_barrier();
 *	b = 3;				y = b;
 *					read_barrier_depends();
 *					x = a;
 * </programlisting>
 *
 * does not enforce ordering, since there is no data dependency between
 * the read of "a" and the read of "b".  Therefore, on some CPUs, such
 * as Alpha, "y" could be set to 3 and "x" to 0.  Use rmb()
 * in cases like this where there are no data dependencies.
 **/

#define read_barrier_depends()	do { } while (0)

#ifdef CONFIG_SMP
#define smp_mb()	mb()
#ifdef CONFIG_X86_PPRO_FENCE
# define smp_rmb()	rmb()
#else
# define smp_rmb()	barrier()
#endif
#ifdef CONFIG_X86_OOSTORE
# define smp_wmb() 	wmb()
#else
# define smp_wmb()	barrier()
#endif
#define smp_read_barrier_depends()	read_barrier_depends()
#define set_mb(var, value) do { (void)xchg(&var, value); } while (0)
#else
#define smp_mb()	barrier()
#define smp_rmb()	barrier()
#define smp_wmb()	barrier()
#define smp_read_barrier_depends()	do { } while (0)
#define set_mb(var, value) do { var = value; barrier(); } while (0)
#endif

/*
 * Stop RDTSC speculation. This is needed when you need to use RDTSC
 * (or get_cycles or vread that possibly accesses the TSC) in a defined
 * code region.
 *
 * (Could use an alternative three way for this if there was one.)
 */
static __always_inline void rdtsc_barrier(void)
{
	alternative(ASM_NOP3, "mfence", X86_FEATURE_MFENCE_RDTSC);
	alternative(ASM_NOP3, "lfence", X86_FEATURE_LFENCE_RDTSC);
}

#endif /* _ASM_X86_SYSTEM_H */
