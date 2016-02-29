#ifndef _ASM_X86_PGTABLE_32_DEFS_H
#define _ASM_X86_PGTABLE_32_DEFS_H

/*
 * The Linux x86 paging architecture is 'compile-time dual-mode', it
 * implements both the traditional 2-level x86 page tables and the
 * newer 3-level PAE-mode page tables.
 */
#ifdef CONFIG_X86_PAE
# include <asm/pgtable-3level_types.h>
# define PMD_SIZE	(1UL << PMD_SHIFT)
# define PMD_MASK	(~(PMD_SIZE - 1))
#else
# include <asm/pgtable-2level_types.h>
#endif

#define PGDIR_SIZE	(1UL << PGDIR_SHIFT)
#define PGDIR_MASK	(~(PGDIR_SIZE - 1))

/* Just any arbitrary offset to the start of the vmalloc VM area: the
 * current 8MB value just means that there will be a 8MB "hole" after the
 * physical memory until the kernel virtual memory starts.  That means that
 * any out-of-bounds memory accesses will hopefully be caught.
 * The vmalloc() routines leaves a hole of 4kB between each vmalloced
 * area for the same reason. ;)
 */

/*
 * http://blog.chinaunix.net/uid-26859697-id-4592322.html
 *
 * 内核空间，分为直接内存映射区和高端内存映射区。
 * 直接内存映射区是指3G到3G+896M的线性空间，直接对应物理地址就是0到896M（前提是
 * 有超过896M的物理内存），其中896M是high_memory值，使用kmalloc/kfree接口操作申请释放。
 * 而高端内存映射区则是超过896M物理内存空间，它又分为动态映射区、永久映射区和
 * 固定映射区。
 *
 * 动态内存映射区，又称之为vmalloc映射区或非连续映射区，是指VMALLOC_START到
 * VMALLOC_END的地址空间，申请释放操作接口是vmalloc/vfree，通常用于将非连续的物理内存
 * 映射为连续的线性地址内存空间。
 *
 * 永久映射区，又称之为KMAP区或持久映射区，是值PKMAP_BASE开始共LAST_PKMAP个页面大小的空间，
 * 操作接口是kmap/kunmap，用于将高端内存长久映射到内存虚拟地址空间中。
 *
 * 固定映射区，称之为临时内核映射区，是指FIXADDR_START到FIXADDR_TOP的地址空间，操作接口是
 * kmap_atomic/kunmap_atomic，用于解决持久映射不能用于中断处理程序而增加的临时内核映射。
 */
#define VMALLOC_OFFSET	(8 * 1024 * 1024)

#ifndef __ASSEMBLER__
extern bool __vmalloc_start_set; /* set once high_memory is set */
#endif

// 动态内存映射区起始地址
#define VMALLOC_START	((unsigned long)high_memory + VMALLOC_OFFSET)
#ifdef CONFIG_X86_PAE
#define LAST_PKMAP 512
#else
// 永久映射空间的映射页面号
#define LAST_PKMAP 1024
#endif

// 永久映射空间的起始地址
#define PKMAP_BASE ((FIXADDR_BOOT_START - PAGE_SIZE * (LAST_PKMAP + 1))	\
		    & PMD_MASK)

#ifdef CONFIG_HIGHMEM
# define VMALLOC_END	(PKMAP_BASE - 2 * PAGE_SIZE)
#else
# define VMALLOC_END	(FIXADDR_START - 2 * PAGE_SIZE)
#endif

#define MODULES_VADDR	VMALLOC_START
#define MODULES_END	VMALLOC_END
#define MODULES_LEN	(MODULES_VADDR - MODULES_END)

#define MAXMEM	(VMALLOC_END - PAGE_OFFSET - __VMALLOC_RESERVE)

#endif /* _ASM_X86_PGTABLE_32_DEFS_H */
