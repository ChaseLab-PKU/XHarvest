#include <assert.h>
#include <atomic>
#include <cstring>
#include <cstdlib>
#include <cstdio>


#include "Enclave_t.h"
#include "sgx_trts.h"
#include "sgx_thread.h"
#include "sgx_tseal.h"
#include "sgx_ftl.h"

// #define FEMU_DEBUG_FTL
// #define SGX_PRINT_PERIOD 2000000
// #define SGX_PROFILE

// #define CPU_BREAKDOWN

// #define PRINT_PERIOD 50000
// 200ms

static inline bool should_gc(struct ssd *ssd, int FTLidx) {
	return (ssd->lm[FTLidx].free_line_cnt <= ssd->sp.gc_thres_lines);
}

static inline bool should_gc_high(struct ssd *ssd, int FTLidx) {
	return (ssd->lm[FTLidx].free_line_cnt <= ssd->sp.gc_thres_lines_high);
}

static inline struct ppa get_maptbl_ent(struct ssd *ssd, uint64_t lpn) {
	return ssd->SGX_maptbl[lpn];
}

static inline void set_maptbl_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa) {
	ftl_assert(lpn < (uint64_t)ssd->sp.tt_pgs);
	ssd->SGX_maptbl[lpn] = *ppa;
}

static uint64_t ppa2pgidx(struct ssd *ssd, struct ppa *ppa) {
	struct ssdparams *spp = &ssd->sp;
	uint64_t pgidx;

	pgidx = ppa->g.ch * spp->pgs_per_ch +
					ppa->g.lun * spp->pgs_per_lun +
					ppa->g.pl * spp->pgs_per_pl +
					ppa->g.blk * spp->pgs_per_blk +
					ppa->g.pg;

	ftl_assert(pgidx < (uint64_t)spp->tt_pgs);

	return pgidx;
}

static inline uint64_t get_rmap_ent(struct ssd *ssd, struct ppa *ppa) {
	uint64_t pgidx = ppa2pgidx(ssd, ppa);

	return ssd->SGX_rmap[pgidx];
}

/* set rmap[page_no(ppa)] -> lpn */
static inline void set_rmap_ent(struct ssd *ssd, uint64_t lpn, struct ppa *ppa) {
	uint64_t pgidx = ppa2pgidx(ssd, ppa);

	ssd->SGX_rmap[pgidx] = lpn;
}

static void ssd_init_lines(struct ssd *ssd, int FTLidx) {
	struct ssdparams *spp = &ssd->sp;
	struct line_mgmt *lm = &ssd->lm[FTLidx];
	struct line *line;

	lm->tt_lines = spp->blks_per_pl;
	ftl_assert(lm->tt_lines == spp->tt_lines);
	lm->lines = (struct line *)malloc(sizeof(struct line) * lm->tt_lines);

	pthread_mutex_init(&lm->lm_mutex, NULL);

	QTAILQ_INIT(&lm->free_line_list);
	QTAILQ_INIT(&lm->victim_line_list);
	QTAILQ_INIT(&lm->full_line_list);

	lm->free_line_cnt = 0;
	for (int i = 0; i < lm->tt_lines; i++)
	{
		line = &lm->lines[i];
		line->id = i;
		line->ipc = 0;
		line->vpc = 0;
		/* initialize all the lines as free lines */
		QTAILQ_INSERT_TAIL(&lm->free_line_list, line, entry);
		lm->free_line_cnt++;
	}

	ftl_assert(lm->free_line_cnt == lm->tt_lines);
	lm->victim_line_cnt = 0;
	lm->full_line_cnt = 0;
}

static inline void check_addr(int a, int max) {
	ftl_assert(a >= 0 && a < max);
}

static void set_wp_ch_lun(struct ssd *ssd, struct write_pointer *wpp, int FTLidx) {
	struct ssdparams *spp = &ssd->sp;
	wpp->ch = ssd->next_lun[FTLidx][wpp->cur_lun] / spp->luns_per_ch;
	wpp->lun = ssd->next_lun[FTLidx][wpp->cur_lun] % spp->luns_per_ch;
	check_addr(wpp->ch, spp->nchs);
	check_addr(wpp->lun, spp->luns_per_ch);
}

static void ssd_init_write_pointer(struct ssd *ssd, int FTLidx) {
	struct write_pointer *wpp = &ssd->wp[FTLidx];
	struct line_mgmt *lm = &ssd->lm[FTLidx];
	struct line *curline = NULL;

	pthread_mutex_lock(&lm->lm_mutex);

	curline = QTAILQ_FIRST(&lm->free_line_list);
	QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
	lm->free_line_cnt--;
	curline->belong_to_th = FTLidx;

	pthread_mutex_unlock(&lm->lm_mutex);

	/* wpp->curline is always our next-to-write super-block */
	wpp->curline = curline;
	ftl_assert(wpp->cur_lun == 0);
	set_wp_ch_lun(ssd, wpp, FTLidx);
	wpp->pg = 0;
	wpp->blk = curline->id;
	wpp->pl = 0;
}

static struct line *get_next_free_line(struct ssd *ssd, int FTLidx) {
	struct line_mgmt *lm = &ssd->lm[FTLidx];
	struct line *curline = NULL;

	// pthread_mutex_lock(&lm->lm_mutex);
	curline = QTAILQ_FIRST(&lm->free_line_list);
	if (!curline) {
		pthread_mutex_unlock(&lm->lm_mutex);
		femu_err("no free line\n");
		assert(0);
		return NULL;
	}

	QTAILQ_REMOVE(&lm->free_line_list, curline, entry);
	lm->free_line_cnt--;
	curline->belong_to_th = FTLidx;
	// pthread_mutex_unlock(&lm->lm_mutex);
	return curline;
}

static void ssd_advance_write_pointer(struct ssd *ssd, int FTLidx) {
	struct ssdparams *spp = &ssd->sp;
	struct write_pointer *wpp = &ssd->wp[FTLidx];
	struct line_mgmt *lm = &ssd->lm[FTLidx];

	check_addr(wpp->cur_lun, ssd->lun_cnt[FTLidx]);
	wpp->cur_lun++;
	if (wpp->cur_lun != ssd->lun_cnt[FTLidx])
		set_wp_ch_lun(ssd, wpp, FTLidx);

	if (wpp->cur_lun == ssd->lun_cnt[FTLidx]) {
		wpp->cur_lun = 0;
		set_wp_ch_lun(ssd, wpp, FTLidx);

		/* go to next page in the block */
		check_addr(wpp->pg, spp->pgs_per_blk);
		wpp->pg++;
		if (wpp->pg == spp->pgs_per_blk) {
			wpp->pg = 0;
			/* move current line to {victim,full} line list */
			if (wpp->curline->vpc == spp->pgs_per_line_FTL[FTLidx]) {
				/* all pgs are still valid, move to full line list */
				ftl_assert(wpp->curline->ipc == 0);
				// pthread_mutex_lock(&lm->lm_mutex);
				QTAILQ_INSERT_TAIL(&lm->full_line_list, wpp->curline, entry);
				lm->full_line_cnt++;
				// pthread_mutex_unlock(&lm->lm_mutex);
			}
			else {
				ftl_assert(wpp->curline->vpc >= 0 && wpp->curline->vpc < spp->pgs_per_line_FTL[FTLidx]);
				/* there must be some invalid pages in this line */
				ftl_assert(wpp->curline->ipc > 0);

				// pthread_mutex_lock(&lm->lm_mutex);
				QTAILQ_INSERT_TAIL(&lm->victim_line_list, wpp->curline, entry);
				lm->victim_line_cnt++;
				// pthread_mutex_unlock(&lm->lm_mutex);
			}
			/* current line is used up, pick another empty line */
			check_addr(wpp->blk, spp->blks_per_pl);
			wpp->curline = NULL;
			wpp->curline = get_next_free_line(ssd, FTLidx);
			if (!wpp->curline)
			{
				/* TODO */
				return;
			}
			wpp->blk = wpp->curline->id;
			check_addr(wpp->blk, spp->blks_per_pl);
			ftl_assert(wpp->cur_lun == 0);
			set_wp_ch_lun(ssd, wpp, FTLidx);
			/* make sure we are starting from page 0 in the super block */
			ftl_assert(wpp->pg == 0);
			/* TODO: assume # of pl_per_lun is 1, fix later */
			ftl_assert(wpp->pl == 0);
		}
	}
}

static struct ppa get_new_page(struct ssd *ssd, int FTLidx) {
	struct write_pointer *wpp = &ssd->wp[FTLidx];
	struct ppa ppa;
	ppa._ppa = 0;
	ppa.g.ch = wpp->ch;
	ppa.g.lun = wpp->lun;
	ppa.g.pg = wpp->pg;
	ppa.g.blk = wpp->blk;
	ppa.g.pl = wpp->pl;
	ftl_assert(ppa.g.pl == 0);

	return ppa;
}

static void check_params(struct ssdparams *spp) {
	/*
	 * we are using a general write pointer increment method now, no need to
	 * force luns_per_ch and nchs to be power of 2
	 */

	// ftl_assert(is_power_of_2(spp->luns_per_ch));
	// ftl_assert(is_power_of_2(spp->nchs));
}

uint64_t DIV_ROUND_UP(uint64_t a, uint64_t b) {
	return (a + b - 1) / b;
}
/*
static void ssd_init_params(struct ssdparams *spp, uint64_t host_capacity) {
	assert(spp != NULL && "ssdparams is NULL");
	spp->secsz = 512;
	spp->secs_per_pg = 8;

	spp->op_ratio = 0.25;
	uint64_t estimited_capacity = uint64_t(host_capacity / (1 - spp->op_ratio)) / (spp->secsz * spp->secs_per_pg);
	
	spp->nchs = 8;
	spp->luns_per_ch = 4;
	spp->pls_per_lun = 1;
	spp->pgs_per_blk = 1024;

	spp->blks_per_pl = DIV_ROUND_UP(estimited_capacity, spp->pgs_per_blk * spp->pls_per_lun * spp->luns_per_ch * spp->nchs);

	spp->capacity = spp->pgs_per_blk * spp->blks_per_pl * spp->pls_per_lun * spp->luns_per_ch * spp->nchs;

	spp->pg_rd_lat = 10;
	spp->pg_wr_lat = 200;
	spp->blk_er_lat = 1000;
	spp->ch_xfer_lat = 0;

	// calculated values 
	spp->secs_per_blk = spp->secs_per_pg * spp->pgs_per_blk;
	spp->secs_per_pl = spp->secs_per_blk * spp->blks_per_pl;
	spp->secs_per_lun = spp->secs_per_pl * spp->pls_per_lun;
	spp->secs_per_ch = spp->secs_per_lun * spp->luns_per_ch;
	spp->tt_secs = spp->secs_per_ch * spp->nchs;

	spp->pgs_per_pl = spp->pgs_per_blk * spp->blks_per_pl;
	spp->pgs_per_lun = spp->pgs_per_pl * spp->pls_per_lun;
	spp->pgs_per_ch = spp->pgs_per_lun * spp->luns_per_ch;
	spp->tt_pgs = spp->pgs_per_ch * spp->nchs;

	spp->blks_per_lun = spp->blks_per_pl * spp->pls_per_lun;
	spp->blks_per_ch = spp->blks_per_lun * spp->luns_per_ch;
	spp->tt_blks = spp->blks_per_ch * spp->nchs;

	spp->pls_per_ch = spp->pls_per_lun * spp->luns_per_ch;
	spp->tt_pls = spp->pls_per_ch * spp->nchs;

	spp->tt_luns = spp->luns_per_ch * spp->nchs;

	// line is special, put it at the end 
	spp->blks_per_line = spp->tt_luns; // TODO: to fix under multiplanes 
	spp->pgs_per_line = spp->blks_per_line * spp->pgs_per_blk;
	spp->secs_per_line = spp->pgs_per_line * spp->secs_per_pg;
	spp->tt_lines = spp->blks_per_lun; // TODO: to fix under multiplanes 

	spp->gc_thres_pcent = 0.75;
	spp->gc_thres_lines = (int)((1 - spp->gc_thres_pcent) * spp->tt_lines);
	spp->gc_thres_pcent_high = 0.95;
	spp->gc_thres_lines_high = (int)((1 - spp->gc_thres_pcent_high) * spp->tt_lines);
	spp->enable_gc_delay = true;

	check_params(spp);
}

static void ssd_init_nand_page(struct nand_page *pg, struct ssdparams *spp) {
	pg->nsecs = spp->secs_per_pg;
	pg->sec = (nand_sec_status_t *)malloc(sizeof(nand_sec_status_t) * pg->nsecs);
	for (int i = 0; i < pg->nsecs; i++) {
		pg->sec[i] = SEC_FREE;
	}
	pg->status = PG_FREE;
}

static void ssd_init_nand_blk(struct nand_block *blk, struct ssdparams *spp) {
	blk->npgs = spp->pgs_per_blk;
	blk->pg = (nand_page *)malloc(sizeof(struct nand_page) * blk->npgs);
	for (int i = 0; i < blk->npgs; i++) {
		ssd_init_nand_page(&blk->pg[i], spp);
	}
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt = 0;
	blk->wp = 0;
}

static void ssd_init_nand_plane(struct nand_plane *pl, struct ssdparams *spp) {
	pl->nblks = spp->blks_per_pl;
	pl->blk = (nand_block *)malloc(sizeof(struct nand_block) * pl->nblks);
	for (int i = 0; i < pl->nblks; i++) {
		ssd_init_nand_blk(&pl->blk[i], spp);
	}
}

static void ssd_init_nand_lun(struct nand_lun *lun, struct ssdparams *spp) {
	lun->npls = spp->pls_per_lun;
	lun->pl = (nand_plane *)malloc(sizeof(struct nand_plane) * lun->npls);
	for (int i = 0; i < lun->npls; i++) {
		ssd_init_nand_plane(&lun->pl[i], spp);
	}
	// lun->next_lun_avail_time = std::chrono::high_resolution_clock::time_point();
	lun->next_lun_avail_time = 0;
	lun->busy = false;
}

static void ssd_init_ch(struct ssd_channel *ch, struct ssdparams *spp) {
	ch->nluns = spp->luns_per_ch;
	ch->lun = (nand_lun *)malloc(sizeof(struct nand_lun) * ch->nluns);
	for (int i = 0; i < ch->nluns; i++) {
		ssd_init_nand_lun(&ch->lun[i], spp);
	}
	ch->next_ch_avail_time = 0;
	ch->busy = 0;
}

static void ssd_init_maptbl(struct ssd *ssd) {
	struct ssdparams *spp = &ssd->sp;

	ssd->maptbl = (ppa *)malloc(sizeof(struct ppa) * spp->tt_pgs);
	for (int i = 0; i < spp->tt_pgs; i++) {
		ssd->maptbl[i]._ppa = UNMAPPED_PPA;
	}
}

static void ssd_init_rmap(struct ssd *ssd) {
	struct ssdparams *spp = &ssd->sp;

	ssd->rmap = (uint64_t *)malloc(sizeof(uint64_t) * spp->tt_pgs);
	for (int i = 0; i < spp->tt_pgs; i++) {
		ssd->rmap[i] = INVALID_LPN;
	}
}
*/

static inline bool valid_ppa(struct ssd *ssd, struct ppa *ppa) {
	struct ssdparams *spp = &ssd->sp;
	int ch = ppa->g.ch;
	int lun = ppa->g.lun;
	int pl = ppa->g.pl;
	int blk = ppa->g.blk;
	int pg = ppa->g.pg;
	int sec = ppa->g.sec;

	if (ch >= 0 && ch < spp->nchs && lun >= 0 && lun < spp->luns_per_ch && pl >= 0 && pl < spp->pls_per_lun && blk >= 0 && blk < spp->blks_per_pl && pg >= 0 && pg < spp->pgs_per_blk && sec >= 0 && sec < spp->secs_per_pg)
		return true;

	return false;
}

static inline bool valid_lpn(struct ssd *ssd, uint64_t lpn) {
	return (lpn < (uint64_t)ssd->sp.tt_pgs);
}

static inline bool mapped_ppa(struct ppa *ppa) {
	return !(ppa->_ppa == UNMAPPED_PPA);
}

static inline struct ssd_channel *get_ch(struct ssd *ssd, struct ppa *ppa) {
	return &(ssd->ch[ppa->g.ch]);
}

static inline struct nand_lun *get_lun(struct ssd *ssd, struct ppa *ppa) {
	struct ssd_channel *ch = get_ch(ssd, ppa);
	return &(ch->lun[ppa->g.lun]);
}

static inline struct nand_plane *get_pl(struct ssd *ssd, struct ppa *ppa) {
	struct nand_lun *lun = get_lun(ssd, ppa);
	return &(lun->pl[ppa->g.pl]);
}

static inline struct nand_block *get_blk(struct ssd *ssd, struct ppa *ppa) {
	struct nand_plane *pl = get_pl(ssd, ppa);
	return &(pl->blk[ppa->g.blk]);
}

static inline struct line *get_line(struct ssd *ssd, struct ppa *ppa, int FTLidx) {
	return &(ssd->lm[FTLidx].lines[ppa->g.blk]);
}

static inline struct nand_page *get_pg(struct ssd *ssd, struct ppa *ppa) {
	struct nand_block *blk = get_blk(ssd, ppa);
	return &(blk->pg[ppa->g.pg]);
}

#ifdef FEMU_PERF
static void print_lat(uint64_t avg_lat, uint64_t max_lat) {
	char *buf = (char *)malloc(100);
	snprintf(buf, 100, "Write avg_lat %lu, max_lat %lu\n", avg_lat, max_lat);
	ocall_print_string(buf);
	free(buf);
}
#endif

// static const uint64_t CPU_frequency = (uint64_t)4000000000; // 4 GHz
static const uint64_t CPU_frequency = (uint64_t)2800000000; // 2.8 GHz


static inline uint64_t get_tick(void)
{
    uint32_t hi, lo;
    __asm__ __volatile__("" : : : "memory");
    __asm__ __volatile__("rdtsc"
                         : "=a"(lo), "=d"(hi));
   __asm__ __volatile__("" : : : "memory");

    return (uint64_t(hi) << 32) | uint64_t(lo);
}

static void SGX_init_lun(struct ssd *ssd) {
	ssd->SGX_lun = new std::list<time_slot>[ssd->sp.tt_luns];
	auto max_time_point = (uint64_t)std::numeric_limits<uint64_t>::max();
	for (int i = 0; i < ssd->sp.tt_luns; i++) {
		ssd->SGX_lun[i].push_back(time_slot{0, max_time_point});
	}
}

static uint64_t advance_time_slot(std::list<time_slot> &slot, uint64_t cmd_stime, uint64_t op_lat, bool flush) {
	bool found = false;
	uint64_t lat;
	time_slot new_slot{cmd_stime, cmd_stime + op_lat};
	auto it = std::upper_bound(slot.begin(), slot.end(), new_slot);
	// assert(it != lun_slot.end() && "No free time slot found");
	if (it != slot.begin()) {
		auto prev = std::prev(it);
		if (prev->end_time >= new_slot.end_time) {
			found = true;
			lat = op_lat;
			if (prev->end_time != new_slot.end_time) {
				time_slot after_slot{new_slot.end_time, prev->end_time};
				slot.insert(it, after_slot);
			}
			prev->end_time = new_slot.start_time;
			if (prev->end_time == prev->start_time) {
				slot.erase(prev);
			}
		}
	}
	if (!found) {
		while (it != slot.end() && it->end_time - op_lat < it->start_time) it++;
	
		ftl_assert(it != slot.end() && "No free time slot found");
		ftl_assert(it->start_time > cmd_stime && "start_time check failed");
		lat = it->start_time + op_lat - cmd_stime;
		it->start_time += op_lat;
		if (it->end_time == it->start_time) {
			slot.erase(it);
		}
	}
	
	if (flush) {
		auto it = slot.begin();
		while (it != slot.end()) {
			if (it->end_time < cmd_stime) {
				it = slot.erase(it);
			} else {
				break;
			}
		}
	}
	return lat;
}

uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct nand_cmd *ncmd, int FTLidx, bool from_IO) {
	int c = ncmd->cmd;
	uint64_t cmd_stime = ncmd->stime, nand_stime;
	struct ssdparams *spp = &ssd->sp;
	struct nand_lun *lun = get_lun(ssd, ppa);
	uint64_t op_lat = 0, nand_free_time;
  switch (c) { 
    case NAND_READ:
      op_lat = spp->pg_rd_lat;
      break;
    case NAND_WRITE:
      op_lat = spp->pg_wr_lat;
      break;
    case NAND_ERASE:
      op_lat = spp->blk_er_lat;
      break;
    default:
      assert(false);
  }
	uint64_t tgt_lun = ppa->g.ch * spp->luns_per_ch + ppa->g.lun;
	std::list<time_slot> &slot = ssd->SGX_lun[tgt_lun];
	uint64_t lat = advance_time_slot(slot, cmd_stime, op_lat, from_IO);
	return lat;
}

#if 0
static uint64_t ssd_advance_status(struct ssd *ssd, struct ppa *ppa, struct nand_cmd *ncmd, int FTLidx) {
	int c = ncmd->cmd;
	uint64_t cmd_stime = ncmd->stime, nand_stime;
	struct ssdparams *spp = &ssd->sp;
	struct nand_lun *lun = get_lun(ssd, ppa);

	uint64_t op_lat;
  uint64_t nand_free_time;
  switch (c) { 
    case NAND_READ:
      op_lat = spp->pg_rd_lat;
      break;
    case NAND_WRITE:
      op_lat = spp->pg_wr_lat;
      break;
    case NAND_ERASE:
      op_lat = spp->blk_er_lat;
      break;
    default:
      assert(false);
  }

	uint64_t lat, expected, desired;

	// pthread_mutex_lock(&lun->lun_mutex);

	static int conflict_cnt = 0, req_cnt = 0;
	static unsigned long long waste_time = 0, total_time = 0;
	req_cnt++;

	do {
		expected = lun->next_lun_avail_time.load(std::memory_order_relaxed);
		desired = (expected <= cmd_stime) ? cmd_stime + op_lat : expected + op_lat;
		if (cmd_stime < expected) {
			waste_time = expected - cmd_stime;
		} else {
			waste_time = 0;
		}
		conflict_cnt++;
	} while (!lun->next_lun_avail_time.compare_exchange_weak(expected, desired, std::memory_order_release, std::memory_order_relaxed));
	
	total_time += waste_time;

	// nand_stime = (lun->next_lun_avail_time <= cmd_stime) ? cmd_stime : lun->next_lun_avail_time;
	// lun->next_lun_avail_time = nand_stime + op_lat;

	/*
	if (req_cnt % 7000 == 0) {
		char *buf = (char *)malloc(100);
		snprintf(buf, 100, "SGX req_cnt: %d, conflict_cnt: %d, total_waste_time: %llu\n", req_cnt, conflict_cnt, total_time);
		ocall_print_string(buf);
		free(buf);
	}
	*/

	lat = desired - cmd_stime;
	// pthread_mutex_unlock(&lun->lun_mutex);

#ifdef FEMU_PERF
	static uint64_t write_avg_lat = 0, write_max_lat = 0, cnt = 0;
	if (c == NAND_WRITE) {
		write_avg_lat += lat;
		if (lat > write_max_lat) {
			write_max_lat = lat;
		}
		cnt++;
	}
	if (cnt % 32768 == 0) {
		print_lat(write_avg_lat / cnt, write_max_lat);
		// cnt = write_avg_lat = write_max_lat = 0;
	}
#endif

	return lat;
}
#endif


static void insert_after_node(cpu_ts_list &slot, cpu_ts *node, cpu_ts *new_slot) {
	new_slot->next = new_slot->prev = NULL;
	if (node == slot.tail) {
		new_slot->prev = slot.tail;
		slot.tail->next = new_slot;

		slot.tail = new_slot;
	} else {
		cpu_ts *next = node->next;

		next->prev = new_slot;
		new_slot->next = next;

		new_slot->prev = node;
		node->next = new_slot;
	}
}

static void delete_node(cpu_ts_list &slot, cpu_ts *node) {
	if (node == slot.head && node == slot.tail) {
		slot.head = slot.tail = NULL;
	} else if (node == slot.head) {
		slot.head = node->next;
		if (slot.head) {
			slot.head->prev = NULL;
		}
	} else if (node == slot.tail) {
		slot.tail = node->prev;
		if (slot.tail) {
			slot.tail->next = NULL;
		}
	} else {
		cpu_ts *prev = node->prev;
		cpu_ts *next = node->next;

		prev->next = next;
		next->prev = prev;
	}
	delete node;
}

static uint64_t advance_cpu_ts(cpu_ts_list &slot, uint64_t cmd_stime, uint64_t op_lat) {
	uint64_t cmd_etime = cmd_stime + op_lat;
	cpu_ts *current = slot.head;
	ftl_assert(current);

	bool found = false;
	uint64_t lat;
	pthread_mutex_lock(&slot.core_mtx);
	while (current) {
		if (current->start_time <= cmd_stime) {
			if (current->end_time >= cmd_etime) {
				found = true;
				lat = op_lat;
				if (current->end_time != cmd_etime) {
					cpu_ts *after_slot = new cpu_ts;
					after_slot->start_time = cmd_etime;
					after_slot->end_time = current->end_time;
					
					insert_after_node(slot, current, after_slot);
				}
				current->end_time = cmd_stime;
				if (current->end_time == current->start_time) {
					delete_node(slot, current);
				}
				break;
			}
		} else {
			if (current->end_time - current->start_time >= op_lat) {
				found = true;
				lat = current->start_time + op_lat - cmd_stime;
				current->start_time = cmd_etime;
				if (current->end_time == current->start_time) {
					delete_node(slot, current);
				}
				break;
			}
		}
		current = current->next;
	}
	pthread_mutex_unlock(&slot.core_mtx);
	ftl_assert(found && "No free time slot found");
	return lat;
}

static void flush_core_status(struct ssd *ssd, uint64_t cmd_stime) {
	for (int cur_core = 0; cur_core < ssd->sp.n_encrp_cores; cur_core++) {
		auto &core = ssd->encrp_cores[cur_core];
		auto &slot = core.free_ts;
		pthread_mutex_lock(&slot.core_mtx);
		struct cpu_ts *it = slot.head, *now;
		while (it) {
			if (it->end_time < cmd_stime) {
				now = it;
				it = it->next;
				delete_node(slot, now);
			} else {
				break;
			}
		}
		pthread_mutex_unlock(&slot.core_mtx);
	}

	for (int cur_core = 0; cur_core < ssd->sp.n_cpu_cores; cur_core++) {
		auto &core = ssd->cores[cur_core];
		auto &slot = core.free_ts;
		pthread_mutex_lock(&slot.core_mtx);
		struct cpu_ts *it = slot.head, *now;
		while (it) {
			if (it->end_time < cmd_stime) {
				now = it;
				it = it->next;
				delete_node(slot, now);
			} else {
				break;
			}
		}
		pthread_mutex_unlock(&slot.core_mtx);
	}
}

static uint64_t advance_encrp_core_status(struct ssd *ssd, uint64_t start_time) {
	struct ssdparams *spp = &ssd->sp;
	uint64_t op_lat = spp->cpu_cache_op_lat;
	int &cur_core = ssd->cur_SGX_encrp_core;
	struct ssd_embeded_core *core = &ssd->encrp_cores[cur_core];
	uint64_t lat = advance_cpu_ts(core->free_ts, start_time, op_lat);
	cur_core = (cur_core + 1) % spp->n_encrp_cores;
	return lat;
}

/* update SSD status about one page from PG_VALID -> PG_VALID */
static void mark_page_invalid(struct ssd *ssd, struct ppa *ppa, int FTLidx) {
	struct line_mgmt *lm = &ssd->lm[FTLidx];
	struct ssdparams *spp = &ssd->sp;
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	bool was_full_line = false;
	struct line *line;

	/* update corresponding page status */
	pg = get_pg(ssd, ppa);
	ftl_assert(pg->status == PG_VALID);
	pg->status = PG_INVALID;

	/* update corresponding block status */
	blk = get_blk(ssd, ppa);
	ftl_assert(blk->ipc >= 0 && blk->ipc < spp->pgs_per_blk);
	blk->ipc++;
	ftl_assert(blk->vpc > 0 && blk->vpc <= spp->pgs_per_blk);
	blk->vpc--;

	/* update corresponding line status */
	line = get_line(ssd, ppa, FTLidx);
	ftl_assert(line->ipc >= 0 && line->ipc < spp->pgs_per_line_FTL[FTLidx]);
	if (line->vpc == spp->pgs_per_line_FTL[FTLidx]) {
		ftl_assert(line->ipc == 0);
		was_full_line = true;
	}
	line->ipc++;
	ftl_assert(line->vpc > 0 && line->vpc <= spp->pgs_per_line_FTL[FTLidx]);
	/* Adjust the position of the victime line in the pq under over-writes */
	line->vpc--;

	if (was_full_line) {
		/* move line: "full" -> "victim" */
		pthread_mutex_lock(&lm->lm_mutex);
		QTAILQ_REMOVE(&lm->full_line_list, line, entry);
		lm->full_line_cnt--;
		QTAILQ_INSERT_TAIL(&lm->victim_line_list, line, entry);
		lm->victim_line_cnt++;
		pthread_mutex_unlock(&lm->lm_mutex);
	}
}

static void mark_page_valid(struct ssd *ssd, struct ppa *ppa, int FTLidx) {
	struct nand_block *blk = NULL;
	struct nand_page *pg = NULL;
	struct line *_line;

	/* update page status */
	pg = get_pg(ssd, ppa);
	ftl_assert(pg->status == PG_FREE);
	pg->status = PG_VALID;

	/* update corresponding block status */
	blk = get_blk(ssd, ppa);
	ftl_assert(blk->vpc >= 0 && blk->vpc < ssd->sp.pgs_per_blk);
	blk->vpc++;

	/* update corresponding line status */
	_line = get_line(ssd, ppa, FTLidx);
	ftl_assert(_line->vpc >= 0 && _line->vpc < ssd->sp.pgs_per_line_FTL[FTLidx]);
	_line->vpc++;
}

static void mark_block_free(struct ssd *ssd, struct ppa *ppa) {
	struct ssdparams *spp = &ssd->sp;
	struct nand_block *blk = get_blk(ssd, ppa);
	struct nand_page *pg = NULL;

	for (int i = 0; i < spp->pgs_per_blk; i++) {
		/* reset page status */
		pg = &blk->pg[i];
		// ftl_assert(pg->nsecs == spp->secs_per_pg);
		pg->status = PG_FREE;
	}

	/* reset block status */
	ftl_assert(blk->npgs == spp->pgs_per_blk);
	blk->ipc = 0;
	blk->vpc = 0;
	blk->erase_cnt++;
}

static void gc_read_page(struct ssd *ssd, struct ppa *ppa, int FTLidx) {
	/* advance ssd status, we don't care about how long it takes */
	if (ssd->sp.enable_gc_delay) {
		struct nand_cmd gcr;
		gcr.type = GC_IO;
		gcr.cmd = NAND_READ;
		gcr.stime = 0;
		ssd_advance_status(ssd, ppa, &gcr, FTLidx, false);
	}
}

/* move valid page data (already in DRAM) from victim line to a new page */
static uint64_t gc_write_page(struct ssd *ssd, struct ppa *old_ppa, int FTLidx) {
	struct ppa new_ppa;
	struct nand_lun *new_lun;
	uint64_t lpn = get_rmap_ent(ssd, old_ppa);

	ftl_assert(valid_lpn(ssd, lpn));
	new_ppa = get_new_page(ssd, FTLidx);
	/* update maptbl */
	set_maptbl_ent(ssd, lpn, &new_ppa);
	/* update rmap */
	set_rmap_ent(ssd, lpn, &new_ppa);

	mark_page_valid(ssd, &new_ppa, FTLidx);

	/* need to advance the write pointer here */
	ssd_advance_write_pointer(ssd, FTLidx);

	if (ssd->sp.enable_gc_delay) {
		struct nand_cmd gcw;
		gcw.type = GC_IO;
		gcw.cmd = NAND_WRITE;
		gcw.stime = 0;
		ssd_advance_status(ssd, &new_ppa, &gcw, FTLidx, false);
	}

	/* advance per-ch gc_endtime as well */
#if 0
    new_ch = get_ch(ssd, &new_ppa);
    new_ch->gc_endtime = new_ch->next_ch_avail_time;
#endif

	new_lun = get_lun(ssd, &new_ppa);
	// new_lun->gc_endtime = new_lun->next_lun_avail_time;

	return 0;
}

static struct line *select_victim_line(struct ssd *ssd, bool force, int FTLidx) {
	struct line_mgmt *lm = &ssd->lm[FTLidx];
	struct line *current_line = NULL, *victim_line = NULL;
	int min_vpc = 0x7fffffff;

	// pthread_mutex_lock(&lm->lm_mutex);
	QTAILQ_FOREACH(current_line, &lm->victim_line_list, entry) {
		if (current_line->vpc < min_vpc) {
			min_vpc = current_line->vpc;
			victim_line = current_line;
		}
	}
	if (!victim_line) {
		// pthread_mutex_unlock(&lm->lm_mutex);
		return NULL;
	}

	if (!force && victim_line->ipc < ssd->sp.pgs_per_line_FTL[FTLidx] / 8) {
		// pthread_mutex_unlock(&lm->lm_mutex);
		return NULL;
	}

	lm->victim_line_cnt--;
	QTAILQ_REMOVE(&lm->victim_line_list, victim_line, entry);

	// pthread_mutex_unlock(&lm->lm_mutex);

	/* victim_line is a danggling node now */
	return victim_line;
}

/* here ppa identifies the block we want to clean */
static void clean_one_block(struct ssd *ssd, struct ppa *ppa, int FTLidx) {
	struct ssdparams *spp = &ssd->sp;
	struct nand_page *pg_iter = NULL;
	int cnt = 0;

	for (int pg = 0; pg < spp->pgs_per_blk; pg++) {
		ppa->g.pg = pg;
		pg_iter = get_pg(ssd, ppa);
		/* there shouldn't be any free page in victim blocks */
		ftl_assert(pg_iter->status != PG_FREE);
		if (pg_iter->status == PG_VALID) {
			gc_read_page(ssd, ppa, FTLidx);
			/* delay the maptbl update until "write" happens */
			gc_write_page(ssd, ppa, FTLidx);
			cnt++;
		}
	}

	ftl_assert(get_blk(ssd, ppa)->vpc == cnt);
}

static void mark_line_free(struct ssd *ssd, struct ppa *ppa, int FTLidx) {
	struct line_mgmt *lm = &ssd->lm[FTLidx];
	struct line *_line = get_line(ssd, ppa, FTLidx);
	_line->ipc = 0;
	_line->vpc = 0;
	/* move this line to free line list */
	pthread_mutex_lock(&lm->lm_mutex);
	QTAILQ_INSERT_TAIL(&lm->free_line_list, _line, entry);
	lm->free_line_cnt++;
	_line->belong_to_th = -1;
	pthread_mutex_unlock(&lm->lm_mutex);
}

static void set_FTL_page_lun(struct FemuCtrl *n, struct ssd *ssd, uint64_t target_FTL_page, struct ppa &ppa, int FTLidx) {
	struct ssdparams *spp = &ssd->sp;
	struct nvmev_config *config = &n->config;
	uint64_t FTL_split_units = config->FTL_split_units / 8 / 1024;
	ftl_assert(config->FTL_split_units % 8 == 0 && config->FTL_split_units % 1024 == 0);
	uint64_t seg_size = FTL_split_units * (1 + config->split_ratio);

	uint64_t ori_tgt_FTL_LPN = target_FTL_page / seg_size * config->split_ratio + (target_FTL_page % seg_size) - 1
	;
	uint64_t FTL_page_lun = ori_tgt_FTL_LPN % (ssd->lun_cnt[FTLidx]);
	ppa.g.ch = ssd->next_lun[FTLidx][FTL_page_lun] / (ssd->sp.luns_per_ch);
	ppa.g.lun = ssd->next_lun[FTLidx][FTL_page_lun] % (ssd->sp.luns_per_ch);
}

static uint64_t ftl_page_read(struct FemuCtrl *n, struct ssd *ssd, uint64_t target_FTL_LPN, uint64_t start_time, int FTLidx) {
	struct ppa ppa;
	uint64_t lat = 0;

	set_FTL_page_lun(n, ssd, target_FTL_LPN, ppa, FTLidx);
	struct nand_cmd srd;
	srd.type = USER_IO;
	srd.cmd = NAND_READ;
	srd.stime = start_time;
	lat = ssd_advance_status(ssd, &ppa, &srd, FTLidx, false);
	return lat;
}

static uint64_t ftl_page_write(struct FemuCtrl *n, struct ssd *ssd, uint64_t target_FTL_LPN, uint64_t start_time, int FTLidx) {
	struct ppa ppa;
	uint64_t lat = 0;
	
	set_FTL_page_lun(n, ssd, target_FTL_LPN, ppa, FTLidx);
	
	struct nand_cmd srd;
	srd.type = USER_IO;
	srd.cmd = NAND_WRITE;
	srd.stime = start_time;
	lat = ssd_advance_status(ssd, &ppa, &srd, FTLidx, false);
	return lat;
}

static void pop_SGX_FTL_cache(struct FemuCtrl *n, struct ssd *ssd, uint64_t &end_time, bool warmup, int FTLidx, struct FTL_SGXcache_t &FTLcache) {
	std::list<uint64_t> &SGXlist = FTLcache.SGXlist;
	FTLcache_t &SGXcache = FTLcache.SGXcache;
	uint64_t victim_FTL_page = SGXlist.back();
	SGXlist.pop_back();
	
	ftl_assert(SGXcache.find(victim_FTL_page) != SGXcache.end());
	uint64_t SGXcache_slotID = SGXcache[victim_FTL_page].first;
	SGXcache.erase(victim_FTL_page);

	uint64_t &cnt = ssd->freeSGXID_cnt;
	ssd->freeSGXID[++cnt] = SGXcache_slotID;

	ftl_assert(SGXcache_slotID < ssd->sp.SGX_FTL_cache_size);
	if (end_time < ssd->SGXcache_avail_time[SGXcache_slotID]) {
		end_time = ssd->SGXcache_avail_time[SGXcache_slotID];
	}

	bool dirty = ssd->SGXcache_dirty[SGXcache_slotID];
	uint64_t ftl_flash_time = 0;

	if (dirty && !warmup) {
		// struct ppa t_ppa;
		// set_FTL_page_lun(ssd, victim_FTL_page, t_ppa);
		ftl_flash_time = ftl_page_write(n, ssd, victim_FTL_page, end_time, FTLidx);
		end_time += ftl_flash_time;
		// end_time += advance_encrp_core_status(ssd, end_time);
#ifdef SGX_PROFILE
		// =============== for stats ===============
		static uint64_t avg_pop_time = 0, pop_cnt = 0;
		avg_pop_time += ftl_flash_time;
		pop_cnt++;
		if (pop_cnt % SGX_PRINT_PERIOD == 0) {
			char *buf = (char *)malloc(100);
			snprintf(buf, 100, "SGX: %ldth, avg_pop_time: %lu\n", pop_cnt, avg_pop_time / pop_cnt);
			ocall_print_string(buf);
			free(buf);
		}
#endif
	}

	ssd->SGXcache_dirty[SGXcache_slotID] = false;
	ssd->SGXcache_avail_time[SGXcache_slotID] = end_time;
}

static void put_SGX_FTL_cache(struct FemuCtrl *n, struct ssd *ssd, uint64_t target_FTL_page, uint64_t &end_time, bool dirty, bool warmup, int FTLidx, struct FTL_SGXcache_t &FTLcache) {
	if (ssd->freeSGXID_cnt == 0) {
		// printf("Enter pop SGX FTL cache\n");
		pop_SGX_FTL_cache(n, ssd, end_time, warmup, FTLidx, FTLcache);
	}
	// printf("Enter put SGX FTL cache\n");
	ftl_assert(ssd->freeSGXID_cnt > 0);

	uint64_t &cnt = ssd->freeSGXID_cnt;
	uint64_t SGXcache_slotID = ssd->freeSGXID[cnt];
	cnt--;
	
	FTLcache.SGXlist.push_front(target_FTL_page);
	FTLcache.SGXcache[target_FTL_page] = std::make_pair(SGXcache_slotID, FTLcache.SGXlist.begin());
	
	ftl_assert(SGXcache_slotID < ssd->sp.SGX_FTL_cache_size);
	ssd->SGXcache_dirty[SGXcache_slotID] = dirty;

	if (end_time < ssd->SGXcache_avail_time[SGXcache_slotID]) {
#ifdef SGX_PROFILE
		// ================= for stats =================
		static uint64_t avg_wait_put_time = 0, put_cnt = 0;
		if (!warmup) {
			uint64_t wait_put_time = FTLcache.SGXcache_avail_time[SGXcache_slotID] - end_time;
			avg_wait_put_time += wait_put_time;
			put_cnt++;
			if (put_cnt % SGX_PRINT_PERIOD == 0) {
				char *buf = (char *)malloc(100);
				snprintf(buf, 100, "SGX: %ldth, avg_wait_put_time: %lu\n", put_cnt, avg_wait_put_time / put_cnt);
				ocall_print_string(buf);
				free(buf);
			}
		}
#endif
		end_time = ssd->SGXcache_avail_time[SGXcache_slotID];
	}
	
	// struct ppa t_ppa;
	// set_FTL_page_lun(ssd, target_FTL_page, t_ppa, FTLidx);

	uint64_t ftl_flash_time = 0;
	if (!warmup) {
		ftl_flash_time = ftl_page_read(n, ssd, target_FTL_page, end_time, FTLidx);
		end_time += ftl_flash_time;
		// end_time += advance_encrp_core_status(ssd, end_time);
#ifdef SGX_PROFILE
		// ================ for stats ================
		static uint64_t avg_FTL_flash_time = 0, FTL_flash_cnt = 0;
		avg_FTL_flash_time += ftl_flash_time;
		FTL_flash_cnt++;
		if (FTL_flash_cnt % SGX_PRINT_PERIOD == 0) {
			char *buf = (char *)malloc(100);
			snprintf(buf, 100, "SGX: %ldth, avg_fetch_time: %lu\n", FTL_flash_cnt, avg_FTL_flash_time / FTL_flash_cnt);
			ocall_print_string(buf);
			free(buf);
		}
#endif
	}

	ssd->SGXcache_avail_time[SGXcache_slotID] = end_time;
}

static bool hit_in_SGX_FTL_cache(struct ssd *ssd, uint64_t target_FTL_page, uint64_t &end_time, bool dirty, bool warmup, struct FTL_SGXcache_t &FTLcache) {
	std::list<uint64_t> &SGXlist = FTLcache.SGXlist;
	FTLcache_t &SGXcache = FTLcache.SGXcache;
	bool hit = (SGXcache.find(target_FTL_page) != SGXcache.end());

#ifdef SGX_PROFILE
	static uint64_t SGX_hit_cnt = 0, SGX_miss_cnt = 0;
	hit ? SGX_hit_cnt++ : SGX_miss_cnt++;
	if ((SGX_hit_cnt + SGX_miss_cnt) % SGX_PRINT_PERIOD == 0) {
		char *buf = (char *)malloc(100);
		snprintf(buf, 100, "SGX: %ldth, SGX hit ratio: %lf\n", SGX_hit_cnt + SGX_miss_cnt, (double)SGX_hit_cnt / (SGX_hit_cnt + SGX_miss_cnt));
		ocall_print_string(buf);
		free(buf);
		// SGX_hit_cnt = SGX_miss_cnt = 0;
	}
#endif

	if (hit) {
    SGXlist.erase(SGXcache[target_FTL_page].second);
    SGXlist.push_front(target_FTL_page);
		SGXcache[target_FTL_page].second = SGXlist.begin();
		uint64_t SGXcache_slotID = SGXcache[target_FTL_page].first;
		
		ftl_assert(SGXcache_slotID < ssd->sp.SGX_FTL_cache_size);
		ssd->SGXcache_dirty[SGXcache_slotID] |= dirty;

		if (end_time < ssd->SGXcache_avail_time[SGXcache_slotID]) {
#ifdef SGX_PROFILE
			// ================= for stats =================
			static uint64_t avg_wait_hit_time = 0, hit_cnt = 0;
			if (!warmup) {
				uint64_t wait_hit_time = FTLcache.SGXcache_avail_time[SGXcache_slotID] - end_time;
				avg_wait_hit_time += wait_hit_time;
				hit_cnt++;
				if (hit_cnt % SGX_PRINT_PERIOD == 0) {
					char *buf = (char *)malloc(100);
					snprintf(buf, 100, "SGX: %ldth, avg_wait_hit_time: %lu\n", hit_cnt, avg_wait_hit_time / hit_cnt);
					ocall_print_string(buf);
					free(buf);
				}
			}
#endif
			end_time = ssd->SGXcache_avail_time[SGXcache_slotID];
		}
  }
	return hit;
}

static uint64_t get_FTL_page(struct FemuCtrl *n, struct ssd *ssd, uint64_t target_LPN, uint64_t start_time, bool dirty, bool warmup, int FTLidx, struct FTL_SGXcache_t &FTLcache) {
	uint64_t end_time = start_time;
	uint64_t target_FTL_page = target_LPN / 1024;
	if (hit_in_SGX_FTL_cache(ssd, target_FTL_page, end_time, dirty, warmup, FTLcache)) {
		// printf("hit in SGX FTL cache\n");
	} else {
		put_SGX_FTL_cache(n, ssd, target_FTL_page, end_time, dirty, warmup, FTLidx, FTLcache);
	}
	return end_time - start_time;
}


uint64_t ssd_read(struct FemuCtrl *n, struct ssd *ssd, nvmev_io_cmd *req, nvmev_flight_io *flight_io, int FTLidx, struct FTL_SGXcache_t &FTLcache) {
	struct ssdparams *spp = &ssd->sp;
	struct ppa ppa;
	uint64_t start_lpn = req->slpa / 8;
	uint64_t end_lpn = (req->slpa + req->nlb - 1) / 8;
	uint64_t lpn;
	uint64_t sublat = 0, maxlat = 0;

	if (end_lpn >= (uint64_t)spp->tt_host_lpn) {
		femu_err("read invalid lpn\n");
	}

	/* normal IO read path */
	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		ppa = get_maptbl_ent(ssd, lpn);
		if (!mapped_ppa(&ppa) || !valid_ppa(ssd, &ppa)) {
			// printf("%s,lpn(%" PRId64 ") not mapped to valid ppa\n", ssd->ssdname, lpn);
			// printf("Invalid ppa,ch:%d,lun:%d,blk:%d,pl:%d,pg:%d,sec:%d\n",
			// ppa.g.ch, ppa.g.lun, ppa.g.blk, ppa.g.pl, ppa.g.pg, ppa.g.sec);
			continue;
		}

		uint64_t stime = flight_io->start_time;
		uint64_t FTL_lat = get_FTL_page(n, ssd, lpn, stime, false, false, FTLidx, FTLcache);
		stime += FTL_lat;

#ifdef SGX_PROFILE
		// ================= for stats =================
		static uint64_t avg_read_FTL_time = 0, read_FTL_cnt = 0;
		avg_read_FTL_time += FTL_lat;
		read_FTL_cnt++;
		if (read_FTL_cnt % SGX_PRINT_PERIOD == 0) {
			char *buf = (char *)malloc(100);
			snprintf(buf, 100, "SGX: %ldth, avg_read_FTL_time: %lu\n", read_FTL_cnt, avg_read_FTL_time / read_FTL_cnt);
			ocall_print_string(buf);
			free(buf);
		}
#endif


		struct nand_cmd srd;
		srd.type = USER_IO;
		srd.cmd = NAND_READ;
		srd.stime = stime;
		sublat = ssd_advance_status(ssd, &ppa, &srd, FTLidx, true);
		
#ifdef SGX_PROFILE
		// ================= for stats =================
		static uint64_t avg_read_flash_time = 0, read_flash_cnt = 0;
		avg_read_flash_time += sublat;
		read_flash_cnt++;
		if (read_flash_cnt % SGX_PRINT_PERIOD == 0) {
			char *buf = (char *)malloc(100);
			snprintf(buf, 100, "SGX: %ldth, avg_read_flash_time: %lu\n", read_flash_cnt, avg_read_flash_time / read_flash_cnt);
			ocall_print_string(buf);
			free(buf);
		}
#endif

		sublat += FTL_lat;
		maxlat = (sublat > maxlat) ? sublat : maxlat;
	}

	return maxlat;
}

static void __do_gc(struct ssd *ssd, uint64_t victim_line_id, int FTLidx) {
	struct ssdparams *spp = &ssd->sp;
	struct nand_lun *lunp;
	struct ppa ppa;
	int ch, lun;

	ppa.g.blk = victim_line_id;
	// femu_log("GC-ing line:%d\n", ppa.g.blk);

	/* copy back valid data */
	for (int i = 0; i < ssd->lun_cnt[FTLidx]; i++) {
		ppa.g.ch = ssd->next_lun[FTLidx][i] / spp->luns_per_ch;
		ppa.g.lun = ssd->next_lun[FTLidx][i] % spp->luns_per_ch;
		ppa.g.pl = 0;
		lunp = get_lun(ssd, &ppa);
		clean_one_block(ssd, &ppa, FTLidx);
		mark_block_free(ssd, &ppa);

		if (spp->enable_gc_delay) {
			struct nand_cmd gce;
			gce.type = GC_IO;
			gce.cmd = NAND_ERASE;
			gce.stime = 0;
			ssd_advance_status(ssd, &ppa, &gce, FTLidx, false);
		}

		// lunp->gc_endtime = lunp->next_lun_avail_time;
	}

	/* update line status */
	mark_line_free(ssd, &ppa, FTLidx);
}

static int do_gc(FemuCtrl *n, struct ssd *ssd, bool force, int FTLidx) {
	struct line *victim_line = NULL;
	
	victim_line = select_victim_line(ssd, force, FTLidx);
	if (!victim_line) {
		return -1;
	}

	static int gc_cnt = 0;
	gc_cnt++;

	if (gc_cnt % 1 == 0) {
		char *buf = (char *)malloc(100);
		snprintf(buf, 100, "GC count: %d\n", gc_cnt);
		// ocall_print_string(buf);
		free(buf);
	}

	ftl_assert(victim_line->belong_to_th == FTLidx);
	__do_gc(ssd, victim_line->id, FTLidx);

	// if (victim_line->belong_to_th != FTLidx) {
	//	assert(victim_line->belong_to_th >= 0 && victim_line->belong_to_th < n->config.ftl_thread_num);
	//	sent_gc_to_slave(n, victim_line->id, victim_line->belong_to_th);
	// } else {
	// __do_gc(ssd, victim_line->id, FTLidx);
	//}

	return 0;
}

uint64_t ssd_write(struct FemuCtrl *n, struct ssd *ssd, nvmev_io_cmd *req, nvmev_flight_io *flight_io, int FTLidx, struct FTL_SGXcache_t &FTLcache) {
	struct ssdparams *spp = &ssd->sp;
	uint64_t start_lpn = req->slpa / 8;
	uint64_t end_lpn = (req->slpa + req->nlb - 1) / 8;
	struct ppa ppa;
	uint64_t lpn;
	uint64_t curlat = 0, maxlat = 0;
	

	ftl_assert(start_lpn <= end_lpn);

	if (end_lpn >= (uint64_t)spp->tt_host_lpn) {
		femu_err("Write invalid lpn in SGX enclave, tt_host_lpn\n");
		assert(false);
	}

	/* TODO: check if we need to perform GC */
	
	int r;
	while (should_gc_high(ssd, FTLidx)) {
		// perform GC here until !should_gc(ssd)
		r = do_gc(n, ssd, true, FTLidx);
		if (r == -1)
			break;
	}	

	for (lpn = start_lpn; lpn <= end_lpn; lpn++) {
		ppa = get_maptbl_ent(ssd, lpn);
		if (mapped_ppa(&ppa)) {
			/* update old page information first */
			mark_page_invalid(ssd, &ppa, FTLidx);
			set_rmap_ent(ssd, INVALID_LPN, &ppa);
		}

		/* new write */
		ppa = get_new_page(ssd, FTLidx);
		/* update maptbl */
		set_maptbl_ent(ssd, lpn, &ppa);
		/* update rmap */
		set_rmap_ent(ssd, lpn, &ppa);

		mark_page_valid(ssd, &ppa, FTLidx);

		/* need to advance the write pointer here */
		ssd_advance_write_pointer(ssd, FTLidx);

		uint64_t stime = flight_io->start_time;
		uint64_t FTL_lat = get_FTL_page(n, ssd, lpn, stime, true, false, FTLidx, FTLcache);
		stime += FTL_lat;

		struct nand_cmd swr;
		swr.type = USER_IO;
		swr.cmd = NAND_WRITE;
		swr.stime = stime;
		/* get latency statistics */
		curlat = ssd_advance_status(ssd, &ppa, &swr, FTLidx, true);
		
#ifdef SGX_PROFILE
		// ================= for stats =================
		static uint64_t avg_write_flash_time = 0, write_flash_cnt = 0;
		avg_write_flash_time += curlat;
		write_flash_cnt++;
		if (write_flash_cnt % SGX_PRINT_PERIOD == 0) {
			char *buf = (char *)malloc(100);
			snprintf(buf, 100, "SGX: %ldth, avg_write_flash_time: %lu\n", write_flash_cnt, avg_write_flash_time / write_flash_cnt);
			ocall_print_string(buf);
			free(buf);
		}
#endif
		
		curlat += FTL_lat;
		maxlat = (curlat > maxlat) ? curlat : maxlat;
	}

	return maxlat;
}

static void ssd_warm_up(struct FemuCtrl *n, int FTLidx) {
	struct ssd *ssd = n->ssd;
	struct ssdparams *spp = &ssd->sp;
	struct nvmev_config *config = &n->config;
	int FTLnum = config->ftl_thread_num;
	int split_ratio = config->split_ratio;
	int single_unit = config->FTL_split_units / 8;
	int div_unit = (config->FTL_split_units / 8) * split_ratio;
	int seg_unit = (config->FTL_split_units / 8) * (1 + split_ratio);
	struct ppa ppa;

	assert(config->FTL_split_units % 8 == 0);

	uint64_t warm_up_ratio = 50;
	uint64_t warm_up_pages = spp->tt_host_lpn * warm_up_ratio / 100 * split_ratio / (1 + split_ratio);

	uint64_t lpn = 0;
	for (uint64_t i = 0; i < warm_up_pages; i++) {
		int bid = i / div_unit;
		int pid = i % div_unit;

		lpn = bid * seg_unit + single_unit + pid;

		assert((lpn * 8 / config->FTL_split_units) % (1 + split_ratio) > 0);

		ppa = get_maptbl_ent(ssd, lpn);
		if (mapped_ppa(&ppa)) {
			assert(0);
			return;
		}
		ppa = get_new_page(ssd, FTLidx);
		set_maptbl_ent(ssd, lpn, &ppa);
		set_rmap_ent(ssd, lpn, &ppa);
		mark_page_valid(ssd, &ppa, FTLidx);
		ssd_advance_write_pointer(ssd, FTLidx);
	}
}
/*
void ssd_init(FemuCtrl *n) {
	assert(0);
	struct ssd *ssd = n->ssd = (struct ssd *)calloc(1, sizeof(struct ssd));
	struct ssdparams *spp = &ssd->sp;

	ocall_print_string("Initializing SSD in SGX enclave\n");

	ftl_assert(ssd);

	ssd_init_params(spp, n->config.storage_size);

	ocall_print_string("Init params in SGX enclave\n");

	// char *buf = (char *)malloc(100);
	// snprintf(buf, 100, "nchs: %d\n", spp->nchs);
	// ocall_print_string(buf);

	// initialize ssd internal layout architecture
	ssd->ch = (ssd_channel *)malloc(sizeof(struct ssd_channel) * spp->nchs);
	for (int i = 0; i < spp->nchs; i++) {
		ssd_init_ch(&ssd->ch[i], spp);
	}

	ocall_print_string("Init init_ch in SGX enclave\n");

	// initialize maptbl 
	ssd_init_maptbl(ssd);

	ocall_print_string("Init maptbl in SGX enclave\n");

	// initialize rmap 
	ssd_init_rmap(ssd);

	// initialize all the lines
	ssd_init_lines(ssd);

	// initialize write pointer, this is how we allocate new pages for writes
	// ssd_init_write_pointer(ssd);

	ocall_print_string("Init write pointer in SGX enclave\n");

	ssd_warm_up(ssd);

	ocall_print_string("Warm up in SGX enclave\n");
}
*/

static void print_num_received(int num_reveived) {
	char *buf = (char *)malloc(100);
	snprintf(buf, 100, "SGX Received %d requests\n", num_reveived);
	ocall_print_string(buf);
	free(buf);
}

static void print_num_send(int num_send) {
	char *buf = (char *)malloc(100);
	snprintf(buf, 100, "SGX Sent %d requests\n", num_send);
	ocall_print_string(buf);
	free(buf);
}

void debug_break() {
    ocall_print_string("Debug break: num_reveived >= 32810");
    // 在这里设置一个断点，或者通过调试器手动中断
}

static void SGX_reset_stats(struct ssd *ssd) {
	struct ssd_stats &stats = ssd->SGX_stats;

	stats.culm_req_cnt += stats.req_cnt;
	stats.req_cnt = 0;
	stats.start_time = get_tick();

#ifdef LAT_BREAKDOWN
	for (int i = 0; i < LAT_BKDN_TYPE_COUNT; i++) {
		stats.par_lat[i] = 0;
	}
#endif

#ifdef CPU_BREAKDOWN
	for (int i = 0; i < CPU_BKDN_TYPE_COUNT; i++) {
		stats.CPU_util[i] = 0;
	}
#endif
}


static uint64_t calc_time_us(uint64_t end, uint64_t start) {
	uint64_t diff = end - start;
	uint64_t CPU_freq = CPU_frequency / 1000000;
	uint64_t trans_us = diff / (CPU_frequency / 1000000);
	return trans_us;
}

static void SGX_print_stats(struct ssd *ssd) {
	struct ssd_stats &stats = ssd->SGX_stats;

	ocall_print_string("\n\n===============================================\n");

	char buf[100];

	uint64_t cur_time = get_tick();
	uint64_t elapsed_tick = cur_time - stats.start_time;
	uint64_t elapsed_time = calc_time_us(cur_time, stats.start_time);

	snprintf(buf, 100, "SGX Request %luth - %luth (%lu)\n", stats.culm_req_cnt, stats.culm_req_cnt + stats.req_cnt, stats.req_cnt);
	ocall_print_string(buf);

	snprintf(buf, 100, "SGX elapsed time: %lu\n", elapsed_time);
	ocall_print_string(buf);
	
#ifdef LAT_BREAKDOWN
	double all_lat_percent = 0;
	for (int i = 1; i < BREAKDOWN_TYPE_COUNT; i++) {
		double par_percent = 1.0 * stats.par_lat[i] / stats.par_lat[1];
		all_percent += par_percent;
		snprintf(buf, 100, "%s %lf %lf\n", lat_bkdn_type_str[i], 1.0 * stats.par_lat[i] / 1000 / stats.req_cnt, par_percent);
		ocall_print_string(buf);
	}
	snprintf(buf, 100, "[LAT] All %lf\n", all_percent);
	ocall_print_string(buf);
#endif

#ifdef CPU_BREAKDOWN
	double all_cpu_percent = 0;
	for (int i = 1; i < CPU_BKDN_TYPE_COUNT; i++) {
		double par_percent = 1.0 * stats.CPU_util[i] / elapsed_tick;
		all_cpu_percent += par_percent;
		snprintf(buf, 100, "%s %lf %lf\n", cpu_bkdn_type_str[i], 1.0 * stats.CPU_util[i] / (CPU_frequency / 1000000), par_percent);
		ocall_print_string(buf);
	}
	snprintf(buf, 100, "[CPU] All %lf\n", all_cpu_percent);
	ocall_print_string(buf);
#endif

	ocall_print_string("===============================================\n\n");
}

static void check_req_from_dispatcher(FemuCtrl *n, int FTLidx, struct nvmev_flight_io *&flight_io_poll, struct nvmev_flight_io **&pq, struct FTL_SGXcache_t &FTLcache) {
	struct ssd *ssd = n->ssd;
	struct nvmev_io_cmd *req = NULL;
	struct nvmev_io_cmd *in_que = NULL;
	struct nvmev_flight_io *flight_io = NULL;
  uint64_t lat = 0;

	for (int i = 0; i < n->config.dispatcher_num; i++) {
		in_que = n->to_ftl[i][FTLidx];
		int &head = n->to_ftl_head[i][FTLidx];
		if (!in_que[head].is_completed.load(std::memory_order_acquire))
			continue;

		uint64_t comm_start = get_tick();

// #define SGX_PERF
#ifdef SGX_PERF    
		static int num_received = 0;
		num_received++;
		if (num_received % 100000 == 0) { // 32810
			print_num_received(num_received);
		}
#endif
			
		req = &in_que[head];

		if (flight_io_poll) {
			flight_io = flight_io_poll;
			flight_io_poll = flight_io_poll->next;
			flight_io->next = NULL;
		} else {
			femu_err("FTL thread %d: flight_io_poll is full\n", FTLidx);
			assert(0);
		}

		flight_io->poller_id = i;
		flight_io->command_id = req->command_id;
		flight_io->sqid = req->sqid;
		flight_io->cqid = req->cqid;
		flight_io->sq_entry = req->sq_entry;
		flight_io->start_time = req->nsecs;

		// femu_log("FTL thread %d: receive request from poller %d, command_id: %d, sqid: %d, cqid: %d, head: %d\n", index, i, req->command_id, req->sqid, req->cqid, head);

#ifdef CPU_BREAKDOWN
		ssd->SGX_stats.CPU_util[CPU_COMM] += get_tick() - comm_start;
#endif

		ssd->SGX_stats.req_cnt ++;


#ifdef PG_CC_BREAKDOWN
     ftl_start_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
     req->rev_ftl_time = ftl_start_time;
#endif

    ftl_assert(req);
		uint64_t ftl_start_time = get_tick();
    switch (req->opcode) {
    case NVME_CMD_WRITE:
    	lat = ssd_write(n, ssd, req, flight_io, FTLidx, FTLcache);
      // lat = 200;
			break;
    case NVME_CMD_READ:
      lat = ssd_read(n, ssd, req, flight_io, FTLidx, FTLcache);
      break;
    case NVME_CMD_DSM:
      lat = 0;
      break;
    default:
      femu_err("FTL received unkown request type, ERROR\n");
			assert(0);
			break;
    }

#ifdef CPU_BREAKDOWN
		ssd->SGX_stats.CPU_util[CPU_FTL] += get_tick() - ftl_start_time;
#endif

    flight_io->end_time = flight_io->start_time + lat;

		// std::atomic_thread_fence(std::memory_order_release); // maybe release order consistency
		// req->is_completed = false;
			
		// 同步点：在修改is_completed之前设置内存屏障
		std::atomic_thread_fence(std::memory_order_release);

		// 修改标志
		req->is_completed.store(false, std::memory_order_relaxed);
		head++;
		if (head >= n->config.in_que_size) {
			head = 0;
		}

		if (!pq[i]) {
			pq[i] = flight_io;
		} else {
			flight_io->next = pq[i];
			pq[i] = flight_io;
		}

		if (ssd->SGX_stats.req_cnt % 1000000 == 0) {
			// char buf[100];
			// snprintf(buf, 100, "SGX: %ldth, req_cnt: %ld, now tick: %lu, start_tick %lu, elapsed time %lu\n", FTLidx, ssd->SGX_stats.req_cnt, get_tick(), ssd->SGX_stats.start_time, calc_time_us(get_tick(), ssd->SGX_stats.start_time));
			// ocall_print_string(buf);
		}

#ifdef CPU_BREAKDOWN
		if (calc_time_us(get_tick(), ssd->SGX_stats.start_time) >= PRINT_PERIOD) {
			SGX_print_stats(ssd);
			SGX_reset_stats(ssd);
		}
#endif

#ifdef PG_CC_BREAKDOWN
    ftl_end_time = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    req->ftl_time = ftl_end_time - ftl_start_time;
    req->send_poller_time = ftl_end_time;
#endif

    /* clean one line if needed (in the background) */
		// TODO: check the GC
    if (should_gc(ssd, FTLidx)) {
			// femu_err("Enter GC\n");
      do_gc(n, ssd, false, FTLidx);
    }
  }
}

static void check_resp_to_dispatcher(FemuCtrl *n, int FTLidx, struct nvmev_flight_io * &flight_io_poll, struct nvmev_flight_io ** &pq) {
	struct nvmev_flight_io *flight_io = NULL;
	struct nvmev_io_cmd *out_que = NULL;

	for (int i = 0; i < n->config.dispatcher_num; i++) {
		while (pq[i]) {
			flight_io = pq[i];
			ftl_assert(flight_io);
			out_que = n->to_poller[i][FTLidx];
			int &tail = n->to_poller_tail[i][FTLidx];
				
			if (out_que[tail].is_completed.load(std::memory_order_acquire)) {
				continue;
			}
				
			uint64_t comm_start = get_tick();

			// pop from pq
			pq[i] = flight_io->next;
				
			flight_io->next = NULL;

#ifdef SGX_PROFILE
			static uint64_t last_req_time = 0, last_req_bw = 0;
			last_req_bw ++;
			if (!last_req_time) 
				last_req_time = flight_io->end_time;
			static uint64_t convert_ns2s = 1000000000, convert_b2mb = 1024 * 1024, convert_kiops = 1000;
			static const uint64_t PROFILE_PERIOD = 500 * 1000000; // 500 ms
			if (flight_io->end_time > last_req_time && flight_io->end_time - last_req_time > PROFILE_PERIOD) {
				char *buf = (char *)malloc(100);
				uint64_t during_time = flight_io->end_time - last_req_time;
				snprintf(buf, 100, "FTL thread %d: start from %ld, end at %ld, req KIOPS %lf\n", FTLidx, last_req_time, flight_io->end_time, 1.0 * last_req_bw * convert_ns2s / during_time / convert_kiops);
				last_req_time = flight_io->end_time;
				last_req_bw = 0;
				ocall_print_string(buf);
				free(buf);
			}
#endif

			out_que[tail].command_id = flight_io->command_id;
			out_que[tail].sqid = flight_io->sqid;
			out_que[tail].cqid = flight_io->cqid;
			out_que[tail].sq_entry = flight_io->sq_entry;
			out_que[tail].status = 0;
			out_que[tail].result0 = 0;
			out_que[tail].result1 = 0;
			out_que[tail].nsecs = flight_io->end_time;

			std::atomic_thread_fence(std::memory_order_release); // maybe release order consistency
				
			out_que[tail].is_completed.store(true, std::memory_order_relaxed);
			
#ifdef CPU_BREAKDOWN
			n->ssd->SGX_stats.CPU_util[CPU_COMM] += get_tick() - comm_start;
#endif

			/*
				num_send++;
				if (num_send % 100 == 0) {
					print_num_send(num_send);
				}
			*/

			if (flight_io_poll == NULL) {
				flight_io_poll = flight_io;
			} else {
				flight_io->next = flight_io_poll;
				flight_io_poll = flight_io;
			}

			tail++;
			if (tail >= n->config.out_que_size) {
				tail = 0;
			}
			// femu_log("FTL thread %d: send response to poller %d, command_id: %d, sqid: %d, new_tail %d, out_que_size %lu\n", index, i, flight_io->command_id, flight_io->sqid, tail, n->config.out_que_size);
		}
	}
}

static void check_req_from_master(FemuCtrl *n, int FTLidx) {
while (1) {
		int &head = n->to_slave_head[FTLidx];
		struct internal_cmd *req = &n->to_slave[FTLidx][head];
		if (!req->is_completed.load(std::memory_order_acquire))
			return;
		
		switch (req->opcode) {
		case INTER_CMD_GC:
			__do_gc(n->ssd, req->param, FTLidx);
			break;
		
		default:
			femu_err("FTL thread %d: received unknown request type, ERROR\n", FTLidx);
			assert(0);
			break;
		}

		std::atomic_thread_fence(std::memory_order_release); // maybe release order consistency

		req->is_completed.store(false, std::memory_order_relaxed);
		head++;
		if (head >= n->config.internal_msg_queue_size)
			head = 0;

		// ============ send response to master ============

		int &tail = n->to_master_tail[FTLidx];
		struct internal_cmd *resp = &n->to_master[FTLidx][tail];

		if (resp->is_completed.load(std::memory_order_acquire)) {
			femu_err("SM to_master queue %d is full\n", FTLidx);
			assert(0);
		}

		resp->opcode = INTER_CMD_GC;
		resp->param = req->param;
		resp->rsvd = 1;

		std::atomic_thread_fence(std::memory_order_release); // maybe release order consistency
		resp->is_completed.store(true, std::memory_order_relaxed);
		tail++;
		if (tail >= n->config.internal_msg_queue_size)
			tail = 0;
	}
}

static void ssd_init_SGX_maptbl(struct ssd *ssd) {
	struct ssdparams *spp = &ssd->sp;

	ssd->SGX_maptbl = (ppa *)malloc(sizeof(struct ppa) * spp->tt_pgs);
	assert(ssd->SGX_maptbl);
	for (int i = 0; i < spp->tt_pgs; i++) {
		ssd->SGX_maptbl[i]._ppa = UNMAPPED_PPA;
	}
}

static void ssd_init_SGX_rmap(struct ssd *ssd) {
	struct ssdparams *spp = &ssd->sp;

	ssd->SGX_rmap = (uint64_t *)malloc(sizeof(uint64_t) * spp->tt_pgs);
	assert(ssd->SGX_rmap);
	for (int i = 0; i < spp->tt_pgs; i++) {
		ssd->SGX_rmap[i] = INVALID_LPN;
	}
}

static void SGX_init_stats(struct ssd *ssd) {
	ssd->SGX_stats.req_cnt = 0;
	ssd->SGX_stats.culm_req_cnt = 0;

	ssd->SGX_stats.start_time = get_tick();

#ifdef LAT_BREAKDOWN
	for (int i = 0; i < LAT_BKDN_TYPE_COUNT; i++) {
		ssd->SGX_stats.par_lat[i] = 0;
	}
#endif

#ifdef CPU_BREAKDOWN
	for (int i = 0; i < CPU_BKDN_TYPE_COUNT; i++) {
		ssd->SGX_stats.CPU_util[i] = 0;
	}
#endif
}

void *ftl_thread(void *arg) {
  FemuCtrl *n = ((NvmeFTLThreadArgument *)arg)->n;
  int index = ((NvmeFTLThreadArgument *)arg)->index;
  struct ssd *ssd = n->ssd;
	struct FTL_SGXcache_t FTLcache;

  // ssd_warm_up(n, ssd);

	ssd_init_SGX_maptbl(ssd);

	ocall_print_string("SGX maptbl init\n");

	ssd_init_SGX_rmap(ssd);

	ocall_print_string("SGX remap init\n");

	ssd_init_lines(ssd, index);

	ssd_init_write_pointer(ssd, index);

	ssd_warm_up(n, index);

	SGX_init_lun(ssd);

	SGX_init_stats(ssd);

	const int MAX_FLIGHT_IO = 4096;

	char cbuf[100];
	snprintf(cbuf, 100, "ssd->flight_ios %p, ssd->flight_io_poll %p, ssd->pq %p\n", ssd->flight_ios, ssd->flight_io_poll, ssd->pq);
	ocall_print_string(cbuf);

	struct nvmev_flight_io *&flight_ios = ssd->flight_ios;
	struct nvmev_flight_io *&flight_io_poll = ssd->flight_io_poll;
	struct nvmev_flight_io **&pq = ssd->pq;

#ifdef PG_CC_BREAKDOWN
  int64_t ftl_start_time, ftl_end_time;
#endif

	femu_log("FTL thread %d in SGX enclave started\n", index);

  while (1) {
    check_req_from_dispatcher(n, index, flight_io_poll, pq, FTLcache);

		check_resp_to_dispatcher(n, index, flight_io_poll, pq);

		// check_req_from_master(n, index);
  }

	char *buf = (char *)malloc(100);
	snprintf(buf, 100, "FTL thread %d in SGX enclave exited\n", index);
	ocall_print_string(buf);
	free(buf);
	assert(0);

  return NULL;
}

/*
int nvme_register_bbssd(FemuCtrl *n)
{
	n->ext_ops = (FemuExtCtrlOps){
			.state = NULL,
			.init = ssd_init,
			.exit = NULL,
	};

	return 0;
}
*/