#ifndef _LINUX_ERR_H
#define _LINUX_ERR_H

#include <linux/compiler.h>

#include <asm/errno.h>

/*
 * Kernel pointers have redundant information, so we can use a
 * scheme where we can return either an error code or a dentry
 * pointer with the same return value.
 *
 * This should be a per-architecture thing, to allow different
 * error and pointer decisions.
 */

// 所有的驱动程序都是运行在内核空间的。
// 内核空间虽然很大，但是总归有限。
// 在这有限的空间中，其最后一个page是专门保留的，
// 也就是说，一般人不可能用到内核空间最后一个page的指针。
//
// 在写设备驱动程序的过程中，涉及的任何一个指针，比然有三种情况：
// 1. 有效指针；2. NULL；3. 错误指针，或者说无效指针
// 所谓的错误指针就是指其到达了最后一个page。
// 比如对于32bit的系统来说，内核空间最高地址0xffffffff，那么最后一个page就是指0xfffff000~0xffffffff（4KB）
// 如果发现指针指向了这个范围，说明代码出错了。

#define MAX_ERRNO	4095

#ifndef __ASSEMBLY__

#define IS_ERR_VALUE(x) unlikely((x) >= (unsigned long)-MAX_ERRNO)

static inline void *ERR_PTR(long error)
{
	return (void *) error;
}

static inline long PTR_ERR(const void *ptr)
{
	return (long) ptr;
}

static inline long IS_ERR(const void *ptr)
{
   // 判断kthread_run()返回的指针是否有错。
   // 如果指针指向了最后一个page，说明这不是一个有效的指针，这个指针里保存的实际上是一种错误代码
	return IS_ERR_VALUE((unsigned long)ptr);
}

/**
 * ERR_CAST - Explicitly cast an error-valued pointer to another pointer type
 * @ptr: The pointer to cast.
 *
 * Explicitly cast an error-valued pointer to another pointer type in such a
 * way as to make it clear that's what's going on.
 */
static inline void *ERR_CAST(const void *ptr)
{
	/* cast away the const */
	return (void *) ptr;
}

#endif

#endif /* _LINUX_ERR_H */
