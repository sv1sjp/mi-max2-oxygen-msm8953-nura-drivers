// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * adspdiag: race-free DIAG client for the msm8953 ADSP (Xiaomi Mi Max 2 "oxygen").
 *
 * Bind (driver_override) to the ADSP's SMD channels DIAG and DIAG_CNTL.
 * rpmsg_char cannot be used for them: rpmsg_dev_probe() opens the SMD channel with
 * priv == NULL and rpmsg_chrdev_probe() only sets priv afterwards, while the ADSP
 * sends its first control packet as soon as DIAG_CNTL opens -> rpmsg_ept_cb()
 * dereferences NULL in IRQ context -> panic (this reset the phone on 2026-10-17).
 * This driver's callback never uses priv.
 *
 * DIAG_CNTL: answered in the kernel. On the ADSP's first control packets it sends
 *   feature mask (cmd 8, byte 0 must be non-zero or the ADSP drops all output), diag mode (cmd 3, real time) and, per SSID range, an F3
 *   mask (cmd 11) plus an F3 mask with preset id (cmd 16, presets 1 and 2), status 3,
 *   all bits on. The ADSP's handlers only update the range whose first SSID matches,
 *   so every range needs its own packets (AFE = SSID 8500).
 * DIAG: raw bytes from the ADSP go to a 1 MiB FIFO, read from
 *   /sys/kernel/debug/adspdiag/data (blocking). Writes there go to the ADSP.
 * /sys/kernel/debug/adspdiag/cntl: write one raw control packet to the ADSP.
 * /sys/kernel/debug/adspdiag/stats: control packet counts and byte counters.
 * Stop any reader of data before unloading (debugfs removal waits for it).
 */
#include <linux/debugfs.h>
#include <linux/kfifo.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/rpmsg.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/soc/qcom/smem.h>
#include <linux/wait.h>
#include <linux/unaligned.h>
#include <linux/workqueue.h>

static const u16 ssid_ranges[][2] = {
	{ 0, 111 }, { 500, 506 }, { 1000, 1007 }, { 2000, 2008 }, { 3000, 3014 },
	{ 4000, 4010 }, { 4500, 4526 }, { 4600, 4615 }, { 5000, 5030 }, { 5500, 5516 },
	{ 6000, 6080 }, { 6500, 6521 }, { 7000, 7003 }, { 7100, 7111 }, { 7200, 7201 },
	{ 8000, 8000 }, { 8500, 8524 }, { 9000, 9008 }, { 9500, 9510 }, { 10200, 10210 },
	{ 10251, 10255 }, { 10300, 10300 }, { 10350, 10374 },
};

#define CNTL_CMDS 40

static DECLARE_KFIFO_PTR(rx_fifo, u8);
static DECLARE_WAIT_QUEUE_HEAD(rx_wq);
static DEFINE_MUTEX(read_lock);
static DEFINE_MUTEX(cntl_lock);	/* cntl_write vs remove */
static struct rpmsg_device *cntl_rpdev, *data_rpdev;
static atomic_t cntl_count[CNTL_CMDS];
static atomic_t rx_bytes, rx_dropped, cntl_bytes, masks_sent, send_errors;
static atomic_t masks_wanted;
static bool cntl_active;	/* set by probe, cleared by remove: gates all sends */
static struct dentry *dbg_dir;

static void masks_work_fn(struct work_struct *work);
static DECLARE_DELAYED_WORK(masks_work, masks_work_fn);

static int cntl_send(struct rpmsg_endpoint *ept, u32 cmd, const void *payload, u32 len)
{
	u8 *buf;
	int ret;

	buf = kmalloc(8 + len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	put_unaligned_le32(cmd, buf);
	put_unaligned_le32(len, buf + 4);
	memcpy(buf + 8, payload, len);
	ret = rpmsg_send(ept, buf, 8 + len);
	kfree(buf);
	if (ret)
		atomic_inc(&send_errors);
	return ret;
}

static void masks_work_fn(struct work_struct *work)
{
	struct rpmsg_device *rpdev = READ_ONCE(cntl_rpdev);
	struct rpmsg_endpoint *ept;
	/*
	 * feature_len 4, byte 0 = 0x01. The ADSP's diag drain (0xf01b729c) DROPS all outgoing
	 * data while apps feature byte 0 is 0. Bit 6 (apps HDLC encoding) stays off, so the
	 * ADSP keeps HDLC-framing its output.
	 */
	static const u8 feature[8] = { 4, 0, 0, 0, 1, 0, 0, 0 };
	__le32 mode[9];
	u8 *f3;
	int i, p;

	if (!READ_ONCE(cntl_active) || !rpdev)
		return;
	ept = rpdev->ept;	/* stored by rpmsg_dev_probe() before our probe ran */
	atomic_set(&masks_wanted, 0);

	cntl_send(ept, 8, feature, sizeof(feature));
	/* version 1, sleep_vote 1, real_time 1, rest 0 (the ADSP requires 36 bytes) */
	memset(mode, 0, sizeof(mode));
	mode[0] = cpu_to_le32(1);
	mode[1] = cpu_to_le32(1);
	mode[2] = cpu_to_le32(1);
	cntl_send(ept, 3, mode, sizeof(mode));

	f3 = kmalloc(12 + 4 * 128, GFP_KERNEL);
	if (!f3)
		return;
	for (i = 0; i < ARRAY_SIZE(ssid_ranges); i++) {
		u16 first = ssid_ranges[i][0], last = ssid_ranges[i][1];
		u32 n = last - first + 1;

		f3[0] = 1;	/* stream 1 */
		f3[1] = 3;	/* status 3 = per-SSID masks */
		f3[2] = 0;	/* msg_mode */
		put_unaligned_le16(first, f3 + 3);
		put_unaligned_le16(last, f3 + 5);
		put_unaligned_le32(n, f3 + 7);
		memset(f3 + 11, 0xff, 4 * n);
		if (!cntl_send(ept, 11, f3, 11 + 4 * n))
			atomic_inc(&masks_sent);
	}
	/*
	 * cmd 11 only fills the table behind the "any mask set" flag; the per-message
	 * lookup reads the per-preset table (entry + 0x10, preset DAT_f0825ca2, default 1),
	 * which only cmd 16 (F3 mask with preset id, handler 0xf01bc500) writes:
	 * stream, preset, status, msg_mode, u16 ssid_first, u16 pad, u32 range_len, masks;
	 * data_len must be range_len * 4 + 12.
	 */
	for (p = 1; p <= 2; p++) {
		for (i = 0; i < ARRAY_SIZE(ssid_ranges); i++) {
			u16 first = ssid_ranges[i][0], last = ssid_ranges[i][1];
			u32 n = last - first + 1;

			f3[0] = 1;	/* stream 1 */
			f3[1] = p;	/* preset id */
			f3[2] = 3;	/* status 3 = per-SSID masks */
			f3[3] = 0;	/* msg_mode */
			put_unaligned_le16(first, f3 + 4);
			put_unaligned_le16(last, f3 + 6);
			put_unaligned_le32(n, f3 + 8);
			memset(f3 + 12, 0xff, 4 * n);
			if (!cntl_send(ept, 16, f3, 12 + 4 * n))
				atomic_inc(&masks_sent);
		}
	}
	kfree(f3);
}

/* Runs in IRQ context; must not use priv (see the comment at the top). */
static int adspdiag_cb(struct rpmsg_device *rpdev, void *buf, int len, void *priv, u32 addr)
{
	if (!strcmp(rpdev->id.name, "DIAG_CNTL")) {
		u8 *p = buf;
		int o = 0;

		atomic_add(len, &cntl_bytes);
		while (o + 8 <= len) {
			u32 cmd = get_unaligned_le32(p + o), l = get_unaligned_le32(p + o + 4);

			atomic_inc(&cntl_count[cmd < CNTL_CMDS ? cmd : 0]);
			/* feature mask, SSID range / build mask report: (re)send our masks */
			if ((cmd == 8 || cmd == 24 || cmd == 25) && READ_ONCE(cntl_active) &&
			    !atomic_xchg(&masks_wanted, 1))
				schedule_delayed_work(&masks_work, 0);
			if (l > len - o - 8)
				break;
			o += 8 + l;
		}
		return 0;
	}

	atomic_add(len, &rx_bytes);
	if (kfifo_in(&rx_fifo, (u8 *)buf, len) < len)
		atomic_inc(&rx_dropped);
	wake_up_interruptible(&rx_wq);
	return 0;
}

static ssize_t data_read(struct file *f, char __user *ubuf, size_t cnt, loff_t *ppos)
{
	unsigned int copied;
	int ret;

	if (kfifo_is_empty(&rx_fifo)) {
		if (f->f_flags & O_NONBLOCK)
			return -EAGAIN;
		ret = wait_event_interruptible(rx_wq, !kfifo_is_empty(&rx_fifo));
		if (ret)
			return ret;
	}
	mutex_lock(&read_lock);
	ret = kfifo_to_user(&rx_fifo, ubuf, cnt, &copied);
	mutex_unlock(&read_lock);
	return ret ? ret : copied;
}

static ssize_t data_write(struct file *f, const char __user *ubuf, size_t cnt, loff_t *ppos)
{
	struct rpmsg_device *rpdev = READ_ONCE(data_rpdev);
	void *buf;
	int ret;

	if (!rpdev || !rpdev->ept)
		return -ENODEV;
	buf = memdup_user(ubuf, min_t(size_t, cnt, 4096));
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	ret = rpmsg_send(rpdev->ept, buf, min_t(size_t, cnt, 4096));
	kfree(buf);
	return ret ? ret : min_t(size_t, cnt, 4096);
}

/* Write one raw control packet (cmd, len, payload) to the ADSP. */
static ssize_t cntl_write(struct file *f, const char __user *ubuf, size_t cnt, loff_t *ppos)
{
	void *buf;
	int ret = -ENODEV;

	if (cnt > 4096)
		return -EINVAL;
	buf = memdup_user(ubuf, cnt);
	if (IS_ERR(buf))
		return PTR_ERR(buf);
	mutex_lock(&cntl_lock);
	if (cntl_active && cntl_rpdev)
		ret = rpmsg_send(cntl_rpdev->ept, buf, cnt);
	mutex_unlock(&cntl_lock);
	kfree(buf);
	return ret ? ret : cnt;
}

static const struct file_operations cntl_fops = {
	.owner = THIS_MODULE,
	.write = cntl_write,
	.llseek = noop_llseek,
};

static const struct file_operations data_fops = {
	.owner = THIS_MODULE,
	.read = data_read,
	.write = data_write,
	.llseek = noop_llseek,
};

static int stats_show(struct seq_file *s, void *unused)
{
	int i;

	seq_printf(s, "cntl %s, data %s\n", cntl_rpdev ? "open" : "-", data_rpdev ? "open" : "-");
	seq_printf(s, "cntl_bytes %d masks_sent %d send_errors %d\n", atomic_read(&cntl_bytes),
		   atomic_read(&masks_sent), atomic_read(&send_errors));
	seq_printf(s, "rx_bytes %d rx_dropped %d fifo %u\n", atomic_read(&rx_bytes),
		   atomic_read(&rx_dropped), kfifo_len(&rx_fifo));
	seq_puts(s, "cntl cmds:");
	for (i = 0; i < CNTL_CMDS; i++)
		if (atomic_read(&cntl_count[i]))
			seq_printf(s, " %d:%d", i, atomic_read(&cntl_count[i]));
	seq_putc(s, '\n');
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(stats);

/*
 * Read-only view of the SMD channel bookkeeping in SMEM (layout as in
 * drivers/rpmsg/qcom_smd.c): for every DIAG* channel on the APPS<->QDSP edge,
 * print state, signals and FIFO head/tail of both directions. "tx" = apps->ADSP.
 */
struct smd_alloc_entry {
	u8 name[20];
	__le32 cid;
	__le32 flags;
	__le32 ref_count;
} __packed;

struct smd_info {
	__le32 state;
	u8 fDSR, fCTS, fCD, fRI, fHEAD, fTAIL, fSTATE, fBLOCKREADINTR;
	__le32 tail;
	__le32 head;
} __packed;

struct smd_info_word {
	__le32 state;
	__le32 fDSR, fCTS, fCD, fRI, fHEAD, fTAIL, fSTATE, fBLOCKREADINTR;
	__le32 tail;
	__le32 head;
} __packed;

static const struct { unsigned int tbl, info; } smd_items[] = { { 13, 14 }, { 266, 138 } };
static unsigned int smd_host = 2;	/* qcom,remote-pid of the ADSP edge */
module_param(smd_host, uint, 0644);

static void smd_show_info(struct seq_file *s, const char *dir, const void *p, bool word)
{
	if (word) {
		const struct smd_info_word *i = p;

		seq_printf(s, "  %s state %u DSR %u CTS %u CD %u RI %u HEAD %u TAIL %u STATE %u tail %u head %u\n",
			   dir, le32_to_cpu(i->state), le32_to_cpu(i->fDSR), le32_to_cpu(i->fCTS),
			   le32_to_cpu(i->fCD), le32_to_cpu(i->fRI), le32_to_cpu(i->fHEAD),
			   le32_to_cpu(i->fTAIL), le32_to_cpu(i->fSTATE), le32_to_cpu(i->tail),
			   le32_to_cpu(i->head));
	} else {
		const struct smd_info *i = p;

		seq_printf(s, "  %s state %u DSR %u CTS %u CD %u RI %u HEAD %u TAIL %u STATE %u tail %u head %u\n",
			   dir, le32_to_cpu(i->state), i->fDSR, i->fCTS, i->fCD, i->fRI, i->fHEAD,
			   i->fTAIL, i->fSTATE, le32_to_cpu(i->tail), le32_to_cpu(i->head));
	}
}

static int smd_show(struct seq_file *s, void *unused)
{
	int t;

	for (t = 0; t < ARRAY_SIZE(smd_items); t++) {
		struct smd_alloc_entry *e;
		size_t size;
		int i, n;

		e = qcom_smem_get(smd_host, smd_items[t].tbl, &size);
		if (IS_ERR(e))
			e = qcom_smem_get(QCOM_SMEM_HOST_ANY, smd_items[t].tbl, &size);
		seq_printf(s, "alloc tbl %u: %ld entries\n", smd_items[t].tbl,
			   IS_ERR(e) ? PTR_ERR(e) : (long)(size / sizeof(*e)));
		if (IS_ERR(e))
			continue;
		n = size / sizeof(*e);
		for (i = 0; i < n; i++) {
			u32 cid = le32_to_cpu(e[i].cid), flags = le32_to_cpu(e[i].flags);
			char name[21];
			void *info;
			size_t isz;

			memcpy(name, e[i].name, 20);
			name[20] = 0;
			if (!flags || strncmp(name, "DIAG", 4))
				continue;
			info = qcom_smem_get(smd_host, smd_items[t].info + cid, &isz);
			if (IS_ERR(info))
				info = qcom_smem_get(QCOM_SMEM_HOST_ANY, smd_items[t].info + cid, &isz);
			seq_printf(s, "%s tbl %d cid %u flags %#x info %zu\n", name, t, cid, flags,
				   IS_ERR(info) ? 0 : isz);
			if (IS_ERR(info))
				continue;
			if (isz == 2 * sizeof(struct smd_info_word)) {
				smd_show_info(s, "tx", info, true);
				smd_show_info(s, "rx", info + sizeof(struct smd_info_word), true);
			} else if (isz == 2 * sizeof(struct smd_info)) {
				smd_show_info(s, "tx", info, false);
				smd_show_info(s, "rx", info + sizeof(struct smd_info), false);
			}
		}
	}
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(smd);

static int adspdiag_probe(struct rpmsg_device *rpdev)
{
	dev_info(&rpdev->dev, "adspdiag bound to %s\n", rpdev->id.name);
	if (!strcmp(rpdev->id.name, "DIAG_CNTL")) {
		WRITE_ONCE(cntl_rpdev, rpdev);
		WRITE_ONCE(cntl_active, true);
		if (!atomic_xchg(&masks_wanted, 1))
			schedule_delayed_work(&masks_work, msecs_to_jiffies(50));
	} else {
		WRITE_ONCE(data_rpdev, rpdev);
	}
	return 0;
}

static void adspdiag_remove(struct rpmsg_device *rpdev)
{
	if (rpdev == cntl_rpdev) {
		mutex_lock(&cntl_lock);
		WRITE_ONCE(cntl_active, false);
		mutex_unlock(&cntl_lock);
		cancel_delayed_work_sync(&masks_work);
		WRITE_ONCE(cntl_rpdev, NULL);
	} else if (rpdev == data_rpdev) {
		WRITE_ONCE(data_rpdev, NULL);
	}
}

static struct rpmsg_driver adspdiag_driver = {
	.probe = adspdiag_probe,
	.remove = adspdiag_remove,
	.callback = adspdiag_cb,
	.drv.name = "adspdiag",
};

static int __init adspdiag_init(void)
{
	int ret;

	ret = kfifo_alloc(&rx_fifo, 1 << 20, GFP_KERNEL);
	if (ret)
		return ret;
	dbg_dir = debugfs_create_dir("adspdiag", NULL);
	debugfs_create_file("data", 0600, dbg_dir, NULL, &data_fops);
	debugfs_create_file("stats", 0400, dbg_dir, NULL, &stats_fops);
	debugfs_create_file("cntl", 0200, dbg_dir, NULL, &cntl_fops);
	debugfs_create_file("smd", 0400, dbg_dir, NULL, &smd_fops);
	ret = register_rpmsg_driver(&adspdiag_driver);
	if (ret) {
		debugfs_remove_recursive(dbg_dir);
		kfifo_free(&rx_fifo);
	}
	return ret;
}

static void __exit adspdiag_exit(void)
{
	unregister_rpmsg_driver(&adspdiag_driver);
	cancel_delayed_work_sync(&masks_work);
	debugfs_remove_recursive(dbg_dir);
	kfifo_free(&rx_fifo);
}

module_init(adspdiag_init);
module_exit(adspdiag_exit);
MODULE_DESCRIPTION("Race-free DIAG client for the msm8953 ADSP");
MODULE_LICENSE("GPL");
