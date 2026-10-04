// SPDX-License-Identifier: GPL-2.0
/*
 * Debug only: snapshot an MMIO range (default: msm8953 LPASS clock controller
 * 0x0c000000, 0x14000 bytes) into /sys/kernel/debug/regsnap/<tag>.bin.
 * Read-only on the hardware. Stays loaded until rmmod (frees the blob).
 * usage: insmod regsnap.ko tag=quat; cat /sys/kernel/debug/regsnap/quat.bin > f; rmmod regsnap
 */
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/vmalloc.h>

static unsigned int addr = 0x0c000000;
module_param(addr, uint, 0444);
static unsigned int size = 0x14000;
module_param(size, uint, 0444);
static char *tag = "snap";
module_param(tag, charp, 0444);

static struct dentry *dir;
static struct debugfs_blob_wrapper blob;

static int __init regsnap_init(void)
{
	void __iomem *base;
	u32 *buf;
	unsigned int i;
	char name[64];

	if (!size || size > 0x100000 || (size & 3))
		return -EINVAL;
	base = ioremap(addr, size);
	if (!base)
		return -ENOMEM;
	buf = vmalloc(size);
	if (!buf) {
		iounmap(base);
		return -ENOMEM;
	}
	for (i = 0; i < size / 4; i++)
		buf[i] = readl(base + i * 4);
	iounmap(base);

	blob.data = buf;
	blob.size = size;
	dir = debugfs_create_dir("regsnap", NULL);
	snprintf(name, sizeof(name), "%s.bin", tag);
	debugfs_create_blob(name, 0400, dir, &blob);
	pr_info("regsnap: %s: 0x%08x + 0x%x captured\n", tag, addr, size);
	return 0;
}

static void __exit regsnap_exit(void)
{
	debugfs_remove_recursive(dir);
	vfree(blob.data);
}
module_init(regsnap_init);
module_exit(regsnap_exit);
MODULE_DESCRIPTION("debug: snapshot an MMIO range to debugfs");
MODULE_LICENSE("GPL");
