#ifndef _ASM_X86_SPINLOCK_H
#define _ASM_X86_SPINLOCK_H

#include <asm/atomic.h>
#include <asm/rwlock.h>
#include <asm/page.h>
#include <asm/processor.h>
#include <linux/compiler.h>
#include <asm/paravirt.h>
/*
 * Your basic SMP spinlocks, allowing only a single CPU anywhere
 *
 * Simple spin lock operations.  There are two variants, one clears IRQ's
 * on the local processor, one does not.
 *
 * These are fair FIFO ticket locks, which are currently limited to 256
 * CPUs.
 *
 * (the type definitions are in asm/spinlock_types.h)
 */

#ifdef CONFIG_X86_32
# define LOCK_PTR_REG "a"
# define REG_PTR_MODE "k"
#else
# define LOCK_PTR_REG "D"
# define REG_PTR_MODE "q"
#endif

#if defined(CONFIG_X86_32) && \
	(defined(CONFIG_X86_OOSTORE) || defined(CONFIG_X86_PPRO_FENCE))
/*
 * On PPro SMP or if we are using OOSTORE, we use a locked operation to unlock
 * (PPro errata 66, 92)
 */
# define UNLOCK_LOCK_PREFIX LOCK_PREFIX
#else
# define UNLOCK_LOCK_PREFIX
#endif

/*
 * Ticket locks are conceptually two parts, one indicating the current head of
 * the queue, and the other indicating the current tail. The lock is acquired
 * by atomically noting the tail and incrementing it by one (thus adding
 * ourself to the queue and noting our position), then waiting until the head
 * becomes equal to the the initial value of the tail.
 *
 * We use an xadd covering *both* parts of the lock, to increment the tail and
 * also load the position of the head, which takes care of memory ordering
 * issues and should be optimal for the uncontended case. Note the tail must be
 * in the high part, because a wide xadd increment of the low part would carry
 * up and contaminate the high part.
 *
 * With fewer than 2^8 possible CPUs, we can use x86's partial registers to
 * save some instructions and make the code more elegant. There really isn't
 * much between them in performance though, especially as locks are out of line.
 */
#if (NR_CPUS < 256)
#define TICKET_SHIFT 8

/*
 * http://blog.sina.com.cn/s/blog_6d7fa49b01014q7p.html
 *
 * x86 SMP自旋锁的实现：
 * xaddw表示先交换源操作数和目的操作数的数值，然后两个操作数想加，结果保存在目的寄存器中
 * 这里的目的寄存器是%1，也就是lock->slock变量
 *
 * 详细过程：
 * 首先，交换slock和inc的值，即slock=0x0100，inc=0x0000
 * 然后，将两者相加并保存到slock中，得到slock=0x0100，inc=0x0000
 * 判断inc的高字节和低字节，H=0x00，L=0x00
 * 二者相等，表示成功申请到自旋锁
 *
 * 如果另一个进行在申请这一把自旋锁，执行spin_lock()
 * 得到inc=0x0100，因为自旋锁已经被另一个进行获得了，此时slock=0x0100
 * 重复上述的过程，得到slock=0x0200，inc=0x0100，inc的高八位和低八位不相等
 * 表示拿不到锁。
 * 然后将slock(0x0200)的低八位赋值给inc的低八位，即H=0x01，L=0x00，依旧不相等，所以一直在自旋
 *
 * 等到另一个进程释放自旋锁，然后slock的低八位加1，即slock=0x0201
 * 那么在自旋的过程中得到H=0x01，L=0x01，相等表示获得锁
 *
 * 下面的rep ; nop是一个混合指令，被翻译成pause指令。
 * 它向处理器提供一种提示：告诉处理器所执行的代码序列是一个自旋等待状态，
 * 处理器会根据这个提示而避开内存序列冲突
 */
static __always_inline void __ticket_spin_lock(raw_spinlock_t *lock)
{
	short inc = 0x0100;

	asm volatile (
		LOCK_PREFIX "xaddw %w0, %1\n"
		"1:\t"
		"cmpb %h0, %b0\n\t"      // 比较inc的高八位和低八位
		"je 2f\n\t"              // 如果相等，表示获得锁
		"rep ; nop\n\t"          // 混合指令，被翻译成pause指令。
		"movb %1, %b0\n\t"       // 将slock的低八位赋给inc的低八位
		/* don't need lfence here, because loads are in-order */
		"jmp 1b\n"               // 调整到上面继续比较inc的高八位和低八位
		"2:"
		: "+Q" (inc), "+m" (lock->slock)
		:
		: "memory", "cc");
}

static __always_inline int __ticket_spin_trylock(raw_spinlock_t *lock)
{
	int tmp, new;

	asm volatile("movzwl %2, %0\n\t"
		     "cmpb %h0,%b0\n\t"
		     "leal 0x100(%" REG_PTR_MODE "0), %1\n\t"
		     "jne 1f\n\t"
		     LOCK_PREFIX "cmpxchgw %w1,%2\n\t"
		     "1:"
		     "sete %b1\n\t"
		     "movzbl %b1,%0\n\t"
		     : "=&a" (tmp), "=&q" (new), "+m" (lock->slock)
		     :
		     : "memory", "cc");

	return tmp;
}

static __always_inline void __ticket_spin_unlock(raw_spinlock_t *lock)
{
	asm volatile(UNLOCK_LOCK_PREFIX "incb %0"
		     : "+m" (lock->slock)
		     :
		     : "memory", "cc");
}
#else
#define TICKET_SHIFT 16

static __always_inline void __ticket_spin_lock(raw_spinlock_t *lock)
{
	int inc = 0x00010000;
	int tmp;

	asm volatile(LOCK_PREFIX "xaddl %0, %1\n"
		     "movzwl %w0, %2\n\t"
		     "shrl $16, %0\n\t"
		     "1:\t"
		     "cmpl %0, %2\n\t"
		     "je 2f\n\t"
		     "rep ; nop\n\t"
		     "movzwl %1, %2\n\t"
		     /* don't need lfence here, because loads are in-order */
		     "jmp 1b\n"
		     "2:"
		     : "+r" (inc), "+m" (lock->slock), "=&r" (tmp)
		     :
		     : "memory", "cc");
}

static __always_inline int __ticket_spin_trylock(raw_spinlock_t *lock)
{
	int tmp;
	int new;

	asm volatile("movl %2,%0\n\t"
		     "movl %0,%1\n\t"
		     "roll $16, %0\n\t"
		     "cmpl %0,%1\n\t"
		     "leal 0x00010000(%" REG_PTR_MODE "0), %1\n\t"
		     "jne 1f\n\t"
		     LOCK_PREFIX "cmpxchgl %1,%2\n\t"
		     "1:"
		     "sete %b1\n\t"
		     "movzbl %b1,%0\n\t"
		     : "=&a" (tmp), "=&q" (new), "+m" (lock->slock)
		     :
		     : "memory", "cc");

	return tmp;
}

static __always_inline void __ticket_spin_unlock(raw_spinlock_t *lock)
{
	asm volatile(UNLOCK_LOCK_PREFIX "incw %0"
		     : "+m" (lock->slock)
		     :
		     : "memory", "cc");
}
#endif

static inline int __ticket_spin_is_locked(raw_spinlock_t *lock)
{
	int tmp = ACCESS_ONCE(lock->slock);

	return !!(((tmp >> TICKET_SHIFT) ^ tmp) & ((1 << TICKET_SHIFT) - 1));
}

static inline int __ticket_spin_is_contended(raw_spinlock_t *lock)
{
	int tmp = ACCESS_ONCE(lock->slock);

	return (((tmp >> TICKET_SHIFT) - tmp) & ((1 << TICKET_SHIFT) - 1)) > 1;
}

#ifndef CONFIG_PARAVIRT_SPINLOCKS

static inline int __raw_spin_is_locked(raw_spinlock_t *lock)
{
	return __ticket_spin_is_locked(lock);
}

static inline int __raw_spin_is_contended(raw_spinlock_t *lock)
{
	return __ticket_spin_is_contended(lock);
}
#define __raw_spin_is_contended	__raw_spin_is_contended

static __always_inline void __raw_spin_lock(raw_spinlock_t *lock)
{
	__ticket_spin_lock(lock);
}

static __always_inline int __raw_spin_trylock(raw_spinlock_t *lock)
{
	return __ticket_spin_trylock(lock);
}

static __always_inline void __raw_spin_unlock(raw_spinlock_t *lock)
{
	__ticket_spin_unlock(lock);
}

static __always_inline void __raw_spin_lock_flags(raw_spinlock_t *lock,
						  unsigned long flags)
{
	__raw_spin_lock(lock);
}

#endif	/* CONFIG_PARAVIRT_SPINLOCKS */

static inline void __raw_spin_unlock_wait(raw_spinlock_t *lock)
{
	while (__raw_spin_is_locked(lock))
		cpu_relax();
}

/*
 * Read-write spinlocks, allowing multiple readers
 * but only one writer.
 *
 * NOTE! it is quite common to have readers in interrupts
 * but no interrupt writers. For those circumstances we
 * can "mix" irq-safe locks - any writer needs to get a
 * irq-safe write-lock, but readers can get non-irqsafe
 * read-locks.
 *
 * On x86, we implement read-write locks as a 32-bit counter
 * with the high bit (sign) being the "contended" bit.
 */

/**
 * read_can_lock - would read_trylock() succeed?
 * @lock: the rwlock in question.
 */
static inline int __raw_read_can_lock(raw_rwlock_t *lock)
{
	return (int)(lock)->lock > 0;
}

/**
 * write_can_lock - would write_trylock() succeed?
 * @lock: the rwlock in question.
 */
static inline int __raw_write_can_lock(raw_rwlock_t *lock)
{
	return (lock)->lock == RW_LOCK_BIAS;
}

static inline void __raw_read_lock(raw_rwlock_t *rw)
{
	asm volatile(LOCK_PREFIX " subl $1,(%0)\n\t"
		     "jns 1f\n"
		     "call __read_lock_failed\n\t"
		     "1:\n"
		     ::LOCK_PTR_REG (rw) : "memory");
}

static inline void __raw_write_lock(raw_rwlock_t *rw)
{
	asm volatile(LOCK_PREFIX " subl %1,(%0)\n\t"
		     "jz 1f\n"
		     "call __write_lock_failed\n\t"
		     "1:\n"
		     ::LOCK_PTR_REG (rw), "i" (RW_LOCK_BIAS) : "memory");
}

static inline int __raw_read_trylock(raw_rwlock_t *lock)
{
	atomic_t *count = (atomic_t *)lock;

	if (atomic_dec_return(count) >= 0)
		return 1;
	atomic_inc(count);
	return 0;
}

static inline int __raw_write_trylock(raw_rwlock_t *lock)
{
	atomic_t *count = (atomic_t *)lock;

	if (atomic_sub_and_test(RW_LOCK_BIAS, count))
		return 1;
	atomic_add(RW_LOCK_BIAS, count);
	return 0;
}

static inline void __raw_read_unlock(raw_rwlock_t *rw)
{
	asm volatile(LOCK_PREFIX "incl %0" :"+m" (rw->lock) : : "memory");
}

static inline void __raw_write_unlock(raw_rwlock_t *rw)
{
	asm volatile(LOCK_PREFIX "addl %1, %0"
		     : "+m" (rw->lock) : "i" (RW_LOCK_BIAS) : "memory");
}

#define __raw_read_lock_flags(lock, flags) __raw_read_lock(lock)
#define __raw_write_lock_flags(lock, flags) __raw_write_lock(lock)

#define _raw_spin_relax(lock)	cpu_relax()
#define _raw_read_relax(lock)	cpu_relax()
#define _raw_write_relax(lock)	cpu_relax()

/* The {read|write|spin}_lock() on x86 are full memory barriers. */
static inline void smp_mb__after_lock(void) { }
#define ARCH_HAS_SMP_MB_AFTER_LOCK

#endif /* _ASM_X86_SPINLOCK_H */
