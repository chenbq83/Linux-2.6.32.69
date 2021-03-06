/* Written 2000 by Andi Kleen */
#ifndef _ASM_X86_DESC_DEFS_H
#define _ASM_X86_DESC_DEFS_H

/*
 * Segment descriptor structure definitions, usable from both x86_64 and i386
 * archs.
 */

#ifndef __ASSEMBLY__

#include <linux/types.h>

/*
 * FIXME: Acessing the desc_struct through its fields is more elegant,
 * and should be the one valid thing to do. However, a lot of open code
 * still touches the a and b acessors, and doing this allow us to do it
 * incrementally. We keep the signature as a struct, rather than an union,
 * so we can get rid of it transparently in the future -- glommer
 */
/* 8 byte segment descriptor */
struct desc_struct {
	union {
		struct {
			unsigned int a;
			unsigned int b;
		};
		struct {
			u16 limit0;
			u16 base0;
			unsigned base1: 8, type: 4, s: 1, dpl: 2, p: 1;
			unsigned limit: 4, avl: 1, l: 1, d: 1, g: 1, base2: 8;
		};
	};
} __attribute__((packed));

#define GDT_ENTRY_INIT(flags, base, limit) { { { \
		.a = ((limit) & 0xffff) | (((base) & 0xffff) << 16), \
		.b = (((base) & 0xff0000) >> 16) | (((flags) & 0xf0ff) << 8) | \
			((limit) & 0xf0000) | ((base) & 0xff000000), \
	} } }

enum {
	GATE_INTERRUPT = 0xE,
	GATE_TRAP = 0xF,
	GATE_CALL = 0xC,
	GATE_TASK = 0x5,
};

/*
 * 门描述符，64位，主要保存了段选择符、权限位和中断处理程序入口地址。
 * CPU主要将门分为三种：任务门，中断门，陷阱门
 * 但是Linux为了处理更多种情况，把门分为五种：中断门，系统门，系统中断门，陷阱门，任务门
 *
 * 当CPU通过中断门找到一个代码段描述项（GDT表的一项），并进而转入相应的服务程序时，
 * 就把这个代码段描述项装入CPU中，而描述项的DPL（GDT表项中的DPL）就变成CPU的当前运行级别，称为CPL
 *
 * 而中断描述符中也有一个DPL，也就是下面定义的dpl，是用来对运行和访问级别进行检查对比的。
 * 当通过一条int指令进入一个中断服务程序时，在指令中给出了一个中断向量。
 * CPU根据该向量在中断向量表中找到一扇门，然后将这个门的DPL和CPU的CPL相比，CPL必须小于或等于DPL，
 * 才能穿过这扇门。
 * 不过，如果中断是由外部产生或是因为CPU异常而产生的话，那就免了这一层检查。
 */
/* 16byte gate */
struct gate_struct64 {
	u16 offset_low;
	u16 segment;
   // p：表示段是否在内存中，因为Linux从不把整个段交换到硬盘上，所以p都被置为1
   // dpl：表示权限，用于限制对这个段的存取。
   //   当为0时，只有cpl=0（内核态）才能够访问这个段。
   //   当为3时，任何等级的cpl（用户态或者内核态）都可以访问
	unsigned ist : 3, zero0 : 5, type : 5, dpl : 2, p : 1;
	u16 offset_middle;
	u32 offset_high;
	u32 zero1;
} __attribute__((packed));

#define PTR_LOW(x) ((unsigned long long)(x) & 0xFFFF)
#define PTR_MIDDLE(x) (((unsigned long long)(x) >> 16) & 0xFFFF)
#define PTR_HIGH(x) ((unsigned long long)(x) >> 32)

enum {
	DESC_TSS = 0x9,
	DESC_LDT = 0x2,
	DESCTYPE_S = 0x10,	/* !system */
};

/* LDT or TSS descriptor in the GDT. 16 bytes. */
struct ldttss_desc64 {
	u16 limit0;
	u16 base0;
	unsigned base1 : 8, type : 5, dpl : 2, p : 1;
	unsigned limit1 : 4, zero0 : 3, g : 1, base2 : 8;
	u32 base3;
	u32 zero1;
} __attribute__((packed));

#ifdef CONFIG_X86_64
typedef struct gate_struct64 gate_desc;
typedef struct ldttss_desc64 ldt_desc;
typedef struct ldttss_desc64 tss_desc;
#define gate_offset(g) ((g).offset_low | ((unsigned long)(g).offset_middle << 16) | ((unsigned long)(g).offset_high << 32))
#define gate_segment(g) ((g).segment)
#else
typedef struct desc_struct gate_desc;
typedef struct desc_struct ldt_desc;
typedef struct desc_struct tss_desc;
#define gate_offset(g)		(((g).b & 0xffff0000) | ((g).a & 0x0000ffff))
#define gate_segment(g)		((g).a >> 16)
#endif

struct desc_ptr {
	unsigned short size;
	unsigned long address;
} __attribute__((packed)) ;

#endif /* !__ASSEMBLY__ */

#endif /* _ASM_X86_DESC_DEFS_H */
