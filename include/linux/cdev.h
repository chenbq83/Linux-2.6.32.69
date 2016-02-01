#ifndef _LINUX_CDEV_H
#define _LINUX_CDEV_H

#include <linux/kobject.h>
#include <linux/kdev_t.h>
#include <linux/list.h>

struct file_operations;
struct inode;
struct module;

// 现实中一个具体的字符硬件设备的数据结构的抽象往往要复杂得多，
// 在这种情况下，cdev常常作为一种内嵌的成员变量出现在实际设备的数据结构中

struct cdev {
   // 内嵌的内核对象
	struct kobject kobj;
	struct module *owner;
   // 字符设备驱动程序中一个及其关键的数据结构
   // 在应用程序通过文件系统接口呼叫到设备驱动程序中实现的文件操作类函数的过程中，
   // 它起着桥梁纽带的作用
	const struct file_operations *ops;
   // 用来将系统中的字符设备形成链表
	struct list_head list;
   // 字符设备的设备号，包括了主设备号和次设备号
	dev_t dev;
   // 隶属于同意主设备号的次设备号的个数。
   // 用于表示由当前设备驱动程序控制的实际同类设备的数量
	unsigned int count;
};

void cdev_init(struct cdev *, const struct file_operations *);

struct cdev *cdev_alloc(void);

void cdev_put(struct cdev *p);

int cdev_add(struct cdev *, dev_t, unsigned);

void cdev_del(struct cdev *);

int cdev_index(struct inode *inode);

void cd_forget(struct inode *);

extern struct backing_dev_info directly_mappable_cdev_bdi;

#endif
