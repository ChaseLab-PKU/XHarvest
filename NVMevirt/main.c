// SPDX-License-Identifier: GPL-2.0-only

#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/syscalls.h>
#include <linux/ktime.h>
#include <linux/sched/clock.h>

#ifdef CONFIG_X86
#include <asm/e820/types.h>
#include <asm/e820/api.h>
#endif

#include "nvmev.h"
#include "conv_ftl.h"
#include "zns_ftl.h"
#include "simple_ftl.h"
#include "kv_ftl.h"
#include "dma.h"

/****************************************************************
 * Memory Layout
 ****************************************************************
 * virtDev
 *  - PCI header
 *    -> BAR at 1MiB area
 *  - PCI capability descriptors
 *
 * +--- memmap_start
 * |
 * v
 * +--------------+------------------------------------------+
 * | <---1MiB---> | <---------- Storage Area --------------> |
 * +--------------+------------------------------------------+
 *
 * 1MiB area for metadata
 *  - BAR : 1 page
 *	- DBS : 1 page
 *	- MSI-x table: 16 bytes/entry * 32
 *
 * Storage area
 *
 ****************************************************************/

/****************************************************************
 * Argument
 ****************************************************************
 * 1. Memmap start
 * 2. Memmap size
 ****************************************************************/

struct nvmev_dev *nvmev_vdev = NULL;

static unsigned long memmap_start = 0;
static unsigned long memmap_size = 0;

static unsigned int read_time = 1;
static unsigned int read_delay = 1;
static unsigned int read_trailing = 0;

static unsigned int write_time = 1;
static unsigned int write_delay = 1;
static unsigned int write_trailing = 0;

static unsigned int nr_io_units = 8;
static unsigned int io_unit_shift = 12;

static unsigned int FTL_split_units = 8192;
static unsigned int split_ratio = 4;

static unsigned int in_que_size = 4096;
static unsigned int out_que_size = 4096;

static unsigned int nr_io_workers = 1;
static unsigned int nr_dispatcher = 1;
static unsigned int nr_SGX_io_workers = 1;

static char *cpus;
static unsigned int debug = 0;

int io_using_dma = false;

#define REQ_QUEUE_ID_MASK  0x000f0000ULL
#define REQ_DISP_ID_MASK   0x00f00000ULL
#define REQ_QUEUE_MAP_MASK 0xff000000ULL

#define REQ_QUEUE_IN       0LL
#define REQ_QUEUE_OUT      0x08000000ULL

static int set_parse_mem_param(const char *val, const struct kernel_param *kp)
{
	unsigned long *arg = (unsigned long *)kp->arg;
	*arg = memparse(val, NULL);
	return 0;
}

static struct kernel_param_ops ops_parse_mem_param = {
	.set = set_parse_mem_param,
	.get = param_get_ulong,
};

module_param_cb(memmap_start, &ops_parse_mem_param, &memmap_start, 0444);
MODULE_PARM_DESC(memmap_start, "Reserved memory address");
module_param_cb(memmap_size, &ops_parse_mem_param, &memmap_size, 0444);
MODULE_PARM_DESC(memmap_size, "Reserved memory size");
module_param(read_time, uint, 0644);
MODULE_PARM_DESC(read_time, "Read time in nanoseconds");
module_param(read_delay, uint, 0644);
MODULE_PARM_DESC(read_delay, "Read delay in nanoseconds");
module_param(read_trailing, uint, 0644);
MODULE_PARM_DESC(read_trailing, "Read trailing in nanoseconds");
module_param(write_time, uint, 0644);
MODULE_PARM_DESC(write_time, "Write time in nanoseconds");
module_param(write_delay, uint, 0644);
MODULE_PARM_DESC(write_delay, "Write delay in nanoseconds");
module_param(write_trailing, uint, 0644);
MODULE_PARM_DESC(write_trailing, "Write trailing in nanoseconds");
module_param(nr_io_units, uint, 0444);
MODULE_PARM_DESC(nr_io_units, "Number of I/O units that operate in parallel");
module_param(io_unit_shift, uint, 0444);
MODULE_PARM_DESC(io_unit_shift, "Size of each I/O unit (2^)");
module_param(FTL_split_units, uint, 0444);
MODULE_PARM_DESC(FTL_split_units, "Number of I/O units that FTL thread process");
module_param(split_ratio, uint, 0444);
MODULE_PARM_DESC(split_ratio, "Ratio of split I/O units");
module_param(cpus, charp, 0444);
MODULE_PARM_DESC(cpus, "CPU list for process, completion(int.) threads, Seperated by Comma(,)");
module_param(in_que_size, uint, 0444);
MODULE_PARM_DESC(in_que_size, "Size of Input queue for FTL thread communication");
module_param(out_que_size, uint, 0444);
MODULE_PARM_DESC(out_que_size, "Size of Output queue for FTL thread communication");
module_param(debug, uint, 0644);

module_param(nr_io_workers, uint, 0644);
MODULE_PARM_DESC(nr_io_workers, "Number of I/O workers");


module_param(nr_dispatcher, uint, 0644);
MODULE_PARM_DESC(nr_dispatcher, "Number of dispatcher threads");
module_param(nr_SGX_io_workers, uint, 0644);
MODULE_PARM_DESC(nr_SGX_io_workers, "Number of I/O workers for SGX");

// Returns true if an event is processed
static bool nvmev_proc_dbs(int dispatcher_id)
{
	int qid;
	int dbs_idx;
	int new_db;
	int old_db;
	bool updated = false;

	if (dispatcher_id == 0) {
		// Admin queue
		new_db = nvmev_vdev->dbs[0];
		if (new_db != nvmev_vdev->old_dbs[0]) {
			nvmev_proc_admin_sq(new_db, nvmev_vdev->old_dbs[0]);
			nvmev_vdev->old_dbs[0] = new_db;
			updated = true;
		}
		new_db = nvmev_vdev->dbs[1];
		if (new_db != nvmev_vdev->old_dbs[1]) {
			nvmev_proc_admin_cq(new_db, nvmev_vdev->old_dbs[1]);
			nvmev_vdev->old_dbs[1] = new_db;
			updated = true;
		}
	}

	int nr_dispatcher = nvmev_vdev->config.nr_dispatcher;
	int start_qid = 1 + dispatcher_id;
	
	// Submission queues
	for (qid = start_qid; qid <= nvmev_vdev->nr_sq; qid += nr_dispatcher) {
		if (nvmev_vdev->sqes[qid] == NULL)
			continue;
		dbs_idx = qid * 2;
		new_db = READ_ONCE(nvmev_vdev->dbs[dbs_idx]);
		old_db = nvmev_vdev->old_dbs[dbs_idx];
		mb();
		if (new_db != old_db) {
			nvmev_vdev->old_dbs[dbs_idx] = nvmev_proc_io_sq(dispatcher_id, qid, new_db, old_db);
			updated = true;
		}
	}

	// Completion queues
	for (qid = start_qid; qid <= nvmev_vdev->nr_cq; qid += nr_dispatcher) {
		if (nvmev_vdev->cqes[qid] == NULL)
			continue;
		dbs_idx = qid * 2 + 1;
		new_db = nvmev_vdev->dbs[dbs_idx];
		old_db = nvmev_vdev->old_dbs[dbs_idx];
		if (new_db != old_db) {
			nvmev_proc_io_cq(qid, new_db, old_db);
			nvmev_vdev->old_dbs[dbs_idx] = new_db;
			updated = true;
		}
	}

	return updated;
}

#ifdef PERF_DEBUG
static unsigned long long intr_clock[NR_MAX_IO_QUEUE + 1];
static unsigned long long intr_counter[NR_MAX_IO_QUEUE + 1];
static unsigned long long intr_diff[NR_MAX_IO_QUEUE + 1];

unsigned long long prev_clock;

static unsigned long long intr_time[11][512], intr_interval[11][512], cnt[11];

static void add_reclaim_cq_stats(int qidx, struct nvmev_completion_queue *cq, unsigned long long prev_clock) {

	intr_clock[qidx] += (local_clock() - prev_clock);
	intr_counter[qidx]++;
	if (intr_counter[qidx] > 70) {
		intr_diff[qidx] += ktime_get_ns() - cq->last_exp_intr_time;

		intr_time[qidx][cnt[qidx]] = ktime_get_ns();
		intr_interval[qidx][cnt[qidx]] =
			intr_time[qidx][cnt[qidx]] - cq->last_exp_intr_time;
		cnt[qidx]++;
	}

	//intr_diff[qidx] += ktime_get_ns() - cq->last_exp_intr_time;

	if (intr_counter[qidx] == 270) {
		// NVMEV_DISPATCHER_LOG("Intr %d: %llu HZ: %d\n", qidx, intr_clock[qidx] / intr_counter[qidx], HZ);
		NVMEV_INTR_LOG("Intr diff: %llu (ns), intr_counter %llu\n",
			       intr_diff[qidx] / (intr_counter[qidx] - 70),
			       intr_counter[qidx] - 70);
		intr_clock[qidx] = 0;
		intr_counter[qidx] = 0;
		intr_diff[qidx] = 0;
	}

	if (cnt[qidx] == 300) {
		int i;
		NVMEV_INTR_LOG("qidx %d\n", qidx);
		for (i = 0; i < 512; i++) {
			NVMEV_INTR_LOG("Intr interval %d: intr_time %llu, interval_time %llu\n", i,
				       intr_time[qidx][i], intr_interval[qidx][i]);
		}
		cnt[qidx] = 0;
	}
}
#endif

static bool nvmev_reclaim_completed_requests(int dispatcher_id)
{
	bool updated = false;
	int qidx;
	// get completed requests from all FTL thread
	dequeue_io_req_from_ftl(dispatcher_id);

	// TODO: the current implementation does not support multiple dispatcher

	int nr_dispatcher = nvmev_vdev->config.nr_dispatcher;
	int start_qid = 1 + dispatcher_id;

	for (qidx = start_qid; qidx <= nvmev_vdev->nr_cq; qidx += nr_dispatcher) {
		struct nvmev_completion_queue *cq = nvmev_vdev->cqes[qidx];

		if (cq == NULL || !cq->irq_enabled)
			continue;

		// if (mutex_trylock(&cq->irq_lock)) {
		if (cq->interrupt_ready == true) {
#ifdef PERF_DEBUG
			prev_clock = local_clock();
#endif
			cq->interrupt_ready = false;
			nvmev_signal_irq(cq->irq_vector);
			updated = true;
#ifdef PERF_DEBUG
			add_reclaim_cq_stats(qidx, cq, prev_clock);
#endif
		}
		//	mutex_unlock(&cq->irq_lock);
		// }
	}

	return updated;
}

#ifdef PERF_DEBUG
static unsigned long long counter = 0;
static unsigned long long resched_time = 0, last_resched_time = 0;

static void add_resched_stats(void) {
	counter++;
	resched_time += ktime_get_ns() - last_resched_time;
	last_resched_time = ktime_get_ns();
}
#endif

static int nvmev_dispatcher(void *data)
{
	static unsigned long last_dispatched_time = 0;
	struct nvmev_dispatcher_ctx *ctx = (struct nvmev_dispatcher_ctx *)data;
	int dispatcher_id = ctx->id;
	int output_flag = 0;

	NVMEV_INFO("nvmev_dispatcher %d started on cpu %d (node %d)\n",
			 dispatcher_id,
		   nvmev_vdev->config.cpu_nr_dispatcher[dispatcher_id],
		   cpu_to_node(nvmev_vdev->config.cpu_nr_dispatcher[dispatcher_id]));

	static unsigned long long intr_interval = 0;
	static unsigned long long last_intr_time = 0;
	static int intr_cnt = 0;

	while (!kthread_should_stop()) {
		if (dispatcher_id == 0) {
			if (nvmev_proc_bars())
			last_dispatched_time = jiffies;
		}

		if (output_flag <= 0) {
			NVMEV_INFO("nvmev_proc_bars successfully\n");
			output_flag++;
		}

		if (nvmev_proc_dbs(dispatcher_id))
			last_dispatched_time = jiffies;

		if (output_flag <= 1) {
			NVMEV_INFO("nvmev_proc_dbs successfully\n");
			output_flag++;
		}

		if (nvmev_reclaim_completed_requests(dispatcher_id)) {
			last_dispatched_time = jiffies;
#ifdef PERF_DEBUG
			if (intr_cnt != 0)
				intr_interval = ktime_get_ns() - last_intr_time;
			intr_cnt++;
			last_intr_time = ktime_get_ns();
#endif
		}

#ifdef PERF_DEBUG
		if (intr_cnt > 0 && intr_cnt % LOG_PRINT_INTERVAL == 0) {
			NVMEV_DISPATCHER_LOG("intr_cnt: %d intr_interval [ns]: %llu\n", intr_cnt, intr_interval / intr_cnt);
			intr_cnt = 0;
		}
#endif

		if (output_flag <= 2) {
			NVMEV_INFO("nvmev_reclaim_completed_requests successfully\n");
			output_flag++;
		}

		/*if (CONFIG_NVMEVIRT_IDLE_TIMEOUT != 0 &&
		  time_after(jiffies, last_dispatched_time + (CONFIG_NVMEVIRT_IDLE_TIMEOUT * HZ)))
			schedule_timeout_interruptible(1);
		else {
			cond_resched();
		} */
		cond_resched();
	}

	return 0;
}

static void NVMEV_DISPATCHER_INIT(struct nvmev_dev *nvmev_vdev)
{
	int i = 0;
	nvmev_vdev->dispatcher_ctxes = kzalloc(
		sizeof(struct nvmev_dispatcher_ctx) * nvmev_vdev->config.nr_dispatcher, GFP_KERNEL);
	nvmev_vdev->nvmev_dispatcher = kzalloc(
		sizeof(struct task_struct *) * nvmev_vdev->config.nr_dispatcher, GFP_KERNEL);
	if (!nvmev_vdev->dispatcher_ctxes || !nvmev_vdev->nvmev_dispatcher) {
		NVMEV_ERROR("Failed to allocate dispatcher context\n");
		return;
	}

	for (i = 0; i < nvmev_vdev->config.nr_dispatcher; i++) {
		nvmev_vdev->dispatcher_ctxes[i].id = i;
		nvmev_vdev->nvmev_dispatcher[i] = kthread_create(
			nvmev_dispatcher, &nvmev_vdev->dispatcher_ctxes[i], "nvmev_dispatcher");

		if (IS_ERR(nvmev_vdev->nvmev_dispatcher[i])) {
			NVMEV_ERROR("Failed to create dispatcher thread\n");
			continue;
		}

		kthread_bind(nvmev_vdev->nvmev_dispatcher[i],
			     nvmev_vdev->config.cpu_nr_dispatcher[i]);
		wake_up_process(nvmev_vdev->nvmev_dispatcher[i]);
	}
}

static void NVMEV_DISPATCHER_FINAL(struct nvmev_dev *nvmev_vdev)
{
	int i;
	for (i = 0; i < nvmev_vdev->config.nr_dispatcher; i++) {
		if (!IS_ERR_OR_NULL(nvmev_vdev->nvmev_dispatcher[i])) {
			kthread_stop(nvmev_vdev->nvmev_dispatcher[i]);
			NVMEV_EXIT_LOG("nvmev_dispatcher %d stopped\n", i);
		}
	}
	kfree(nvmev_vdev->nvmev_dispatcher);
	nvmev_vdev->nvmev_dispatcher = NULL;

	if (nvmev_vdev->dispatcher_ctxes) {
		kfree(nvmev_vdev->dispatcher_ctxes);
		nvmev_vdev->dispatcher_ctxes = NULL;
	}
}

#ifdef CONFIG_X86
static int __validate_configs_arch(void)
{
	unsigned long resv_start_bytes;
	unsigned long resv_end_bytes;

	resv_start_bytes = memmap_start;
	resv_end_bytes = resv_start_bytes + memmap_size - 1;

	if (e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RAM) ||
	    e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RESERVED_KERN)) {
		NVMEV_ERROR("[mem %#010lx-%#010lx] is usable, not reseved region\n",
			    (unsigned long)resv_start_bytes, (unsigned long)resv_end_bytes);
		return -EPERM;
	}

	if (!e820__mapped_any(resv_start_bytes, resv_end_bytes, E820_TYPE_RESERVED)) {
		NVMEV_ERROR("[mem %#010lx-%#010lx] is not reseved region\n",
			    (unsigned long)resv_start_bytes, (unsigned long)resv_end_bytes);
		return -EPERM;
	}
	return 0;
}
#else
static int __validate_configs_arch(void)
{
	/* TODO: Validate architecture-specific configurations */
	return 0;
}
#endif

static int __validate_configs(void)
{
	if (!memmap_start) {
		NVMEV_ERROR("[memmap_start] should be specified\n");
		return -EINVAL;
	}

	if (!memmap_size) {
		NVMEV_ERROR("[memmap_size] should be specified\n");
		return -EINVAL;
	} else if (memmap_size <= MB(1)) {
		NVMEV_ERROR("[memmap_size] should be bigger than 1 MiB\n");
		return -EINVAL;
	}

	NVMEV_ERROR("memmap_start: 0x%lx, memmap_size: 0x%lx\n", memmap_start, memmap_size);

	// if (__validate_configs_arch()) {
	//	return -EPERM;
	// }

	if (nr_io_units == 0 || io_unit_shift == 0) {
		NVMEV_ERROR("Need non-zero IO unit size and at least one IO unit\n");
		return -EINVAL;
	}
	if (read_time == 0) {
		NVMEV_ERROR("Need non-zero read time\n");
		return -EINVAL;
	}
	if (write_time == 0) {
		NVMEV_ERROR("Need non-zero write time\n");
		return -EINVAL;
	}

	return 0;
}

static void __print_perf_configs(void)
{
#ifdef CONFIG_NVMEV_VERBOSE
	unsigned long unit_perf_kb = nvmev_vdev->config.nr_io_units
				     << (nvmev_vdev->config.io_unit_shift - 10);
	struct nvmev_config *cfg = &nvmev_vdev->config;

	NVMEV_INFO("=============== Configurations ===============\n");
	NVMEV_INFO("* IO units : %d x %d\n", cfg->nr_io_units, 1 << cfg->io_unit_shift);
	NVMEV_INFO("* I/O times\n");
	NVMEV_INFO("  Read     : %u + %u x + %u ns\n", cfg->read_delay, cfg->read_time,
		   cfg->read_trailing);
	NVMEV_INFO("  Write    : %u + %u x + %u ns\n", cfg->write_delay, cfg->write_time,
		   cfg->write_trailing);
	NVMEV_INFO("* Bandwidth\n");
	NVMEV_INFO("  Read     : %lu MiB/s\n",
		   (1000000000UL / (cfg->read_time + cfg->read_delay + cfg->read_trailing)) *
				   unit_perf_kb >>
			   10);
	NVMEV_INFO("  Write    : %lu MiB/s\n",
		   (1000000000UL / (cfg->write_time + cfg->write_delay + cfg->write_trailing)) *
				   unit_perf_kb >>
			   10);
#endif
}

static int __get_nr_entries(int dbs_idx, int queue_size)
{
	int diff = nvmev_vdev->dbs[dbs_idx] - nvmev_vdev->old_dbs[dbs_idx];
	if (diff < 0) {
		diff += queue_size;
	}
	return diff;
}

static int __proc_file_read(struct seq_file *m, void *data)
{
	const char *filename = m->private;
	struct nvmev_config *cfg = &nvmev_vdev->config;

	if (strcmp(filename, "read_times") == 0) {
		seq_printf(m, "%u + %u x + %u", cfg->read_delay, cfg->read_time,
			   cfg->read_trailing);
	} else if (strcmp(filename, "write_times") == 0) {
		seq_printf(m, "%u + %u x + %u", cfg->write_delay, cfg->write_time,
			   cfg->write_trailing);
	} else if (strcmp(filename, "io_units") == 0) {
		seq_printf(m, "%u x %u", cfg->nr_io_units, cfg->io_unit_shift);
	} else if (strcmp(filename, "stat") == 0) {
		int i;
		unsigned int nr_in_flight = 0;
		unsigned int nr_dispatch = 0;
		unsigned int nr_dispatched = 0;
		unsigned long long total_io = 0;
		for (i = 1; i <= nvmev_vdev->nr_sq; i++) {
			struct nvmev_submission_queue *sq = nvmev_vdev->sqes[i];
			if (!sq)
				continue;

			seq_printf(m, "%2d: %2u %4u %4u %4u %4u %llu\n", i,
				   __get_nr_entries(i * 2, sq->queue_size), sq->stat.nr_in_flight,
				   sq->stat.max_nr_in_flight, sq->stat.nr_dispatch,
				   sq->stat.nr_dispatched, sq->stat.total_io);

			nr_in_flight += sq->stat.nr_in_flight;
			nr_dispatch += sq->stat.nr_dispatch;
			nr_dispatched += sq->stat.nr_dispatched;
			total_io += sq->stat.total_io;

			barrier();
			sq->stat.max_nr_in_flight = 0;
		}
		seq_printf(m, "total: %u %u %u %llu\n", nr_in_flight, nr_dispatch, nr_dispatched,
			   total_io);
	} else if (strcmp(filename, "debug") == 0) {
		/* Left for later use */
	} else if (strcmp(filename, "thread_config") == 0) {
		seq_printf(m, "FTL_split_units:%u\n", nvmev_vdev->config.FTL_split_units);
		seq_printf(m, "split_ratio:%u\n", nvmev_vdev->config.split_ratio);
		seq_printf(m, "dispatcher_num:%u\n", nvmev_vdev->config.nr_dispatcher);
		seq_printf(m, "ftl_thread_num:%u\n", nvmev_vdev->config.nr_io_workers);
		seq_printf(m, "SGX_ftl_thread_num:%u\n", nvmev_vdev->config.nr_SGX_io_workers);
		seq_printf(m, "io_workers_cpu:");
		int i;
		seq_printf(m, "%u", nvmev_vdev->config.cpu_nr_io_workers[0]);
		for (i = 1; i < nvmev_vdev->config.nr_io_workers; i++) {
			seq_printf(m, ",%u", nvmev_vdev->config.cpu_nr_io_workers[i]);
		}
		seq_printf(m, "\n");
	} else if (strcmp(filename, "ssd_size") == 0) {
		seq_printf(m, "size:%lu\n", nvmev_vdev->config.storage_size);
	} else if (strcmp(filename, "worker_queue") == 0) {
		seq_printf(m, "in_que_size:%u\n", nvmev_vdev->config.in_que_size);
		seq_printf(m, "out_que_size:%u\n", nvmev_vdev->config.out_que_size);
		int d_id, ftl_id;
		for (d_id = 0; d_id < nvmev_vdev->config.nr_dispatcher; d_id++) {
			for (ftl_id = 0; ftl_id < nvmev_vdev->config.nr_io_workers; ftl_id++) {
				seq_printf(m, "in_que_start:%llu\n",
					   (uint64_t)(nvmev_vdev->in_que[d_id][ftl_id]));
				seq_printf(m, "out_que_start:%llu\n",
					   (uint64_t)(nvmev_vdev->out_que[d_id][ftl_id]));
			}
		}
	}

	return 0;
}

static ssize_t __proc_file_write(struct file *file, const char __user *buf, size_t len,
				 loff_t *offp)
{
	ssize_t count = len;
	const char *filename = file->f_path.dentry->d_name.name;
	char input[128];
	unsigned int ret;
	unsigned long long *old_stat;
	struct nvmev_config *cfg = &nvmev_vdev->config;
	size_t nr_copied;

	nr_copied = copy_from_user(input, buf, min(len, sizeof(input)));

	if (!strcmp(filename, "read_times")) {
		ret = sscanf(input, "%u %u %u", &cfg->read_delay, &cfg->read_time,
			     &cfg->read_trailing);
		//adjust_ftl_latency(0, cfg->read_time);
	} else if (!strcmp(filename, "write_times")) {
		ret = sscanf(input, "%u %u %u", &cfg->write_delay, &cfg->write_time,
			     &cfg->write_trailing);
		//adjust_ftl_latency(1, cfg->write_time);
	} else if (!strcmp(filename, "io_units")) {
		ret = sscanf(input, "%d %d", &cfg->nr_io_units, &cfg->io_unit_shift);
		if (ret < 1)
			goto out;

		old_stat = nvmev_vdev->io_unit_stat;
		nvmev_vdev->io_unit_stat =
			kzalloc(sizeof(*nvmev_vdev->io_unit_stat) * cfg->nr_io_units, GFP_KERNEL);

		mdelay(100); /* XXX: Delay the free of old stat so that outstanding
						 * requests accessing the unit_stat are all returned
						 */
		kfree(old_stat);
	} else if (!strcmp(filename, "stat")) {
		int i;
		for (i = 1; i <= nvmev_vdev->nr_sq; i++) {
			struct nvmev_submission_queue *sq = nvmev_vdev->sqes[i];
			if (!sq)
				continue;

			memset(&sq->stat, 0x00, sizeof(sq->stat));
		}
	} else if (!strcmp(filename, "debug")) {
		/* Left for later use */
	}

out:
	__print_perf_configs();

	return count;
}

static int __proc_file_open(struct inode *inode, struct file *file)
{
	return single_open(file, __proc_file_read, (char *)file->f_path.dentry->d_name.name);
}

#if LINUX_VERSION_CODE > KERNEL_VERSION(5, 0, 0)
static const struct proc_ops proc_file_fops = {
	.proc_open = __proc_file_open,
	.proc_write = __proc_file_write,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#else
static const struct file_operations proc_file_fops = {
	.open = __proc_file_open,
	.write = __proc_file_write,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#endif

static int nvmev_com_open(struct inode *inode, struct file *file)
{
	/*
	 * The fuse device's file's private_data is used to hold
	 * the fuse_conn(ection) when it is mounted, and is used to
	 * keep track of whether the file has been mounted already.
	 */
	file->private_data = NULL;
	return 0;
}

/* do this across all riqs */
static void *rfuse_validate_mmap_request(loff_t queoff, size_t size, size_t offset)
{
	struct page *page;
	void *ptr;

	/* riq_id should be passed by the mmap offset argument */
	unsigned long long map_args = queoff << PAGE_SHIFT;
	loff_t map_queue = map_args & REQ_QUEUE_MAP_MASK;
	int disp_id = (int)((map_args & REQ_DISP_ID_MASK) >> 20);
	int riq_id = (int)((map_args & REQ_QUEUE_ID_MASK) >> 16);

	// TODO: hack to support only one dispatcher

	switch (map_queue) {
	case REQ_QUEUE_IN:
		NVMEV_INFO("map in queue, disp_id: %d, riq_id: %d\n", disp_id, riq_id);
		ptr = nvmev_vdev->in_que[disp_id][riq_id];
		break;
	case REQ_QUEUE_OUT:
		NVMEV_INFO("map out queue, disp_id: %d, riq_id: %d\n", disp_id, riq_id);
		ptr = nvmev_vdev->out_que[disp_id][riq_id];
		break;
	default:
		NVMEV_ERROR("Invalid map_queue argument\n");
		NVMEV_ASSERT(0);
		return ERR_PTR(-EINVAL);
	}

	ptr = (void*)((char*)ptr + offset);

	// Check if the virtual memory space is big enough
	page = virt_to_head_page(ptr);
	if (size > page_size(page)) {
		NVMEV_ERROR("rfuse mmap: size is too big\n");
		BUG_ON(1);
		return ERR_PTR(-EINVAL);
	}

	return ptr;
}

/* 
rfuse: Initialize a mmap area of queues
 */
static int nvmev_com_mmap(struct file *file, struct vm_area_struct *vma)
{
	unsigned long size = (unsigned long)(vma->vm_end - vma->vm_start);
	unsigned long pfn;
	void *ptr;
	size_t offset = 0, page_size = 0;
	unsigned long user_addr = vma->vm_start;

	NVMEV_PARAM_LOG("mmap: start: %lx, end: %lx, size: %lu\n", vma->vm_start, vma->vm_end,
			size);

	if (size != sizeof(struct nvmev_io_cmd) * nvmev_vdev->config.in_que_size || size % 4096 != 0) {
		printk(KERN_ERR "rfuse mmap: size is not correct, current size %lu\n", size);
		return -EINVAL;
	}

	while (size > 0) {
		page_size = min_t(size_t, size, PAGE_SIZE);
		NVMEV_ASSERT(page_size == PAGE_SIZE);

		ptr = rfuse_validate_mmap_request(vma->vm_pgoff, page_size, offset);

		if (IS_ERR(ptr)) {
			NVMEV_ASSERT(0);
			return PTR_ERR(ptr);
		}

		pfn = virt_to_phys(ptr) >> PAGE_SHIFT;

		if (remap_pfn_range(vma, user_addr, pfn, page_size, vma->vm_page_prot)) {
			printk(KERN_ERR "rfuse mmap: mmap failed\n");
			NVMEV_ASSERT(0);
			return -EAGAIN;
		}

		offset += page_size;
		user_addr += page_size;
		size -= page_size;
	}

	return 0;
}

const struct file_operations nvmev_com_operations = {
	.owner = THIS_MODULE,
	.open = nvmev_com_open,
	.mmap = nvmev_com_mmap,
};
EXPORT_SYMBOL_GPL(nvmev_com_operations);

#define NVMEV_MINOR 254

static struct miscdevice nvmev_miscdevice = {
	.minor = NVMEV_MINOR,
	.name = "nvmev_com",
	.fops = &nvmev_com_operations,
};

static void NVMEV_STORAGE_INIT(struct nvmev_dev *nvmev_vdev)
{
	NVMEV_INFO("Storage: %#010lx-%#010lx (%lu MiB)\n", nvmev_vdev->config.storage_start,
		   nvmev_vdev->config.storage_start + nvmev_vdev->config.storage_size,
		   BYTE_TO_MB(nvmev_vdev->config.storage_size));

	nvmev_vdev->io_unit_stat = kzalloc(
		sizeof(*nvmev_vdev->io_unit_stat) * nvmev_vdev->config.nr_io_units, GFP_KERNEL);

	nvmev_vdev->storage_mapped = memremap(nvmev_vdev->config.storage_start,
	 				      nvmev_vdev->config.storage_size, MEMREMAP_WB);

	if (!nvmev_vdev->storage_mapped) {
		ASSERT(0 && "Failed to map storage space");
		return;
	}

	nvmev_vdev->proc_root = proc_mkdir("nvmev", NULL);
	nvmev_vdev->proc_read_times =
		proc_create("read_times", 0664, nvmev_vdev->proc_root, &proc_file_fops);
	nvmev_vdev->proc_write_times =
		proc_create("write_times", 0664, nvmev_vdev->proc_root, &proc_file_fops);
	nvmev_vdev->proc_io_units =
		proc_create("io_units", 0664, nvmev_vdev->proc_root, &proc_file_fops);
	nvmev_vdev->proc_stat = proc_create("stat", 0444, nvmev_vdev->proc_root, &proc_file_fops);
	nvmev_vdev->proc_debug = proc_create("debug", 0444, nvmev_vdev->proc_root, &proc_file_fops);
	// sharing worker queue with FTL threads
	nvmev_vdev->proc_worker_queue =
		proc_create("worker_queue", 0664, nvmev_vdev->proc_root, &proc_file_fops);
	nvmev_vdev->proc_ssd_size =
		proc_create("ssd_size", 0664, nvmev_vdev->proc_root, &proc_file_fops);
	nvmev_vdev->proc_thread_config =
		proc_create("thread_config", 0664, nvmev_vdev->proc_root, &proc_file_fops);
}

static void NVMEV_STORAGE_FINAL(struct nvmev_dev *nvmev_vdev)
{
	remove_proc_entry("read_times", nvmev_vdev->proc_root);
	remove_proc_entry("write_times", nvmev_vdev->proc_root);
	remove_proc_entry("io_units", nvmev_vdev->proc_root);
	remove_proc_entry("stat", nvmev_vdev->proc_root);
	remove_proc_entry("debug", nvmev_vdev->proc_root);
	remove_proc_entry("thread_config", nvmev_vdev->proc_root);
	remove_proc_entry("ssd_size", nvmev_vdev->proc_root);
	remove_proc_entry("worker_queue", nvmev_vdev->proc_root);

	remove_proc_entry("nvmev", NULL);

	if (nvmev_vdev->storage_mapped)
		memunmap(nvmev_vdev->storage_mapped);

	if (nvmev_vdev->io_unit_stat)
		kfree(nvmev_vdev->io_unit_stat);
	
	NVMEV_EXIT_LOG("Storage final\n");
}

static bool __load_configs(struct nvmev_config *config)
{
	unsigned int cpu_nr;
	char *cpu;
	int io_worker_count = 0, dispatcher_count = 0;

	if (__validate_configs() < 0) {
		return false;
	}

#if (BASE_SSD == KV_PROTOTYPE)
	memmap_size -= KV_MAPPING_TABLE_SIZE; // Reserve space for KV mapping table
#endif

	config->memmap_start = memmap_start;
	config->memmap_size = memmap_size;
	// storage space starts from 1M offset
	config->storage_start = memmap_start + MB(1);
	config->storage_size = memmap_size - MB(1);

	config->read_time = read_time;
	config->read_delay = read_delay;
	config->read_trailing = read_trailing;
	config->write_time = write_time;
	config->write_delay = write_delay;
	config->write_trailing = write_trailing;
	config->nr_io_units = nr_io_units;
	config->io_unit_shift = io_unit_shift;

	config->FTL_split_units = FTL_split_units;
	config->split_ratio = split_ratio;

	if (nr_io_workers == 2) {
		config->seg_units[0] = 1 * config->FTL_split_units;
		config->seg_units[1] = split_ratio * config->FTL_split_units;
	} else if (nr_io_workers == 1) {
		config->seg_units[0] = config->FTL_split_units;
	} else {
		ASSERT(0 && "Invalid number of nr_io_workers");
	}
	

	NVMEV_PARAM_LOG("seg_uint[0]: %u, seg_uint[1]: %u\n", config->seg_units[0], config->seg_units[1]);

	config->nr_dispatcher = nr_dispatcher;
	config->nr_io_workers = nr_io_workers;
	config->nr_SGX_io_workers = nr_SGX_io_workers;

	config->in_que_size = in_que_size;
	config->out_que_size = out_que_size;

	NVMEV_PARAM_LOG("FTL_split_units: %u, split_ratio: %u, dispatcher: %u, FTL threads: %u, SGX FTL threads: %u\n",
		    config->FTL_split_units, config->split_ratio, config->nr_dispatcher, config->nr_io_workers, config->nr_SGX_io_workers);

	while ((cpu = strsep(&cpus, ",")) != NULL) {
		cpu_nr = (unsigned int)simple_strtol(cpu, NULL, 10);
		if (dispatcher_count < config->nr_dispatcher) {
			config->cpu_nr_dispatcher[dispatcher_count] = cpu_nr;
			dispatcher_count++;
		} else {
			config->cpu_nr_io_workers[io_worker_count] = cpu_nr;
			io_worker_count++;
		}
	}

	NVMEV_INFO("Dispatcher %d, I/O worker %d\n", dispatcher_count, io_worker_count);

	if (io_worker_count != config->nr_io_workers) {
		NVMEV_ERROR(
			"I/O worker core setting mismatch, set %u core, but currently allocate %u\n",
			config->nr_io_workers, io_worker_count);
		return false;
	}

	if (dispatcher_count != config->nr_dispatcher) {
		NVMEV_ERROR(
			"Dispatcher core setting mismatch, set %u core, but currently allocate %u\n",
			config->nr_dispatcher, dispatcher_count);
		return false;
	}

	return true;
}

static void NVMEV_NAMESPACE_INIT(struct nvmev_dev *nvmev_vdev)
{
	unsigned long long remaining_capacity = nvmev_vdev->config.storage_size;
	void *ns_addr = nvmev_vdev->storage_mapped;
	const int nr_ns = NR_NAMESPACES; // XXX: allow for dynamic nr_ns
	//TODO: hack to set cpu_nr_dispatcher[0]
	const unsigned int disp_no = nvmev_vdev->config.cpu_nr_dispatcher[0];
	int i;
	unsigned long long size;

	struct nvmev_ns *ns = kmalloc(sizeof(struct nvmev_ns) * nr_ns, GFP_KERNEL);

	for (i = 0; i < nr_ns; i++) {
		if (NS_CAPACITY(i) == 0)
			size = remaining_capacity;
		else
			size = min(NS_CAPACITY(i), remaining_capacity);

		if (NS_SSD_TYPE(i) == SSD_TYPE_NVM)
			simple_init_namespace(&ns[i], i, size, ns_addr, disp_no);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_CONV)
			conv_init_namespace(&ns[i], i, size, ns_addr, disp_no);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_ZNS)
			zns_init_namespace(&ns[i], i, size, ns_addr, disp_no);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_KV)
			kv_init_namespace(&ns[i], i, size, ns_addr, disp_no);
		else
			BUG_ON(1);

		remaining_capacity -= size;
		ns_addr += size;
		NVMEV_INFO("ns %d/%d: size %lld MiB\n", i, nr_ns, BYTE_TO_MB(ns[i].size));
	}

	nvmev_vdev->ns = ns;
	nvmev_vdev->nr_ns = nr_ns;
	nvmev_vdev->mdts = MDTS;
}

static void NVMEV_NAMESPACE_FINAL(struct nvmev_dev *nvmev_vdev)
{
	struct nvmev_ns *ns = nvmev_vdev->ns;
	const int nr_ns = NR_NAMESPACES; // XXX: allow for dynamic nvmev_vdev->nr_ns
	int i;

	for (i = 0; i < nr_ns; i++) {
		if (NS_SSD_TYPE(i) == SSD_TYPE_NVM)
			simple_remove_namespace(&ns[i]);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_CONV)
			conv_remove_namespace(&ns[i]);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_ZNS)
			zns_remove_namespace(&ns[i]);
		else if (NS_SSD_TYPE(i) == SSD_TYPE_KV)
			kv_remove_namespace(&ns[i]);
		else
			BUG_ON(1);
	}

	kfree(ns);
	nvmev_vdev->ns = NULL;

	NVMEV_EXIT_LOG("Namespace removed\n");
}

static void __print_base_config(void)
{
	const char *type = "unknown";
	switch (BASE_SSD) {
	case INTEL_OPTANE:
		type = "NVM SSD";
		break;
	case SAMSUNG_970PRO:
		type = "Samsung 970 Pro SSD";
		break;
	case ZNS_PROTOTYPE:
		type = "ZNS SSD Prototype";
		break;
	case KV_PROTOTYPE:
		type = "KVSSD Prototype";
		break;
	case WD_ZN540:
		type = "WD ZN540 ZNS SSD";
		break;
	}

	NVMEV_INFO("Version %x.%x for >> %s <<\n", (NVMEV_VERSION & 0xff00) >> 8,
		   (NVMEV_VERSION & 0x00ff), type);
}

static int NVMeV_init(void)
{
	int ret = 0;
	int i, j;
	int dispatcher_num, io_worker_num;

	__print_base_config();

	nvmev_vdev = VDEV_INIT();
	if (!nvmev_vdev)
		return -EINVAL;

	if (!__load_configs(&nvmev_vdev->config)) {
		goto ret_err;
	}

	NVMEV_ERROR("Load configs successfully\n");

	NVMEV_STORAGE_INIT(nvmev_vdev);

	NVMEV_ERROR("Storage init successfully\n");

	NVMEV_NAMESPACE_INIT(nvmev_vdev);

	NVMEV_ERROR("Namespace init successfully\n");

	if (io_using_dma) {
		if (ioat_dma_chan_set("dma7chan0") != 0) {
			io_using_dma = false;
			NVMEV_ERROR("Cannot use DMA engine, Fall back to memcpy\n");
		}
	}

	if (!NVMEV_PCI_INIT(nvmev_vdev)) {
		NVMEV_ERROR("PCI INIT failed\n");
		goto ret_err;
	}

	NVMEV_INFO("PCI INIT successfully\n");

	__print_perf_configs();

	dispatcher_num = nvmev_vdev->config.nr_dispatcher, io_worker_num = nvmev_vdev->config.nr_io_workers;
	NVMEV_PARAM_LOG("dispatcher_num %d, io_worker_num %d\n", dispatcher_num, io_worker_num);

	nvmev_vdev->completed_list = kmalloc(sizeof(struct list_head) * nvmev_vdev->config.nr_dispatcher, GFP_KERNEL);
	for (i = 0; i < nvmev_vdev->config.nr_dispatcher; i++) {
		INIT_LIST_HEAD(&nvmev_vdev->completed_list[i]);
		ASSERT(list_empty(&nvmev_vdev->completed_list[i]));
	}

	// TODO: fix the fixed size of in/out queue
	// nvmev_vdev->config.in_que_size = PAGE_SIZE / sizeof(struct nvmev_io_cmd);
	// nvmev_vdev->config.out_que_size = PAGE_SIZE / sizeof(struct nvmev_io_cmd);

	NVMEV_PARAM_LOG("in_que_size %u, out_que_size %u\n", nvmev_vdev->config.in_que_size, nvmev_vdev->config.out_que_size);

	nvmev_vdev->in_que = kzalloc(sizeof(struct nvmev_io_cmd **) * dispatcher_num, GFP_KERNEL);
	nvmev_vdev->out_que = kzalloc(sizeof(struct nvmev_io_cmd **) * dispatcher_num, GFP_KERNEL);
	nvmev_vdev->in_que_tail = kzalloc(sizeof(int *) * dispatcher_num, GFP_KERNEL);
	nvmev_vdev->out_que_head = kzalloc(sizeof(int *) * dispatcher_num, GFP_KERNEL);
	
	NVMEV_ASSERT(nvmev_vdev->in_que && nvmev_vdev->out_que && nvmev_vdev->in_que_tail && nvmev_vdev->out_que_head);

	for (i = 0; i < dispatcher_num; i++) {
		nvmev_vdev->in_que[i] =
			kzalloc(sizeof(struct nvmev_io_cmd *) * io_worker_num, GFP_KERNEL);
		nvmev_vdev->out_que[i] =
			kzalloc(sizeof(struct nvmev_io_cmd *) * io_worker_num, GFP_KERNEL);
		nvmev_vdev->in_que_tail[i] = kzalloc(sizeof(int) * io_worker_num, GFP_KERNEL);
		nvmev_vdev->out_que_head[i] = kzalloc(sizeof(int) * io_worker_num, GFP_KERNEL);
		
		NVMEV_ASSERT(nvmev_vdev->in_que[i] && nvmev_vdev->out_que[i] && nvmev_vdev->in_que_tail[i] && nvmev_vdev->out_que_head[i]);

		for (j = 0; j < io_worker_num; j++) {
			nvmev_vdev->in_que[i][j] = kzalloc(sizeof(struct nvmev_io_cmd) *
								   nvmev_vdev->config.in_que_size,
							   GFP_KERNEL);
			nvmev_vdev->out_que[i][j] = kzalloc(sizeof(struct nvmev_io_cmd) *
								    nvmev_vdev->config.out_que_size,
							    GFP_KERNEL);
			NVMEV_ASSERT(nvmev_vdev->in_que[i][j] != NULL);
			NVMEV_ASSERT(nvmev_vdev->out_que[i][j] != NULL);
		}
	}

	NVMEV_INFO("Initiate in_que out_que successfully\n");

	// NVMEV_IO_WORKER_INIT(nvmev_vdev);
	NVMEV_DISPATCHER_INIT(nvmev_vdev);

	NVMEV_INFO("Initiate dispatcher successfully\n");

	pci_bus_add_devices(nvmev_vdev->virt_bus);

	NVMEV_INFO("Add devices to PCI bus successfully\n");

	ret = misc_register(&nvmev_miscdevice);
	if (ret) {
		NVMEV_ERROR("Failed to register misc device\n");
		goto ret_err;
	}

	NVMEV_INFO("Virtual NVMe device created\n");

	return 0;

ret_err:
	VDEV_FINALIZE(nvmev_vdev);
	return -EIO;
}

static void NVMEV_MSG_QUEUE_FINAL(struct nvmev_dev *nvmev_vdev)
{
	int i, j;
	int dispatcher_num = 1, io_worker_num = nvmev_vdev->config.nr_io_workers;
	for (i = 0; i < dispatcher_num; i++) {
		for (j = 0; j < io_worker_num; j++) {
			kfree(nvmev_vdev->in_que[i][j]);
			kfree(nvmev_vdev->out_que[i][j]);
		}
		kfree(nvmev_vdev->in_que[i]);
		kfree(nvmev_vdev->out_que[i]);
		kfree(nvmev_vdev->in_que_tail[i]);
		kfree(nvmev_vdev->out_que_head[i]);
	}
	kfree(nvmev_vdev->in_que);
	kfree(nvmev_vdev->out_que);
	kfree(nvmev_vdev->in_que_tail);
	kfree(nvmev_vdev->out_que_head);
	NVMEV_EXIT_LOG("in_que out_que freed\n");
}

static void NVMeV_exit(void)
{
	int i;

	if (nvmev_vdev->virt_bus != NULL) {
		pci_stop_root_bus(nvmev_vdev->virt_bus);
		pci_remove_root_bus(nvmev_vdev->virt_bus);
	}

	NVMEV_DISPATCHER_FINAL(nvmev_vdev);
	// NVMEV_IO_WORKER_FINAL(nvmev_vdev);

	NVMEV_MSG_QUEUE_FINAL(nvmev_vdev);

	NVMEV_NAMESPACE_FINAL(nvmev_vdev);
	NVMEV_STORAGE_FINAL(nvmev_vdev);

	if (io_using_dma) {
		ioat_dma_cleanup();
	}

	for (i = 0; i < nvmev_vdev->nr_sq; i++) {
		kfree(nvmev_vdev->sqes[i]);
		if (nvmev_vdev->reqqs[i]) {
			if (nvmev_vdev->reqqs[i]->internal_req) {
				kfree(nvmev_vdev->reqqs[i]->internal_req);
				nvmev_vdev->reqqs[i]->internal_req = NULL;
			}
			kfree(nvmev_vdev->reqqs[i]);
		}
	}

	NVMEV_EXIT_LOG("sqes reqqs freed\n");

	for (i = 0; i < nvmev_vdev->nr_cq; i++) {
		kfree(nvmev_vdev->cqes[i]);
	}

	NVMEV_EXIT_LOG("cqes freed\n");

	misc_deregister(&nvmev_miscdevice);

	NVMEV_EXIT_LOG("misc device deregistered\n");

	VDEV_FINALIZE(nvmev_vdev);

	NVMEV_EXIT_LOG("Virtual NVMe device closed\n");
}

MODULE_LICENSE("GPL v2");
module_init(NVMeV_init);
module_exit(NVMeV_exit);
