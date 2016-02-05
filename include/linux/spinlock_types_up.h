#ifndef __LINUX_SPINLOCK_TYPES_UP_H
#define __LINUX_SPINLOCK_TYPES_UP_H

#ifndef __LINUX_SPINLOCK_TYPES_H
# error "please don't include this file directly"
#endif

/*
 * include/linux/spinlock_types_up.h - spinlock type definitions for UP
 *
 * portions Copyright 2005, Red Hat, Inc., Ingo Molnar
 * Released under the General Public License (GPL).
 */

#ifdef CONFIG_DEBUG_SPINLOCK

typedef struct {
	volatile unsigned int slock;
} raw_spinlock_t;

#define __RAW_SPIN_LOCK_UNLOCKED { 1 }

#else

// 在单核处理器中，这个结构是个空结构体！
// 为什么？
// 因为自旋锁的思想就是在SMP环境中，保护共享的数据结构
// 也就是在CPU-A访问共享数据的期间，其他CPU不能访问同样的数据
// 单CPU的情况下，关闭中断就达到了独占的目的了
typedef struct { } raw_spinlock_t;

#define __RAW_SPIN_LOCK_UNLOCKED { }

#endif

typedef struct {
	/* no debug version on UP */
} raw_rwlock_t;

#define __RAW_RW_LOCK_UNLOCKED { }

#endif /* __LINUX_SPINLOCK_TYPES_UP_H */
