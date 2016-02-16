#ifndef _LINUX_STDDEF_H
#define _LINUX_STDDEF_H

#include <linux/compiler.h>

#undef NULL
#if defined(__cplusplus)
#define NULL 0
#else
#define NULL ((void *)0)
#endif

#ifdef __KERNEL__

enum {
	false	= 0,
	true	= 1
};

#undef offsetof
#ifdef __compiler_offsetof
#define offsetof(TYPE,MEMBER) __compiler_offsetof(TYPE,MEMBER)
#else
/*
 * 将地址0“看作”是一个type类型的指针，从而定位到member在结构体重的偏移位置
 * 编译器认为0是一个有效的地址，从而认为0是type结构体的起始地址
 */
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif
#endif /* __KERNEL__ */

#endif
