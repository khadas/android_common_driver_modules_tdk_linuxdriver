/*
 * Copyright (c) 2017, Amlogic, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/sysfs.h>
#include <linux/kthread.h>
#include <generated/uapi/linux/version.h>
#include <linux/workqueue.h>

#include "optee_smc.h"
#include "optee_private.h"
#include "../tee_private.h"

//#define OPTEE_LOG_BUFFER_DEBUG      1

#define OPTEE_LOG_SHM_ADDR_MAGIC    0x1001
#define OPTEE_LOG_BUFFER_MAGIC      0xAA00AA00
#define OPTEE_LOG_BUFFER_OFFSET     0x00000080
#define OPTEE_LOG_READ_MAX          PAGE_SIZE
#define OPTEE_LOG_LINE_MAX          1024
#define OPTEE_LOG_TIMER_INTERVAL    1

#undef pr_fmt
#define pr_fmt(fmt) "[TEE] " fmt

struct optee_log_ctl_s {
	unsigned int magic;
	unsigned int inited;
	unsigned int total_size;
	unsigned int fill_size;
	unsigned int mode;
	unsigned int reader;
	unsigned int writer;

	unsigned char *buffer;
};

static struct optee_log_ctl_s *optee_log_ctl;
static unsigned char *optee_log_buff;
static uint32_t optee_log_mode = 1;
static uint8_t line_buff[OPTEE_LOG_LINE_MAX];
static uint32_t looped = 0;
static void *g_shm_va;

struct delayed_work log_work;
static struct workqueue_struct *log_workqueue = NULL;

static bool init_shm(phys_addr_t shm_pa, uint32_t shm_size)
{
	struct arm_smccc_res smccc = { 0 };
	uint32_t start = 1;

	arm_smccc_smc(OPTEE_SMC_SET_LOGGER, start, shm_pa, shm_size, 0, 0, 0, 0,
			&smccc);

	if (smccc.a0 != TEEC_SUCCESS) {
		pr_err("smc set logger failed, err = 0x%x\n", smccc.a0);
		return false;
	}

	if (smccc.a1 == OPTEE_LOG_SHM_ADDR_MAGIC) {
		shm_pa = smccc.a2;
		shm_size = smccc.a3;
	}

/* pfn_valid returns incorrect value in kernel 5.15 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 15, 0)
#else
	if (pfn_valid(__phys_to_pfn(shm_pa)))
		g_shm_va = (void __iomem *)__phys_to_virt(shm_pa);
	else
#endif
		g_shm_va = ioremap_cache(shm_pa, shm_size);

	if (!g_shm_va) {
		pr_err("map logger share-mem failed\n");
		return false;
	}

	return (0 == smccc.a0);
}

static void uninit_shm(void)
{
	struct arm_smccc_res smccc;
	uint32_t start = 0;

	if (g_shm_va)
		iounmap(g_shm_va);

	arm_smccc_smc(OPTEE_SMC_SET_LOGGER, start, 0, 0, 0, 0, 0, 0, &smccc);
}

static ssize_t log_buff_get_read_buff(char **buf, int len)
{
	int writer;
	int reader;
	int read_size = 0;
	struct optee_log_ctl_s *ctl = optee_log_ctl;

	if ((!ctl) || (len <= 0))
		return 0;

	writer = ctl->writer;
	reader = ctl->reader;

	if (reader == writer)
		read_size = 0;
	else if (reader < writer)
		read_size = writer - reader;
	else {
		looped = 1;
		read_size = ctl->total_size - reader;
	}

	if (read_size > len)
		read_size = len;

	*buf = optee_log_buff + reader;
	ctl->reader += read_size;
	if (ctl->reader == ctl->total_size)
		ctl->reader = 0;

	return read_size;
}
/* not defined in kernel 5.15 s4d */
void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;

	while (n-- != 0) {
		if ((unsigned char)c == *p++) {
			return (void *)(p - 1);
		}
	}

	return NULL;
}

static size_t log_print_text(char *buf, size_t size)
{
	const char *text = buf;
	int32_t remaining = size;

	char *line = line_buff;
	const char *next = NULL;
	size_t line_size = 0;
	size_t scan_size = 0;

	if (!buf || !size)
		return 0;

	while (text && remaining > 0) {
		/* Reserve 2 bytes for EOL and EOS */
		scan_size = remaining > (OPTEE_LOG_LINE_MAX - 2)?
			(OPTEE_LOG_LINE_MAX - 2): remaining;
		next = memchr(text, '\n', scan_size);
		if (next) {
			/* EOL is found */
			next++;
			line_size = next - text;
			/* Add a extra EOS */
			line[line_size] = '\0';
		} else {
			/* No EOL found.*/
			line_size = scan_size;
			/* Truncate string to scan_size and add EOL and EOS. */
			line[line_size] = '\n';
			line[line_size + 1] = '\0';
		}
		memcpy(line, text, line_size);
		remaining -= line_size;
		text += line_size;
		pr_notice("%s", line);
	}
	/* All remaining should be consumed */
	if (!text || remaining) {
		pr_err("WARNING: text(%p) is NULL or remaining(%d) is not 0.\n",
				text, remaining);
	}
	return size;
}

static void log_buff_output(void)
{
	size_t len;
	char *read_buff = NULL;

	if (optee_log_mode == 0)
		return;

	len = log_buff_get_read_buff(&read_buff, OPTEE_LOG_READ_MAX);
	if (len > 0)
		log_print_text(read_buff, len);
}

static void do_log_timer(struct work_struct *work)
{
	log_buff_output();
	if (queue_delayed_work(log_workqueue, &log_work, OPTEE_LOG_TIMER_INTERVAL * HZ) == 0) {
		pr_err("%s:%d Failed to join the workqueue\n", __func__, __LINE__);
	}
}

int optee_log_init(struct tee_device *tee_dev, phys_addr_t shm_pa,
		uint32_t shm_size)
{
	int rc = 0;

	if (!init_shm(shm_pa, shm_size))
		return -EACCES;

	optee_log_buff = (unsigned char *)(g_shm_va + OPTEE_LOG_BUFFER_OFFSET);
	optee_log_ctl = (struct optee_log_ctl_s *)g_shm_va;
	if ((optee_log_ctl->magic != OPTEE_LOG_BUFFER_MAGIC)
		|| (optee_log_ctl->inited != 1)) {
		uninit_shm();
		optee_log_ctl = NULL;
		rc = -EINVAL;
		pr_err("tee log buffer init failed\n");
		goto err;
	}
	optee_log_ctl->mode = optee_log_mode;

	/* init workqueue */
	log_workqueue = create_singlethread_workqueue("tee-log-wq");
	INIT_DELAYED_WORK(&log_work,do_log_timer);
	if (queue_delayed_work(log_workqueue, &log_work, OPTEE_LOG_TIMER_INTERVAL * HZ) == 0) {
		pr_err("%s:%d Failed to join the workqueue.\n", __func__, __LINE__);
	}

err:
	return rc;
}

void optee_log_exit(struct tee_device *tee_dev)
{

	if (log_workqueue) {
		cancel_delayed_work_sync(&log_work);
		destroy_workqueue(log_workqueue);
	}

	uninit_shm();
}
