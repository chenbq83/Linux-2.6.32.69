#ifndef _ASM_X86_CURRENT_H
#define _ASM_X86_CURRENT_H

#include <linux/compiler.h>
#include <asm/percpu.h>

#ifndef __ASSEMBLY__
struct task_struct;

// 用于在编译时候声明一个perCPU变量。该变量被放在一个特殊的段中，
// 原型是DECLARE_PER_CPU(type, name)，主要作用是为处理器创建一个type类型，名为name的变量
DECLARE_PER_CPU(struct task_struct *, current_task);

static __always_inline struct task_struct *get_current(void)
{
   // current_task保存的就是当前进程的task_struct地址
   // 所以不需要通过ESP->thread_info->task_struct
	return percpu_read_stable(current_task);
}

// current宏返回的是当前进程的task_struct指针
#define current get_current()

#endif /* __ASSEMBLY__ */

#endif /* _ASM_X86_CURRENT_H */
