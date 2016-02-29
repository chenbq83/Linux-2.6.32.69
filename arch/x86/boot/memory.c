/* -*- linux-c -*- ------------------------------------------------------- *
 *
 *   Copyright (C) 1991, 1992 Linus Torvalds
 *   Copyright 2007 rPath, Inc. - All Rights Reserved
 *   Copyright 2009 Intel Corporation; author H. Peter Anvin
 *
 *   This file is part of the Linux kernel, and is made available under
 *   the terms of the GNU General Public License version 2.
 *
 * ----------------------------------------------------------------------- */

/*
 * Memory detection code
 */

#include "boot.h"

#define SMAP	0x534d4150	/* ASCII "SMAP" */

static int detect_memory_e820(void)
{
	int count = 0;
	struct biosregs ireg, oreg;
	struct e820entry *desc = boot_params.e820_map;
	static struct e820entry buf; /* static so it is zeroed */

	initregs(&ireg);
	ireg.ax  = 0xe820;
	ireg.cx  = sizeof buf;
	ireg.edx = SMAP;
	ireg.di  = (size_t)&buf;

	/*
	 * Note: at least one BIOS is known which assumes that the
	 * buffer pointed to by one e820 call is the same one as
	 * the previous call, and only changes modified fields.  Therefore,
	 * we use a temporary buffer and copy the results entry by entry.
	 *
	 * This routine deliberately does not try to account for
	 * ACPI 3+ extended attributes.  This is because there are
	 * BIOSes in the field which report zero for the valid bit for
	 * all ranges, and we don't currently make any use of the
	 * other attribute bits.  Revisit this if we see the extended
	 * attribute bits deployed in a meaningful way in the future.
	 */

	do {
      /*
       * http://blog.chinaunix.net/uid-26859697-id-4390055.html
       *
       * 循环调用BIOS的0x15中断的功能。 0x15是中断向量，输入参数是ireg，输出参数是oreg
       * ireg.ax赋值为0xe820（这就是著名的e820的由来）
       * 所谓的e820是指在x86的机器上，由BIOS提供的0x15中断去获取内存布局，
       * 其中中断调用时，AX寄存器必须为0xe820，中断调用后将返回被BIOS保留内存地址范围
       * 以及系统可以使用的内存地址范围。
       * 所有通过中断获取的数据将会填充在boot_params.e820_map中，也就是著名的e820图了。
       *
       * 输入：
       * EAX=0xe820
       * EBX 用来表示读取信息的index，初始值为0，中断后返回该寄存器用来下次要获取的信号的序号
       * ES:DI 用来保存信息的buffer地址
       * ECX buffer空间的大小
       * EDX 入参签名，必须为SMAP
       *
       * 输出：
       * CF 如果flag寄存器中的CF被置位表示调用出错
       * EAX 用来返回SMAP，否则表示出错
       * ES:DI 对应的buffer，里面存放获取到的信息
       * ECX BIOS在buffer中存放数据的大小
       * EBX BIOS返回的下次调用的序号，如果返回0，则表示无后续信息
       */
      
		intcall(0x15, &ireg, &oreg);
		ireg.ebx = oreg.ebx; /* for next iteration... */

		/* BIOSes which terminate the chain with CF = 1 as opposed
		   to %ebx = 0 don't always report the SMAP signature on
		   the final, failing, probe. */
		if (oreg.eflags & X86_EFLAGS_CF)
         // EFLAGS中的CF被置位，表示调用出错
			break;

		/* Some BIOSes stop returning SMAP in the middle of
		   the search loop.  We don't know exactly how the BIOS
		   screwed up the map at that point, we might have a
		   partial map, the full map, or complete garbage, so
		   just return failure. */
		if (oreg.eax != SMAP) {
         // EAX必须返回SMAP，否则表示出错
			count = 0;
			break;
		}

		*desc++ = buf;
		count++;
	} while (ireg.ebx && count < ARRAY_SIZE(boot_params.e820_map));
   // EBX返回的是下次调用的序号，如果为0，表示无后续信息
   // 或者是，读取的信息已经填满了e820_map

	return boot_params.e820_entries = count;
}

static int detect_memory_e801(void)
{
	struct biosregs ireg, oreg;

	initregs(&ireg);
	ireg.ax = 0xe801;
	intcall(0x15, &ireg, &oreg);

	if (oreg.eflags & X86_EFLAGS_CF)
		return -1;

	/* Do we really need to do this? */
	if (oreg.cx || oreg.dx) {
		oreg.ax = oreg.cx;
		oreg.bx = oreg.dx;
	}

	if (oreg.ax > 15*1024) {
		return -1;	/* Bogus! */
	} else if (oreg.ax == 15*1024) {
		boot_params.alt_mem_k = (oreg.dx << 6) + oreg.ax;
	} else {
		/*
		 * This ignores memory above 16MB if we have a memory
		 * hole there.  If someone actually finds a machine
		 * with a memory hole at 16MB and no support for
		 * 0E820h they should probably generate a fake e820
		 * map.
		 */
		boot_params.alt_mem_k = oreg.ax;
	}

	return 0;
}

static int detect_memory_88(void)
{
	struct biosregs ireg, oreg;

	initregs(&ireg);
	ireg.ah = 0x88;
	intcall(0x15, &ireg, &oreg);

	boot_params.screen_info.ext_mem_k = oreg.ax;

	return -(oreg.eflags & X86_EFLAGS_CF); /* 0 or -1 */
}

// 实模式下探测物理内存布局
// 此时尚未进入保护模式
int detect_memory(void)
{
	int err = -1;

   // 较新的电脑（？）调用detect_memory_e820足以探测内存布局
   // detect_memory_e801和detect_memory_88是针对较老的电脑进行兼容而保留的
	if (detect_memory_e820() > 0)
		err = 0;

	if (!detect_memory_e801())
		err = 0;

	if (!detect_memory_88())
		err = 0;

	return err;
}
