#ifndef __LINUX_COMPILER_H
#error "Please don't include <linux/compiler-gcc.h> directly, include <linux/compiler.h> instead."
#endif

/*
 * Common definitions for all gcc versions go here.
 */

/*
 * http://blog.csdn.net/yunsongice/article/details/5605260
 *
 * 当使用指令优化的编译器时，千万不要以为指令会严格按它们在源代码中出现的顺序执行。
 * 编译器可能重新安排汇编语言指令以使寄存器以最优的方式使用。
 * 此外，现代CPU通常并行地执行若干条指令，且可能重新安排内存访问。
 * 这种重新排序可以极大地加速程序的执行。
 *
 * 在处理同步时，必须避免指令重新排序。
 * 因为如果放在同步原语之后的一条指令在同步原语本身之前执行了，事情就变得失控。
 * 所以，所有的同步技术都应该避免指令优化后的乱序，这里用到一个“优化屏障”和
 * “内存壁垒”的技术
 *
 * 优化屏障
 * 保证编译器不会混淆放在同步操作之前的汇编语言指令和放在同步操作之后的汇编语言指令
 * 指令的具体内容其实是一个空的指令，asm告诉编译器要插入汇编语言片段（这里为空）。
 * volatile禁止编译器把asm指令与程序中的其他指令重新组合。
 *
 * 因此，执行优化屏障宏后，编译器不能使用存放在CPU寄存器中的内存单元的值来优化asm指令前
 * 的代码。注意，优化屏障并不保证不使当前CPU把汇编语言指令混在一起执行（避免CPU乱序执行
 * 是内存壁垒的工作）
 *
 * 内存壁垒
 * 确保在进入临界区之后的操作开始执行之前，临界区之前的操作已经完成。
 * 因此，内存壁垒类似于防火墙，让任何汇编语言指令都不能通过。
 * 在80x86处理器中，下列种类的汇编语言指令时“串行的”，因此它们其内存壁垒的作用。
 * 1. 对I/O端口进行操作的所有指令
 * 2. 有lock前缀的所有指令
 * 3. 写控制寄存器、系统寄存器或调试寄存器的所有指令（如cli、sti）
 * 4. lfence、sfence和mfence分别有效地实现了读内存壁垒、写内存壁垒和读写内存壁垒
 * 5. 少数专门的指令，如iret
 */

/*
 * CPU越过内存屏障后，将刷新自己对存储器的缓冲状态。
 * 这条语句实际上不会产生任何代码，但可使gcc在barrier()之后刷新寄存器对变量的分配
 *
 * __asm__用于指示编译器在此插入汇编语句
 * __volatile__告诉编译器，严禁将此处的汇编语句与其他的语句重组合优化！
 * memory强制gcc编译器假设RAM所以内存单元均被汇编指令修改，这样CPU中的寄存器和缓存中的内存单元的数据将作废
 * CPU将不得不在需要的时候重新读取内存中的数据。
 * 这就阻止了CPU又将寄存器，缓存中的数据用于去优化指令，避免去访问内存
 *
 * 注意barrier()只能防止编译器对指令做乱序优化，但是不会阻止CPU的乱序执行
 */
/* Optimization barrier */
/* The "volatile" is due to gcc bugs */
#define barrier() __asm__ __volatile__("": : :"memory")

/*
 * This macro obfuscates arithmetic on a variable address so that gcc
 * shouldn't recognize the original var, and make assumptions about it.
 *
 * This is needed because the C standard makes it undefined to do
 * pointer arithmetic on "objects" outside their boundaries and the
 * gcc optimizers assume this is the case. In particular they
 * assume such arithmetic does not wrap.
 *
 * A miscompilation has been observed because of this on PPC.
 * To work around it we hide the relationship of the pointer and the object
 * using this macro.
 *
 * Versions of the ppc64 compiler before 4.1 had a bug where use of
 * RELOC_HIDE could trash r30. The bug can be worked around by changing
 * the inline assembly constraint from =g to =r, in this particular
 * case either is valid.
 */
#define RELOC_HIDE(ptr, off)					\
  ({ unsigned long __ptr;					\
    __asm__ ("" : "=r"(__ptr) : "0"(ptr));		\
    (typeof(ptr)) (__ptr + (off)); })

/* &a[0] degrades to a pointer: a different type from an array */
#define __must_be_array(a) \
  BUILD_BUG_ON_ZERO(__builtin_types_compatible_p(typeof(a), typeof(&a[0])))

/*
 * Force always-inline if the user requests it so via the .config,
 * or if gcc is too old:
 */
#if !defined(CONFIG_ARCH_SUPPORTS_OPTIMIZED_INLINING) || \
    !defined(CONFIG_OPTIMIZE_INLINING) || (__GNUC__ < 4)
# define inline		inline		__attribute__((always_inline))
# define __inline__	__inline__	__attribute__((always_inline))
# define __inline	__inline	__attribute__((always_inline))
#endif

#define __deprecated			__attribute__((deprecated))
#define __packed			__attribute__((packed))
#define __weak				__attribute__((weak))

/*
 * it doesn't make sense on ARM (currently the only user of __naked) to trace
 * naked functions because then mcount is called without stack and frame pointer
 * being set up and there is no chance to restore the lr register to the value
 * before mcount was called.
 */
#define __naked				__attribute__((naked)) notrace

#define __noreturn			__attribute__((noreturn))

/*
 * From the GCC manual:
 *
 * Many functions have no effects except the return value and their
 * return value depends only on the parameters and/or global
 * variables.  Such a function can be subject to common subexpression
 * elimination and loop optimization just as an arithmetic operator
 * would be.
 * [...]
 */
#define __pure				__attribute__((pure))
#define __aligned(x)			__attribute__((aligned(x)))
#define __printf(a,b)			__attribute__((format(printf,a,b)))
#define  noinline			__attribute__((noinline))
#define __attribute_const__		__attribute__((__const__))
#define __maybe_unused			__attribute__((unused))

#define __gcc_header(x) #x
#define _gcc_header(x) __gcc_header(linux/compiler-gcc##x.h)
#define gcc_header(x) _gcc_header(x)
#include gcc_header(__GNUC__)
