// SPDX-License-Identifier: GPL-2.0-only

#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/ktime.h>
#include <linux/highmem.h>
#include <linux/sched/clock.h>

#include "nvmev.h"
#include "dma.h"

#if (SUPPORTED_SSD_TYPE(CONV) || SUPPORTED_SSD_TYPE(ZNS))
#include "ssd.h"
#else
struct buffer;
#endif

#undef PERF_DEBUG

#define sq_entry(entry_id) sq->sq[SQ_ENTRY_TO_PAGE_NUM(entry_id)][SQ_ENTRY_TO_PAGE_OFFSET(entry_id)]
#define cq_entry(entry_id) cq->cq[CQ_ENTRY_TO_PAGE_NUM(entry_id)][CQ_ENTRY_TO_PAGE_OFFSET(entry_id)]

extern bool io_using_dma;

static inline unsigned int __get_io_worker(int sqid)
{
#ifdef CONFIG_NVMEV_IO_WORKER_BY_SQ
	return (sqid - 1) % nvmev_vdev->config.nr_io_workers;
#else
	return nvmev_vdev->io_worker_turn;
#endif
}

static inline unsigned long long __get_wallclock(void)
{
	//TODO: hack to set the wallclock to the current time of cpu 0
	// return cpu_clock(nvmev_vdev->config.cpu_nr_dispatcher[0]);
	return ktime_get_ns();
}

static void push_completed_req(struct list_head *head, struct nvmev_internal_request *req)
{
	struct list_head *pos;
	struct nvmev_internal_request *entry;

	if (list_empty(head)) {
		list_add(&req->list, head);
		return;
	}

	list_for_each(pos, head) {
		entry = list_entry(pos, struct nvmev_internal_request, list);
		if (req->nsecs_end <= entry->nsecs_end) {
			list_add(&req->list, pos);
			return;
		}
	}
	list_add_tail(&req->list, head);
}

static void delete_completed_req(struct nvmev_internal_request *req)
{
	list_del(&req->list);
}

static struct nvmev_internal_request *pop_completed_req(struct list_head *head)
{
	if (list_empty(head)) {
		return NULL;
	}

	struct nvmev_internal_request *req =
		list_first_entry(head, struct nvmev_internal_request, list);
	// NVMEV_ERROR("pop_completed_req: %p\n", req);
	// NVMEV_ERROR("pop_completed_req cmd %p, sq_id %d, nsecs_end %llu\n", req->cmd, req->io_cmd.sqid, req->nsecs_end);

	return req;
}

static inline size_t __cmd_io_offset(struct nvme_rw_command *cmd)
{
	return (cmd->slba) << LBA_BITS;
}

static inline size_t __cmd_io_size(struct nvme_rw_command *cmd)
{
	return (cmd->length + 1) << LBA_BITS;
}

static unsigned int __do_perform_io(int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	struct nvme_rw_command *cmd = &sq_entry(sq_entry).rw;
	size_t offset;
	size_t length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	u64 paddr;
	u64 *paddr_list = NULL;
	size_t nsid = cmd->nsid - 1; // 0-based

	offset = __cmd_io_offset(cmd);
	length = __cmd_io_size(cmd);
	remaining = length;

	if (offset + length > nvmev_vdev->config.storage_size) {
		NVMEV_ERROR("Invalid IO request: offset %lu, length %lu, SSD size %lu\n",
			    offset, length, nvmev_vdev->config.storage_size);
		return 0;
	}

	// NVMEV_ERROR("IO request: offset %lu, length %lu, nsid %lu\n", offset, length, nsid);

	// ASSERT(nvmev_vdev->ns[nsid].mapped != NULL);

	while (remaining) {
		size_t io_size;
		void *vaddr;
		size_t mem_offs = 0;

		prp_offs++;
		if (prp_offs == 1) {
			paddr = cmd->prp1;
		} else if (prp_offs == 2) {
			paddr = cmd->prp2;
			if (remaining > PAGE_SIZE) {
				paddr_list = kmap_atomic_pfn(PRP_PFN(paddr)) +
					     (paddr & PAGE_OFFSET_MASK);
				paddr = paddr_list[prp2_offs++];
			}
		} else {
			paddr = paddr_list[prp2_offs++];
		}

		// ASSERT(paddr != 0);

		vaddr = kmap_atomic_pfn(PRP_PFN(paddr));

		// ASSERT(vaddr != NULL);

		io_size = min_t(size_t, remaining, PAGE_SIZE);

		if (paddr & PAGE_OFFSET_MASK) {
			mem_offs = paddr & PAGE_OFFSET_MASK;
			if (io_size + mem_offs > PAGE_SIZE)
				io_size = PAGE_SIZE - mem_offs;
		}

		if (cmd->opcode == nvme_cmd_write || cmd->opcode == nvme_cmd_zone_append) {
			memcpy(nvmev_vdev->ns[nsid].mapped + offset, vaddr + mem_offs, io_size);
		} else if (cmd->opcode == nvme_cmd_read) {
			memcpy(vaddr + mem_offs, nvmev_vdev->ns[nsid].mapped + offset, io_size);
		}

		kunmap_atomic(vaddr);

		remaining -= io_size;
		offset += io_size;
	}

	if (paddr_list != NULL)
		kunmap_atomic(paddr_list);

	return length;
}

static u64 paddr_list[513] = {
	0,
}; // Not using index 0 to make max index == num_prp
static unsigned int __do_perform_io_using_dma(int sqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	struct nvme_rw_command *cmd = &sq_entry(sq_entry).rw;
	size_t offset;
	size_t length, remaining;
	int prp_offs = 0;
	int prp2_offs = 0;
	int num_prps = 0;
	u64 paddr;
	u64 *tmp_paddr_list = NULL;
	size_t io_size;
	size_t mem_offs = 0;

	offset = __cmd_io_offset(cmd);
	length = __cmd_io_size(cmd);
	remaining = length;

	memset(paddr_list, 0, sizeof(paddr_list));
	/* Loop to get the PRP list */
	while (remaining) {
		io_size = 0;

		prp_offs++;
		if (prp_offs == 1) {
			paddr_list[prp_offs] = cmd->prp1;
		} else if (prp_offs == 2) {
			paddr_list[prp_offs] = cmd->prp2;
			if (remaining > PAGE_SIZE) {
				tmp_paddr_list = kmap_atomic_pfn(PRP_PFN(paddr_list[prp_offs])) +
						 (paddr_list[prp_offs] & PAGE_OFFSET_MASK);
				paddr_list[prp_offs] = tmp_paddr_list[prp2_offs++];
			}
		} else {
			paddr_list[prp_offs] = tmp_paddr_list[prp2_offs++];
		}

		io_size = min_t(size_t, remaining, PAGE_SIZE);

		if (paddr_list[prp_offs] & PAGE_OFFSET_MASK) {
			mem_offs = paddr_list[prp_offs] & PAGE_OFFSET_MASK;
			if (io_size + mem_offs > PAGE_SIZE)
				io_size = PAGE_SIZE - mem_offs;
		}

		remaining -= io_size;
	}
	num_prps = prp_offs;

	if (tmp_paddr_list != NULL)
		kunmap_atomic(tmp_paddr_list);

	remaining = length;
	prp_offs = 1;

	/* Loop for data transfer */
	while (remaining) {
		size_t page_size;
		mem_offs = 0;
		io_size = 0;
		page_size = 0;

		paddr = paddr_list[prp_offs];
		page_size = min_t(size_t, remaining, PAGE_SIZE);

		/* For non-page aligned paddr, it will never be between continuous PRP list (Always first paddr)  */
		if (paddr & PAGE_OFFSET_MASK) {
			mem_offs = paddr & PAGE_OFFSET_MASK;
			if (page_size + mem_offs > PAGE_SIZE) {
				page_size = PAGE_SIZE - mem_offs;
			}
		}

		for (prp_offs++; prp_offs <= num_prps; prp_offs++) {
			if (paddr_list[prp_offs] == paddr_list[prp_offs - 1] + PAGE_SIZE)
				page_size += PAGE_SIZE;
			else
				break;
		}

		io_size = min_t(size_t, remaining, page_size);

		if (cmd->opcode == nvme_cmd_write || cmd->opcode == nvme_cmd_zone_append) {
			ioat_dma_submit(paddr, nvmev_vdev->config.storage_start + offset, io_size);
		} else if (cmd->opcode == nvme_cmd_read) {
			ioat_dma_submit(nvmev_vdev->config.storage_start + offset, paddr, io_size);
		}

		remaining -= io_size;
		offset += io_size;
	}

	return length;
}

static void __insert_req_sorted(unsigned int entry, struct nvmev_io_worker *worker,
				unsigned long nsecs_target)
{
	/**
	 * Requests are placed in @work_queue sorted by their target time.
	 * @work_queue is statically allocated and the ordered list is
	 * implemented by chaining the indexes of entries with @prev and @next.
	 * This implementation is nasty but we do this way over dynamically
	 * allocated linked list to minimize the influence of dynamic memory allocation.
	 * Also, this O(n) implementation can be improved to O(logn) scheme with
	 * e.g., red-black tree but....
	 */
	if (worker->io_seq == -1) {
		worker->io_seq = entry;
		worker->io_seq_end = entry;
	} else {
		unsigned int curr = worker->io_seq_end;

		while (curr != -1) {
			if (worker->work_queue[curr].nsecs_target <= worker->latest_nsecs)
				break;

			if (worker->work_queue[curr].nsecs_target <= nsecs_target)
				break;

			curr = worker->work_queue[curr].prev;
		}

		if (curr == -1) { /* Head inserted */
			worker->work_queue[worker->io_seq].prev = entry;
			worker->work_queue[entry].next = worker->io_seq;
			worker->io_seq = entry;
		} else if (worker->work_queue[curr].next == -1) { /* Tail */
			worker->work_queue[entry].prev = curr;
			worker->io_seq_end = entry;
			worker->work_queue[curr].next = entry;
		} else { /* In between */
			worker->work_queue[entry].prev = curr;
			worker->work_queue[entry].next = worker->work_queue[curr].next;

			worker->work_queue[worker->work_queue[entry].next].prev = entry;
			worker->work_queue[curr].next = entry;
		}
	}
}

static struct nvmev_io_worker *__allocate_work_queue_entry(int sqid, unsigned int *entry)
{
	unsigned int io_worker_turn = __get_io_worker(sqid);
	struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[io_worker_turn];
	unsigned int e = worker->free_seq;
	struct nvmev_io_work *w = worker->work_queue + e;

	if (w->next >= NR_MAX_PARALLEL_IO) {
		WARN_ON_ONCE("IO queue is almost full");
		return NULL;
	}

	if (++io_worker_turn == nvmev_vdev->config.nr_io_workers)
		io_worker_turn = 0;
	nvmev_vdev->io_worker_turn = io_worker_turn;

	worker->free_seq = w->next;
	BUG_ON(worker->free_seq >= NR_MAX_PARALLEL_IO);
	*entry = e;

	return worker;
}

static void __enqueue_io_req_to_ftl(int dispatcher_id, int FTL_id, int sqid, int cqid, int sq_entry,
				    int command_id, int opcode, uint64_t slpa, uint64_t nlb, struct nvmev_internal_request *req)
{
	int *new_tail = &(nvmev_vdev->in_que_tail[dispatcher_id][FTL_id]);
	NVMEV_ASSERT(new_tail);

	struct nvmev_io_cmd *cmd = &nvmev_vdev->in_que[dispatcher_id][FTL_id][*new_tail];
	NVMEV_ASSERT(cmd);

	if (atomic_read(&(cmd->is_completed)) == 1) {
		NVMEV_ERROR("cmd is not completed: %d %d %d %d %d %lld %lld\n", dispatcher_id,
			    FTL_id, sqid, cqid, sq_entry, slpa, nlb);
		NVMEV_ASSERT(0);
	}

	rmb();

	req->remaining_req++;

	cmd->sqid = sqid;
	cmd->cqid = cqid;
	cmd->sq_entry = sq_entry;
	cmd->command_id = command_id;
	cmd->opcode = opcode;
	cmd->slpa = slpa;
	cmd->nlb = nlb;
	cmd->nsecs = req->nsecs_start;
	mb(); /* FTL shall see the updated cmd at once */

	atomic_set(&(cmd->is_completed), 1);

	(*new_tail)++;
	if (*new_tail >= nvmev_vdev->config.in_que_size) 
		*new_tail = 0;
	// *new_tail = (*new_tail + 1) % nvmev_vdev->config.in_que_size;
	// NVMEV_ERROR("__enqueue io req to ftl: dispatcher_id %d, FTL_id %d, sqid %d, cqid %d, sq_entry %d, slpa %lld, nlb %lld, command_id %d, new_tail %d, in_que_size %u\n", dispatcher_id, FTL_id, sqid, cqid, sq_entry, slpa, nlb, cmd->command_id, *new_tail, nvmev_vdev->config.in_que_size);
}

#ifdef PERF_DEBUG
static unsigned long long req_start[512] = { 0 };
static int entry_id[512] = { 0 }, req_cnt = 0;

static void add_enqueue_state(unsigned long long nsecs_start, int sq_entry_id)
{
	req_start[req_cnt] = nsecs_start;
	entry_id[req_cnt++] = sq_entry_id;

	if (req_cnt == 512) {
		int i;
		for (i = 0; i < 512; i++) {
			NVMEV_FINE_LOG("req_start %d %llu %llu\n", i, entry_id[i], req_start[i]);
		}
		req_cnt = 0;
	}
}
#endif

static void enqueue_io_req_to_ftl(int dispatcher_id, int sqid, int cqid, int sq_entry_id)
{
	struct nvmev_submission_queue *sq = NULL;
	struct nvmev_internal_request *req = NULL;
	struct nvmev_config *config = NULL;
	unsigned int entry;
	uint64_t slpa, nlb, FTL_id, cur_nlb;
	__u16 cid;
	__u8 opcode;

	sq = nvmev_vdev->sqes[sqid];
	NVMEV_ASSERT(sq);
	struct nvme_rw_command *cmd = &sq_entry(sq_entry_id).rw;
	// NVMEV_ERROR("sq_entry_id %d\n", sq_entry_id);
	NVMEV_ASSERT(cmd);
	cid = cmd->command_id;
	opcode = cmd->opcode;

	if (cid >= 65536) {
		NVMEV_ERROR("cid %u, queue_size %d\n", cid, sq->queue_size);
		NVMEV_ASSERT(0);
	}
	ASSERT(cid < 65536);

	NVMEV_ASSERT(!list_empty(&sq->free_req_list));
	req = list_first_entry(&sq->free_req_list, struct nvmev_internal_request, list);
	NVMEV_ASSERT(req);
	
	list_del(&req->list);
	
	NVMEV_ASSERT(sq->tag_mapper[cid] == 0);
	sq->tag_mapper[cid] = req->interl_req_id;
	NVMEV_ASSERT(req->interl_req_id != 0);

	// req = &(nvmev_vdev->reqqs[sqid]->internal_req[req_id]);
	config = &nvmev_vdev->config;

	NVMEV_ASSERT(config);

	if ((sqid - 1) % config->nr_dispatcher != dispatcher_id) {
		NVMEV_ERROR("sqid %d, dispatcher_id %d\n", sqid, dispatcher_id);
		NVMEV_ASSERT(0);
		// NVMEV_ASSERT(sqid % config->nr_dispatcher == dispatcher_id - 1, "sqid %d, dispatcher_id %d\n", sqid, dispatcher_id);
	}
	NVMEV_ASSERT(sqid == cqid);

	// NVMEV_ERROR("command id %u\n", cid);

	// WARNING: 可能存在sq_entry_id被回收的情况
	if (req->is_flight) {
		NVMEV_ERROR("req is already in flight: %d %d %d %d %u %lld %lld\n", dispatcher_id,
			    sqid, cqid, sq_entry_id, req->command_id, req->nsecs_start,
			    req->nsecs_end);
		NVMEV_ERROR("now tick: %lld\n", ktime_get_ns());
		NVMEV_ERROR("now req: %d %d %d %d %u\n", dispatcher_id, sqid, cqid,
			    sq_entry_id, cid);
		NVMEV_ASSERT(0);
	}
	NVMEV_ASSERT(req->is_flight == false);
	req->is_flight = true;

	req->command_id = cid;
	req->nsecs_start = req->nsecs_end = ktime_get_ns();

#ifdef PERF_DEBUG
	add_enqueue_state(req->nsecs_start, sq_entry_id);
#endif

	slpa = (sq_entry(sq_entry_id).rw.slba);
	nlb = (sq_entry(sq_entry_id).rw.length) + 1;

	// split I/O request into multiple requests
	// send multiple requests to different FTL threads
	uint64_t seg_size = 1 + config->split_ratio;
	FTL_id = ((slpa / config->FTL_split_units) % seg_size) == 0 ? 0 : 1;
	FTL_id = config->nr_io_workers == 1 ? 0 : FTL_id;

	// NVMEV_ERROR("slpa = %lld, nlb = %lld, FTL_split_units = %d, nr_io_workers = %d\n", slpa, nlb, config->FTL_split_units, config->nr_io_workers);

	NVMEV_ASSERT(config->FTL_split_units != 0);
	NVMEV_ASSERT(config->nr_io_workers != 0);
	NVMEV_ASSERT(req->remaining_req == 0);

	while (nlb) {
		cur_nlb = min_t(uint64_t, nlb, config->seg_units[FTL_id]);

		__enqueue_io_req_to_ftl(dispatcher_id, FTL_id, sqid, cqid, sq_entry_id, cid, opcode, slpa, cur_nlb, req);
		slpa += cur_nlb;
		nlb -= cur_nlb;
		// TODO: revise to LPN-based fashion
		FTL_id++;
		if (FTL_id >= config->nr_io_workers)
			FTL_id = 0;
		// FTL_id = (FTL_id + 1) % config->nr_io_workers;
	}
}

static void hack_enqueue_io_req_to_ftl(int dispatcher_id, int sqid, int cqid, int sq_entry)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	struct nvme_rw_command *cmd = &sq_entry(sq_entry).rw;
	unsigned int command_id = cmd->command_id;
	unsigned int status = 0;
	unsigned int result0 = 0;
	unsigned int result1 = 0;

	struct nvmev_completion_queue *cq = nvmev_vdev->cqes[cqid];
	int cq_head = cq->cq_head;
	struct nvme_completion *cqe = &cq_entry(cq_head);

	spin_lock(&cq->entry_lock);
	cqe->command_id = command_id;
	cqe->sq_id = sqid;
	cqe->sq_head = sq_entry;
	cqe->result0 = result0;
	cqe->result1 = result1;

	// cqe->status = cq->phase | (status << 1);

	mb();

	WRITE_ONCE(cqe->status, cq->phase | (status << 1));

	if (++cq_head == cq->queue_size) {
		cq_head = 0;
		cq->phase = !cq->phase;
	}

	cq->cq_head = cq_head;
	cq->interrupt_ready = true;
	spin_unlock(&cq->entry_lock);
}

static void __enqueue_io_req(int sqid, int cqid, int sq_entry, unsigned long long nsecs_start,
			     struct nvmev_result *ret)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	struct nvmev_io_worker *worker;
	struct nvmev_io_work *w;
	unsigned int entry;

	worker = __allocate_work_queue_entry(sqid, &entry);
	if (!worker)
		return;

	w = worker->work_queue + entry;

	NVMEV_DEBUG_VERBOSE("%s/%u[%d], sq %d cq %d, entry %d, %llu + %llu\n", worker->thread_name,
			    entry, sq_entry(sq_entry).rw.opcode, sqid, cqid, sq_entry, nsecs_start,
			    ret->nsecs_target - nsecs_start);

	/////////////////////////////////
	w->sqid = sqid;
	w->cqid = cqid;
	w->sq_entry = sq_entry;
	w->command_id = sq_entry(sq_entry).common.command_id;
	w->nsecs_start = nsecs_start;
	w->nsecs_enqueue = local_clock();
	w->nsecs_target = ret->nsecs_target;
	w->status = ret->status;
	w->is_completed = false;
	w->is_copied = false;
	w->prev = -1;
	w->next = -1;

	w->is_internal = false;
	mb(); /* IO worker shall see the updated w at once */

	__insert_req_sorted(entry, worker, ret->nsecs_target);
}

void schedule_internal_operation(int sqid, unsigned long long nsecs_target,
				 struct buffer *write_buffer, size_t buffs_to_release)
{
	struct nvmev_io_worker *worker;
	struct nvmev_io_work *w;
	unsigned int entry;

	worker = __allocate_work_queue_entry(sqid, &entry);
	if (!worker)
		return;

	w = worker->work_queue + entry;

	NVMEV_DEBUG_VERBOSE("%s/%u, internal sq %d, %llu + %llu\n", worker->thread_name, entry,
			    sqid, local_clock(), nsecs_target - local_clock());

	/////////////////////////////////
	w->sqid = sqid;
	w->nsecs_start = w->nsecs_enqueue = local_clock();
	w->nsecs_target = nsecs_target;
	w->is_completed = false;
	w->is_copied = true;
	w->prev = -1;
	w->next = -1;

	w->is_internal = true;
	w->write_buffer = write_buffer;
	w->buffs_to_release = buffs_to_release;
	mb(); /* IO worker shall see the updated w at once */

	__insert_req_sorted(entry, worker, nsecs_target);
}

static void __reclaim_completed_reqs(void)
{
	unsigned int turn;

	for (turn = 0; turn < nvmev_vdev->config.nr_io_workers; turn++) {
		struct nvmev_io_worker *worker;
		struct nvmev_io_work *w;

		unsigned int first_entry = -1;
		unsigned int last_entry = -1;
		unsigned int curr;
		int nr_reclaimed = 0;

		worker = &nvmev_vdev->io_workers[turn];

		first_entry = worker->io_seq;
		curr = first_entry;

		while (curr != -1) {
			w = &worker->work_queue[curr];
			if (w->is_completed == true && w->is_copied == true &&
			    w->nsecs_target <= worker->latest_nsecs) {
				last_entry = curr;
				curr = w->next;
				nr_reclaimed++;
			} else {
				break;
			}
		}

		if (last_entry != -1) {
			w = &worker->work_queue[last_entry];
			worker->io_seq = w->next;
			if (w->next != -1) {
				worker->work_queue[w->next].prev = -1;
			}
			w->next = -1;

			w = &worker->work_queue[first_entry];
			w->prev = worker->free_seq_end;

			w = &worker->work_queue[worker->free_seq_end];
			w->next = first_entry;

			worker->free_seq_end = last_entry;
			NVMEV_DEBUG_VERBOSE("%s: %u -- %u, %d\n", __func__, first_entry, last_entry,
					    nr_reclaimed);
		}
	}
}

static size_t __nvmev_proc_io(int dispatcher_id, int sqid, int sq_entry_id, size_t *io_size)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	unsigned long long nsecs_start = __get_wallclock();
	struct nvme_command *cmd = &sq_entry(sq_entry_id);
#if (BASE_SSD == KV_PROTOTYPE)
	uint32_t nsid = 0; // Some KVSSD programs give 0 as nsid for KV IO
#else
	uint32_t nsid = cmd->common.nsid - 1;
#endif
	struct nvmev_ns *ns = &nvmev_vdev->ns[nsid];

#ifdef PERF_DEBUG
	unsigned long long prev_clock = local_clock();
	unsigned long long prev_clock2 = 0;
	unsigned long long prev_clock3 = 0;
	unsigned long long prev_clock4 = 0;
	static unsigned long long clock1 = 0;
	static unsigned long long clock2 = 0;
	static unsigned long long clock3 = 0;
	static unsigned long long counter = 0;
#endif

	// NVMEV_ERROR("sq_id %d, sq_entry_id = %d, opcode = %d, slba = %lld, length = %lu\n", sqid, sq_entry_id, cmd->rw.opcode, cmd->rw.slba, __cmd_io_size(&sq_entry(sq_entry_id).rw));

	*io_size = __cmd_io_size(&sq_entry(sq_entry_id).rw); // for stat collection

	__do_perform_io(sqid, sq_entry_id);

	// put command into FTL thread queue
	static int hack_num = 200;
	static int no_hack_num = 0;
	if (hack_num-- > 0) {
		hack_enqueue_io_req_to_ftl(dispatcher_id, sqid, sq->cqid, sq_entry_id);
		NVMEV_HACK_LOG("HACK: %d\n", hack_num);
		mb();
	} else {
		enqueue_io_req_to_ftl(dispatcher_id, sqid, sq->cqid, sq_entry_id);
	}

#ifdef PERF_DEBUG
	prev_clock2 = local_clock();
#endif

	// __enqueue_io_req(sqid, sq->cqid, sq_entry, nsecs_start, &ret);

#ifdef PERF_DEBUG
	prev_clock3 = local_clock();
#endif

	// __reclaim_completed_reqs();

#ifdef PERF_DEBUG
	prev_clock4 = local_clock();

	clock1 += (prev_clock2 - prev_clock);
	clock2 += (prev_clock3 - prev_clock2);
	clock3 += (prev_clock4 - prev_clock3);
	counter++;

	if (counter > 1000) {
		NVMEV_DEBUG("LAT: %llu, ENQ: %llu, CLN: %llu\n", clock1 / counter, clock2 / counter,
			    clock3 / counter);
		clock1 = 0;
		clock2 = 0;
		clock3 = 0;
		counter = 0;
	}
#endif
	return true;
}

#ifdef PERF_DEBUG
static unsigned long long nr_sec_interval = 0, last_sq_rec_time = 0, interval_count = 0;

static void add_proc_sq_stats() {
	unsigned long long time_diff;
	
	if (last_sq_rec_time > 0) {
		time_diff = ktime_get_ns() - last_sq_rec_time;
		if (time_diff <= 200 * 100) {
			nr_sec_interval += time_diff;
			interval_count++;
		}
	}
	last_sq_rec_time = ktime_get_ns();
	if (interval_count > 0 && interval_count % LOG_PRINT_INTERVAL == 0) {
		NVMEV_QUEUE_LOG("SQ: avg_interval %llu, counter %llu\n",
			     nr_sec_interval / interval_count, interval_count);
	}
}
#endif

int nvmev_proc_io_sq(int dispatcher_id, int sqid, int new_db, int old_db)
{
	struct nvmev_submission_queue *sq = nvmev_vdev->sqes[sqid];
	int num_proc = new_db - old_db;
	int seq;
	int sq_entry = old_db;
	int latest_db;

	if (unlikely(!sq))
		return old_db;
	if (unlikely(num_proc < 0))
		num_proc += sq->queue_size;


	for (seq = 0; seq < num_proc; seq++) {
		size_t io_size;
		if (!__nvmev_proc_io(dispatcher_id, sqid, sq_entry, &io_size))
			break;

		if (++sq_entry == sq->queue_size) {
			sq_entry = 0;
		}
		sq->stat.nr_dispatched++;
		sq->stat.nr_in_flight++;
		sq->stat.total_io += io_size;
	}
	sq->stat.nr_dispatch++;
	sq->stat.max_nr_in_flight = max_t(int, sq->stat.max_nr_in_flight, sq->stat.nr_in_flight);

	latest_db = (old_db + seq) % sq->queue_size;
	return latest_db;
}

void nvmev_proc_io_cq(int cqid, int new_db, int old_db)
{
	struct nvmev_completion_queue *cq = nvmev_vdev->cqes[cqid];
	int i;
	for (i = old_db; i != new_db; i++) {
		int sqid = cq_entry(i).sq_id;
		if (i >= cq->queue_size) {
			i = -1;
			continue;
		}

		/* Should check the validity here since SPDK deletes SQ immediately
		 * before processing associated CQes */
		if (!nvmev_vdev->sqes[sqid]) continue;

		nvmev_vdev->sqes[sqid]->stat.nr_in_flight--;
	}

	cq->cq_tail = new_db - 1;
	if (new_db == -1)
		cq->cq_tail = cq->queue_size - 1;
}

static void __fill_cq_result_for_FTL(struct nvmev_io_cmd *cmd)
{
	NVMEV_ASSERT(cmd);
	int sqid = cmd->sqid;
	int cqid = cmd->cqid;
	int sq_entry = cmd->sq_entry;
	unsigned int command_id = cmd->command_id;
	unsigned int status = cmd->status;
	unsigned int result0 = cmd->result0;
	unsigned int result1 = cmd->result1;
	struct nvmev_completion_queue *cq;
	int cq_head;
	struct nvme_completion *cqe;

	// NVMEV_ERROR(
	//	"__fill_cq_result_for_FTL: sqid %d, cqid %d, sq_entry %d, command_id %d, status %d, result0 %d, result1 %d, nsecs %lld\n",
	// 	sqid, cqid, sq_entry, command_id, status, result0, result1, cmd->nsecs);

	cq = nvmev_vdev->cqes[cqid];
	NVMEV_ASSERT(cq);

	cq_head = cq->cq_head;
	cqe = &cq_entry(cq_head);
	NVMEV_ASSERT(cqe);

	// spin_lock(&cq->entry_lock);
	cqe->command_id = command_id;
	cqe->sq_id = sqid;
	cqe->sq_head = sq_entry;
	cqe->result0 = result0;
	cqe->result1 = result1;

	mb();

	// WRITE_ONCE(cqe->status, cq->phase | (status << 1));
	cqe->status = cq->phase | (status << 1);

	if (++cq_head == cq->queue_size) {
		cq_head = 0;
		cq->phase = !cq->phase;
	}

	cq->cq_head = cq_head;
#ifdef PERF_DEBUG
	if (cq->interrupt_ready == false) {
		cq->last_exp_intr_time = ktime_get_ns();
	}
#endif
	cq->interrupt_ready = true;
	// spin_unlock(&cq->entry_lock);
}

static void copy_io_result(struct nvmev_internal_request *req, struct nvmev_io_cmd *cmd)
{
	NVMEV_ASSERT(req);
	NVMEV_ASSERT(cmd);

	// NVMEV_ERROR("copy_io_result for cmd: %d %d %d %lld\n", cmd->sqid, cmd->cqid, cmd->command_id, cmd->nsecs);

	memcpy(&(req->io_cmd), cmd, sizeof(struct nvmev_io_cmd));

	// NVMEV_ERROR("copy_io_result for req: %d %d %d %lld\n", req->io_cmd.sqid, req->io_cmd.cqid, req->io_cmd.command_id, req->io_cmd.nsecs);
}

// #define PERF_CHECK_STATS
#ifdef PERF_CHECK_STATS
static int proc_num[3] = { 0, 0, 0 }, diff_num[3] = { 0, 0, 0 }, rec_interval_num[3] = { 0, 0, 0 };
static uint64_t exp_necs[3] = { 0, 0, 0 }, diff_necs[3] = { 0, 0, 0 }, SGX_necs[3] = { 0, 0, 0 },
		rec_interval[3] = { 0, 0, 0 };
static int sum_proc_num[3] = { 0, 0, 0 }, sum_diff_num[3] = { 0, 0, 0 },
	   sum_rec_interval_num[3] = { 0, 0, 0 };
static uint64_t sum_exp_necs[3] = { 0, 0, 0 }, sum_diff_necs[3] = { 0, 0, 0 },
		sum_SGX_necs[3] = { 0, 0, 0 }, sum_rec_interval[3] = { 0, 0, 0 };
static uint64_t last_req_rec_end_time = 0;

static int check_num[3] = { 0, 0, 0 }, check_interval_num[3] = { 0, 0, 0 };
static uint64_t necs_total[3] = { 0, 0, 0 }, necs_diff[3] = { 0, 0, 0 },
		necs_interval[3] = { 0, 0, 0 };
static int sum_check_num[3] = { 0, 0, 0 }, sum_check_interval_num[3] = { 0, 0, 0 };
static uint64_t sum_necs_total[3] = { 0, 0, 0 }, sum_necs_diff[3] = { 0, 0, 0 },
		sum_necs_interval[3] = { 0, 0, 0 };
static uint64_t last_req_check_end_time = 0;

static unsigned long long start_time[512], end_time[512], total_time[512], diff_time[512],
	interval[512], cnt = 0;

static char *opcode_str[3] = { "ALL", "WRITE", "READ" };

static void add_rev_stats(struct nvmev_internal_request *req)
{
	unsigned long long time_diff = 0;
	uint64_t tick_now = 0;
	int opcode;

	opcode = req->cmd->rw.opcode;

	NVMEV_ASSERT(opcode != nvme_cmd_flush);
	NVMEV_ASSERT(opcode < 3);

	proc_num[opcode]++;
	proc_num[0]++;
	exp_necs[opcode] += req->nsecs_end - req->nsecs_start;

	tick_now = __get_wallclock();
	if (tick_now >= req->nsecs_end) {
		diff_num[opcode]++;
		diff_necs[opcode] += tick_now - req->nsecs_end;
	}
	SGX_necs[opcode] += tick_now - req->nsecs_start;

	if (last_req_rec_end_time > 0) {
		time_diff = tick_now - last_req_rec_end_time;
		if (time_diff <= 200 * 100) {
			rec_interval_num[opcode]++;
			rec_interval[opcode] += tick_now - last_req_rec_end_time;
		}
	}
	last_req_rec_end_time = tick_now;

	if (proc_num[0] % LOG_PRINT_INTERVAL == 0) {
		exp_necs[0] = exp_necs[1] + exp_necs[2];
		diff_necs[0] = diff_necs[1] + diff_necs[2];
		diff_num[0] = diff_num[1] + diff_num[2];
		SGX_necs[0] = SGX_necs[1] + SGX_necs[2];
		rec_interval[0] = rec_interval[1] + rec_interval[2];
		rec_interval_num[0] = rec_interval_num[1] + rec_interval_num[2];

		int i;
		for (i = 0; i < 3; i++) {
			sum_proc_num[i] += proc_num[i];
			sum_exp_necs[i] += exp_necs[i];
			sum_diff_num[i] += diff_num[i];
			sum_diff_necs[i] += diff_necs[i];
			sum_SGX_necs[i] += SGX_necs[i];
			sum_rec_interval[i] += rec_interval[i];
			sum_rec_interval_num[i] += rec_interval_num[i];

			if (sum_proc_num[i] > 0) {
				NVMEV_DISPATCHER_LOG(
					"Received [%s] SUM: %d th, avg_ssd_lat (us): %lld\n",
					opcode_str[i], sum_proc_num[i],
					sum_exp_necs[i] / sum_proc_num[i] / 1000);
				NVMEV_DISPATCHER_LOG(
					"Received [%s] SUM: %d th, avg_sgx_lat (us): %lld\n",
					opcode_str[i], sum_proc_num[i],
					sum_SGX_necs[i] / sum_proc_num[i] / 1000);
				if (sum_rec_interval_num[i] > 0) {
					NVMEV_DISPATCHER_LOG(
						"Received [%s] SUM: %d th, avg_interval_lat (us): %lld\n",
						opcode_str[i], sum_rec_interval_num[i],
						sum_rec_interval[i] / sum_rec_interval_num[i] /
							1000);
				}
				if (sum_diff_num[i] > 0) {
					NVMEV_DISPATCHER_LOG(
						"Received [%s] SUM: %d th, avg_outer_lat (us): %lld\n",
						opcode_str[i], sum_diff_num[i],
						sum_diff_necs[i] / sum_diff_num[i] / 1000);
				}

				if (proc_num[i] > 0) {
					NVMEV_DISPATCHER_LOG(
						"Received [%s] avg_ssd_lat (us): %lld\n",
						opcode_str[i], exp_necs[i] / proc_num[i] / 1000);
					NVMEV_DISPATCHER_LOG(
						"Received [%s] avg_sgx_lat (us): %lld\n",
						opcode_str[i], SGX_necs[i] / proc_num[i] / 1000);
					if (rec_interval_num[i] > 0) {
						NVMEV_DISPATCHER_LOG(
							"Received [%s] avg_interval_lat (us): %lld\n",
							opcode_str[i],
							rec_interval[i] / rec_interval_num[i] /
								1000);
					}
					if (diff_num[i] > 0) {
						NVMEV_DISPATCHER_LOG(
							"Received [%s] diff_num: %d, avg_outer_lat (us): %lld\n",
							opcode_str[i], diff_num[i],
							diff_necs[i] / diff_num[i] / 1000);
					}
				}
				proc_num[i] = exp_necs[i] = SGX_necs[i] = diff_num[i] =
					diff_necs[i] = rec_interval[i] = rec_interval_num[i] = 0;
			}
		}
	}
}

static void add_check_stats(struct nvmev_internal_request *check_req)
{
	unsigned long long time_diff = 0;
	uint64_t tick_now = 0;
	int opcode;

	opcode = check_req->cmd->rw.opcode;
	check_num[opcode]++;
	check_num[0]++;

	tick_now = __get_wallclock();
	necs_total[opcode] += check_req->nsecs_end - check_req->nsecs_start;
	necs_diff[opcode] += __get_wallclock() - check_req->nsecs_end;

	if (last_req_check_end_time > 0) {
		time_diff = tick_now - last_req_check_end_time;
		if (time_diff <= 200 * 100) {
			check_interval_num[opcode]++;
			necs_interval[opcode] += tick_now - last_req_check_end_time;
		}
	}

	last_req_check_end_time = tick_now;

	start_time[cnt] = check_req->nsecs_start;
	end_time[cnt] = check_req->nsecs_end;
	total_time[cnt] = check_req->nsecs_end - check_req->nsecs_start;
	diff_time[cnt] = __get_wallclock() - check_req->nsecs_end;
	if (cnt != 0) {
		interval[cnt] = start_time[cnt] - start_time[cnt - 1];
	}
	cnt++;

	if (cnt == 512) {
		int i;
		for (i = 0; i < 512; i++) {
			// NVMEV_FINE_LOG("REQ %d, start %llu, end %llu, total %llu, diff %llu, interval %llu\n", i, start_time[i], end_time[i], total_time[i], diff_time[i], interval[i]);
		}
		cnt = 0;
	}

	if (check_num[0] % LOG_PRINT_INTERVAL == 0) {
		necs_total[0] = necs_total[1] + necs_total[2];
		necs_diff[0] = necs_diff[1] + necs_diff[2];
		necs_interval[0] = necs_interval[1] + necs_interval[2];
		check_interval_num[0] = check_interval_num[1] + check_interval_num[2];

		int i;
		for (i = 0; i < 3; i++) {
			sum_necs_total[i] += necs_total[i];
			sum_necs_diff[i] += necs_diff[i];
			sum_check_num[i] += check_num[i];
			sum_necs_interval[i] += necs_interval[i];
			sum_check_interval_num[i] += check_interval_num[i];

			if (sum_check_num[i] > 0) {
				NVMEV_DISPATCHER_LOG(
					"check_num [%s] SUM: %d th, avg_total_lat (us): %lld\n",
					opcode_str[i], sum_check_num[i],
					sum_necs_total[i] / sum_check_num[i] / 1000);

				if (sum_check_interval_num[i] > 0) {
					NVMEV_DISPATCHER_LOG(
						"check_num [%s] SUM: %d th, avg_interval_lat (us): %lld\n",
						opcode_str[i], sum_check_interval_num[i],
						sum_necs_interval[i] / sum_check_interval_num[i] /
							1000);
				}

				NVMEV_DISPATCHER_LOG(
					"check_num [%s] SUM: %d th, avg_diff_lat (us): %lld\n",
					opcode_str[i], sum_check_num[i],
					sum_necs_diff[i] / sum_check_num[i] / 1000);
			}

			if (check_num[i] > 0) {
				NVMEV_DISPATCHER_LOG(
					"check_num [%s]: %d, avg_total_lat (us): %lld\n",
					opcode_str[i], check_num[i],
					necs_total[i] / check_num[i] / 1000);

				if (check_interval_num[i] > 0) {
					NVMEV_DISPATCHER_LOG(
						"check_num [%s]: %d, avg_interval_lat (us): %lld\n",
						opcode_str[i], check_interval_num[i],
						necs_interval[i] / check_interval_num[i] / 1000);
				}

				NVMEV_DISPATCHER_LOG(
					"check_num [%s]: %d, avg_diff_lat (us): %lld\n",
					opcode_str[i], check_num[i],
					necs_diff[i] / check_num[i] / 1000);
			}

			check_num[i] = necs_total[i] = necs_diff[i] = necs_interval[i] =
				check_interval_num[i] = 0;
		}
	}
}

#endif

#ifdef LAT_BREAKDOWN
static DEFINE_PER_CPU(uint64_t, diff_time);
static DEFINE_PER_CPU(uint64_t, total_time);
static DEFINE_PER_CPU(uint64_t, total_cnt);
#endif

static bool __dequeue_io_req_from_ftl(int dispatcher_id, int FTL_id)
{
	NVMEV_ASSERT(nvmev_vdev->out_que_head[dispatcher_id]);
	NVMEV_ASSERT(nvmev_vdev->out_que[dispatcher_id]);
	int *head = &(nvmev_vdev->out_que_head[dispatcher_id][FTL_id]);
	struct nvmev_io_cmd *que = nvmev_vdev->out_que[dispatcher_id][FTL_id];
	struct nvmev_internal_request *req = NULL, *check_req = NULL;
	struct nvmev_submission_queue *sq = NULL;
	int sqid;
	__u16 command_id;
	int internal_req_id;
	bool found = false;

	NVMEV_ASSERT(que);

	// NVMEV_ERROR("__dequeue_io_req_from_ftl: %d %d %d\n", dispatcher_id, FTL_id, *head);

	while (atomic_read(&que[*head].is_completed)) {
		rmb();
		// NVMEV_ERROR("__dequeue_io_req_from_ftl: %d %d %d\n", dispatcher_id, FTL_id, *head);
		sqid = que[*head].sqid;
		command_id = que[*head].command_id;

		NVMEV_ASSERT((sqid - 1) % nvmev_vdev->config.nr_dispatcher == dispatcher_id);

		NVMEV_ASSERT(nvmev_vdev->reqqs[sqid]);
		sq = nvmev_vdev->sqes[sqid];

		internal_req_id = sq->tag_mapper[command_id];
		NVMEV_ASSERT(internal_req_id != 0);

		req = &(nvmev_vdev->reqqs[sqid]->internal_req[internal_req_id]);

		NVMEV_ASSERT(req);

		req->remaining_req--;
		req->nsecs_end = max_t(unsigned long long, req->nsecs_end, que[*head].nsecs);

		// put the result into CQ
		if (req->remaining_req == 0) {
			copy_io_result(req, &(que[*head]));
			push_completed_req(&(nvmev_vdev->completed_list[dispatcher_id]), req);
#ifdef PERF_CHECK_STATS
			add_rev_stats(req);
#endif
		}

		mb();

		// reclaim the completed request
		atomic_set(&que[*head].is_completed, 0);
		(*head)++;
		if (unlikely(*head >= nvmev_vdev->config.out_que_size))
			*head = 0;
		// *head = (*head + 1) % nvmev_vdev->config.out_que_size;
	}

	uint64_t tick_now = __get_wallclock();

#ifdef LAT_BREAKDOWN
	uint64_t *diff_time_ptr = &get_cpu_var(diff_time);
	uint64_t *total_time_ptr = &get_cpu_var(total_time);
	uint64_t *total_cnt_ptr = &get_cpu_var(total_cnt);
#endif

	while ((check_req = pop_completed_req(&(nvmev_vdev->completed_list[dispatcher_id])))) {
		NVMEV_ASSERT(check_req);
		if (tick_now < check_req->nsecs_end)
			break;

#ifdef LAT_BREAKDOWN
		(*total_cnt_ptr)++;
		(*diff_time_ptr) += tick_now - check_req->nsecs_end;
		(*total_time_ptr) += tick_now - check_req->nsecs_start;
		if ((*total_cnt_ptr) % 10000000 == 0) {
			NVMEV_DISPATCHER_LOG("DISP %d, diff_cnt: %lld, avg_total_time: %lld, avg_diff_time: %lld\n", dispatcher_id, *total_cnt_ptr, *total_time_ptr / *total_cnt_ptr, *diff_time_ptr / *total_cnt_ptr);
			*total_cnt_ptr = *total_time_ptr = *diff_time_ptr = 0;
		}
#endif

		__fill_cq_result_for_FTL(&(check_req->io_cmd));
		mb();
		delete_completed_req(check_req);
		sqid = check_req->io_cmd.sqid;
		sq = nvmev_vdev->sqes[sqid];
		command_id = check_req->io_cmd.command_id;
		sq->tag_mapper[command_id] = 0;
		NVMEV_ASSERT(sq);
		list_add_tail(&(check_req->list), &(sq->free_req_list));
		check_req->is_flight = false;

#ifdef PERF_CHECK_STATS
		add_check_stats(check_req);
#endif

		found = true;
	}

#ifdef LAT_BREAKDOWN
	put_cpu_var(total_time);
	put_cpu_var(diff_time);
  put_cpu_var(total_cnt);
#endif

	return found;
}

bool dequeue_io_req_from_ftl(int dispatcher_id)
{
	int FTL_id;
	bool found = false;

	for (FTL_id = 0; FTL_id < nvmev_vdev->config.nr_io_workers; FTL_id++) {
		found |= __dequeue_io_req_from_ftl(dispatcher_id, FTL_id);
	}

	return found;
}

static void __fill_cq_result(struct nvmev_io_work *w)
{
	int sqid = w->sqid;
	int cqid = w->cqid;
	int sq_entry = w->sq_entry;
	unsigned int command_id = w->command_id;
	unsigned int status = w->status;
	unsigned int result0 = w->result0;
	unsigned int result1 = w->result1;

	struct nvmev_completion_queue *cq = nvmev_vdev->cqes[cqid];
	int cq_head = cq->cq_head;
	struct nvme_completion *cqe = &cq_entry(cq_head);

	spin_lock(&cq->entry_lock);
	cqe->command_id = command_id;
	cqe->sq_id = sqid;
	cqe->sq_head = sq_entry;
	cqe->status = cq->phase | (status << 1);
	cqe->result0 = result0;
	cqe->result1 = result1;

	if (++cq_head == cq->queue_size) {
		cq_head = 0;
		cq->phase = !cq->phase;
	}

	cq->cq_head = cq_head;
	cq->interrupt_ready = true;
	spin_unlock(&cq->entry_lock);
}

static int nvmev_io_worker(void *data)
{
	struct nvmev_io_worker *worker = (struct nvmev_io_worker *)data;
	struct nvmev_ns *ns;
	static unsigned long last_io_time = 0;

#ifdef PERF_DEBUG
	static unsigned long long intr_clock[NR_MAX_IO_QUEUE + 1];
	static unsigned long long intr_counter[NR_MAX_IO_QUEUE + 1];

	unsigned long long prev_clock;
#endif

	NVMEV_INFO("%s started on cpu %d (node %d)\n", worker->thread_name, smp_processor_id(),
		   cpu_to_node(smp_processor_id()));

	while (!kthread_should_stop()) {
		unsigned long long curr_nsecs_wall = __get_wallclock();
		unsigned long long curr_nsecs_local = local_clock();
		long long delta = curr_nsecs_wall - curr_nsecs_local;

		volatile unsigned int curr = worker->io_seq;
		int qidx;

		while (curr != -1) {
			struct nvmev_io_work *w = &worker->work_queue[curr];
			unsigned long long curr_nsecs = local_clock() + delta;
			worker->latest_nsecs = curr_nsecs;

			if (w->is_completed == true) {
				curr = w->next;
				continue;
			}

			if (w->is_copied == false) {
#ifdef PERF_DEBUG
				w->nsecs_copy_start = local_clock() + delta;
#endif
				if (w->is_internal) {
					;
				} else if (io_using_dma) {
					__do_perform_io_using_dma(w->sqid, w->sq_entry);
				} else {
#if (BASE_SSD == KV_PROTOTYPE)
					struct nvmev_submission_queue *sq =
						nvmev_vdev->sqes[w->sqid];
					ns = &nvmev_vdev->ns[0];
					if (ns->identify_io_cmd(ns, sq_entry(w->sq_entry))) {
						w->result0 = ns->perform_io_cmd(
							ns, &sq_entry(w->sq_entry), &(w->status));
					} else {
						__do_perform_io(w->sqid, w->sq_entry);
					}
#else
					__do_perform_io(w->sqid, w->sq_entry);
#endif
				}

#ifdef PERF_DEBUG
				w->nsecs_copy_done = local_clock() + delta;
#endif
				w->is_copied = true;
				last_io_time = jiffies;

				NVMEV_DEBUG_VERBOSE("%s: copied %u, %d %d %d\n",
						    worker->thread_name, curr, w->sqid, w->cqid,
						    w->sq_entry);
			}

			if (w->nsecs_target <= curr_nsecs) {
				if (w->is_internal) {
#if (SUPPORTED_SSD_TYPE(CONV) || SUPPORTED_SSD_TYPE(ZNS))
					buffer_release((struct buffer *)w->write_buffer,
						       w->buffs_to_release);
#endif
				} else {
					__fill_cq_result(w);
				}

				NVMEV_DEBUG_VERBOSE("%s: completed %u, %d %d %d\n",
						    worker->thread_name, curr, w->sqid, w->cqid,
						    w->sq_entry);

#ifdef PERF_DEBUG
				w->nsecs_cq_filled = local_clock() + delta;
				trace_printk("%llu %llu %llu %llu %llu %llu\n", w->nsecs_start,
					     w->nsecs_enqueue - w->nsecs_start,
					     w->nsecs_copy_start - w->nsecs_start,
					     w->nsecs_copy_done - w->nsecs_start,
					     w->nsecs_cq_filled - w->nsecs_start,
					     w->nsecs_target - w->nsecs_start);
#endif
				mb(); /* Reclaimer shall see after here */
				w->is_completed = true;
			}

			curr = w->next;
		}

		for (qidx = 1; qidx <= nvmev_vdev->nr_cq; qidx++) {
			struct nvmev_completion_queue *cq = nvmev_vdev->cqes[qidx];

#ifdef CONFIG_NVMEV_IO_WORKER_BY_SQ
			if ((worker->id) != __get_io_worker(qidx))
				continue;
#endif
			if (cq == NULL || !cq->irq_enabled)
				continue;

			if (mutex_trylock(&cq->irq_lock)) {
				if (cq->interrupt_ready == true) {
#ifdef PERF_DEBUG
					prev_clock = local_clock();
#endif
					cq->interrupt_ready = false;
					nvmev_signal_irq(cq->irq_vector);

#ifdef PERF_DEBUG
					intr_clock[qidx] += (local_clock() - prev_clock);
					intr_counter[qidx]++;

					if (intr_counter[qidx] > 100) {
						NVMEV_DISPATCHER_LOG(
							"Intr %d: %llu HZ: %d\n", qidx,
							intr_clock[qidx] / intr_counter[qidx], HZ);
						intr_clock[qidx] = 0;
						intr_counter[qidx] = 0;
					}
#endif
				}
				mutex_unlock(&cq->irq_lock);
			}
		}
		if (CONFIG_NVMEVIRT_IDLE_TIMEOUT != 0 &&
		    time_after(jiffies, last_io_time + (CONFIG_NVMEVIRT_IDLE_TIMEOUT * HZ)))
			schedule_timeout_interruptible(1);
		else
			cond_resched();
	}

	return 0;
}

void NVMEV_IO_WORKER_INIT(struct nvmev_dev *nvmev_vdev)
{
	unsigned int i, worker_id;

	nvmev_vdev->io_workers = kcalloc(sizeof(struct nvmev_io_worker),
					 nvmev_vdev->config.nr_io_workers, GFP_KERNEL);
	nvmev_vdev->io_worker_turn = 0;

	for (worker_id = 0; worker_id < nvmev_vdev->config.nr_io_workers; worker_id++) {
		struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[worker_id];

		worker->work_queue =
			kzalloc(sizeof(struct nvmev_io_work) * NR_MAX_PARALLEL_IO, GFP_KERNEL);
		for (i = 0; i < NR_MAX_PARALLEL_IO; i++) {
			worker->work_queue[i].next = i + 1;
			worker->work_queue[i].prev = i - 1;
		}
		worker->work_queue[NR_MAX_PARALLEL_IO - 1].next = -1;
		worker->id = worker_id;
		worker->free_seq = 0;
		worker->free_seq_end = NR_MAX_PARALLEL_IO - 1;
		worker->io_seq = -1;
		worker->io_seq_end = -1;

		snprintf(worker->thread_name, sizeof(worker->thread_name), "nvmev_io_worker_%d",
			 worker_id);

		worker->task_struct =
			kthread_create(nvmev_io_worker, worker, "%s", worker->thread_name);

		kthread_bind(worker->task_struct, nvmev_vdev->config.cpu_nr_io_workers[worker_id]);
		wake_up_process(worker->task_struct);
	}
}

void NVMEV_IO_WORKER_FINAL(struct nvmev_dev *nvmev_vdev)
{
	unsigned int i;

	for (i = 0; i < nvmev_vdev->config.nr_io_workers; i++) {
		struct nvmev_io_worker *worker = &nvmev_vdev->io_workers[i];

		if (!IS_ERR_OR_NULL(worker->task_struct)) {
			kthread_stop(worker->task_struct);
		}

		kfree(worker->work_queue);
	}

	kfree(nvmev_vdev->io_workers);
}
