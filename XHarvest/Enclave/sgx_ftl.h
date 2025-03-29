#ifndef __SGX_FTL_H
#define __SGX_FTL_H


#include <chrono>

// #include "./sgx_pqueue.h"
#include "./sgx_queue.h"
#include "./sgx_qemu_queue.h"
#include "./sgx_nvme.h"
#include <unordered_map>
#include <list>

typedef __uint64_t uint64_t;

#define INVALID_PPA (~(0ULL))
#define INVALID_LPN (~(0ULL))
#define UNMAPPED_PPA (~(0ULL))

enum
{
	NAND_READ = 0,
	NAND_WRITE = 1,
	NAND_ERASE = 2,

	NAND_READ_LATENCY = 40000,
	NAND_PROG_LATENCY = 200000,
	NAND_ERASE_LATENCY = 2000000,
};

enum
{
	USER_IO = 0,
	GC_IO = 1,
};

enum
{
	SEC_FREE = 0,
	SEC_INVALID = 1,
	SEC_VALID = 2,

	PG_FREE = 0,
	PG_INVALID = 1,
	PG_VALID = 2
};

enum
{
	FEMU_ENABLE_GC_DELAY = 1,
	FEMU_DISABLE_GC_DELAY = 2,

	FEMU_ENABLE_DELAY_EMU = 3,
	FEMU_DISABLE_DELAY_EMU = 4,

	FEMU_RESET_ACCT = 5,
	FEMU_ENABLE_LOG = 6,
	FEMU_DISABLE_LOG = 7,
};

#define BLK_BITS (16)
#define PG_BITS (16)
#define SEC_BITS (8)
#define PL_BITS (8)
#define LUN_BITS (8)
#define CH_BITS (7)

/* describe a physical page addr */

typedef struct NvmeRequest
{
	uint64_t slba;
	uint16_t nlb;
	int64_t stime;
	int64_t expire_time;
} NvmeRequest;

struct ppa
{
	union
	{
		struct
		{
			uint64_t blk : BLK_BITS;
			uint64_t pg : PG_BITS;
			uint64_t sec : SEC_BITS;
			uint64_t pl : PL_BITS;
			uint64_t lun : LUN_BITS;
			uint64_t ch : CH_BITS;
			uint64_t rsv : 1;
		} g;

		uint64_t _ppa;
	};
};

typedef int nand_sec_status_t;

struct nand_page
{
	// nand_sec_status_t *sec;
	// int nsecs;
	int status;
};

struct nand_block
{
	struct nand_page *pg;
	int npgs;
	int ipc; /* invalid page count */
	int vpc; /* valid page count */
	int erase_cnt;
	int wp; /* current write pointer */
};

struct nand_plane
{
	struct nand_block *blk;
	int nblks;
};

struct nand_lun
{
	struct nand_plane *pl;
	int npls;
	// std::chrono::high_resolution_clock::time_point next_lun_avail_time;
	// std::atomic<uint64_t> next_lun_avail_time;
	bool busy;
	// std::chrono::high_resolution_clock::time_point gc_endtime;
	// std::list<time_slot> free_time_slot;
	uint64_t gc_endtime;
	pthread_mutex_t lun_mutex;
};

struct ssd_channel
{
	struct nand_lun *lun;
	int nluns;
	uint64_t next_ch_avail_time;
	bool busy;
	uint64_t gc_endtime;
};


struct time_slot {
  uint64_t start_time, end_time;
  // comparison operator for priority queue
  bool operator < (const time_slot& other) const {
    return start_time < other.start_time;
  }
};

struct cpu_ts {
  uint64_t start_time, end_time;
	struct cpu_ts *prev, *next;
};

struct cpu_ts_list {
	struct cpu_ts *head, *tail;
	pthread_mutex_t core_mtx;
};

struct ssd_embeded_core {
	cpu_ts_list free_ts;
};

struct ssdparams
{
	uint64_t capacity;
	int secsz;			 /* sector size in bytes */
	int secs_per_pg; /* # of sectors per page */
	int pgs_per_blk; /* # of NAND pages per block */
	int blks_per_pl; /* # of blocks per plane */
	int pls_per_lun; /* # of planes per LUN (Die) */
	int luns_per_ch; /* # of LUNs per channel */
	int nchs;				 /* # of channels in the SSD */

	int pg_rd_lat;	 /* NAND page read latency in nanoseconds */
	int pg_wr_lat;	 /* NAND page program latency in nanoseconds */
	int blk_er_lat;	 /* NAND block erase latency in nanoseconds */
	int ch_xfer_lat; /* channel transfer latency for one page in nanoseconds
										* this defines the channel bandwith
										*/
	double op_ratio;

	double gc_thres_pcent;
	int gc_thres_lines;
	double gc_thres_pcent_high;
	int gc_thres_lines_high;
	bool enable_gc_delay;

	int tt_host_lpn;

	/* below are all calculated values */
	int secs_per_blk; /* # of sectors per block */
	int secs_per_pl;	/* # of sectors per plane */
	int secs_per_lun; /* # of sectors per LUN */
	int secs_per_ch;	/* # of sectors per channel */
	int tt_secs;			/* # of sectors in the SSD */

	int pgs_per_pl;	 /* # of pages per plane */
	int pgs_per_lun; /* # of pages per LUN (Die) */
	int pgs_per_ch;	 /* # of pages per channel */
	int tt_pgs;			 /* total # of pages in the SSD */

	int blks_per_lun; /* # of blocks per LUN */
	int blks_per_ch;	/* # of blocks per channel */
	int tt_blks;			/* total # of blocks in the SSD */

	int secs_per_line;
	int pgs_per_line;
	int blks_per_line;
	int tt_lines;

	int pls_per_ch; /* # of planes per channel */
	int tt_pls;			/* total # of planes in the SSD */

	int tt_luns; /* total # of LUNs in the SSD */

	int pgs_per_line_FTL[21];

	int n_cpu_cores;
	int n_encrp_cores;
	int cpu_op_lat_first;
	int cpu_op_lat_second;
	int cpu_op_lat_cache;
	int cpu_cache_op_lat;

	int cpu_op_lat_remote_rd;
	int cpu_op_lat_remote_wr;

	int HMB_FTL_cache_size;
	int local_FTL_cache_size;
	int SGX_FTL_cache_size;
};

typedef struct line
{
	int id;	 /* line id, the same as corresponding block id */
	int ipc; /* invalid page count in this line */
	int vpc; /* valid page count in this line */
	QTAILQ_ENTRY(line)
	entry; /* in either {free,victim,full} list */
	int belong_to_th;
} line;

/* wp: record next write addr */
struct write_pointer
{
	struct line *curline;
	int cur_lun;
	int ch;
	int lun;
	int pg;
	int blk;
	int pl;
};

struct line_mgmt
{
	struct line *lines;
	/* free line list, we only need to maintain a list of blk numbers */
	QTAILQ_HEAD(free_line_list, line) free_line_list;
	// pqueue_t *victim_line_pq;
	QTAILQ_HEAD(victim_line_list, line) victim_line_list;
	QTAILQ_HEAD(full_line_list, line) full_line_list;
	int tt_lines;
	int free_line_cnt;
	int victim_line_cnt;
	int full_line_cnt;
	pthread_mutex_t lm_mutex;
};

struct nand_cmd
{
	int type;
	int cmd;
	// std::chrono::high_resolution_clock::time_point stime; /* Coperd: request arrival time */
	uint64_t stime;
};

typedef std::unordered_map<uint64_t, std::pair<uint64_t, std::list<uint64_t>::iterator>> FTLcache_t;

enum LAT_BKDN_TYPE_COUNT {
	ERROR,
	TOTAL_LAT,
	CPU_LAT,
	FTL_PG_TRANS_LAT,
	FTL_PG_NAND_LAT,
	WAITING_CACHE_LAT,
	DATA_NAND_LAT,
	LAT_BKDN_TYPE_COUNT
};

static const char *lat_bkdn_type_str[] = {
	"LAT_ERROR",
	"TOTAL_LAT",
	"CPU_LAT",
	"FTL_PG_TRANS_LAT",
	"FTL_PG_NAND_LAT",
	"WAITING_CACHE_LAT",
	"DATA_NAND_LAT",
	"LAT_BKDN_TYPE_COUNT"
};

enum CPU_BKDN_TYPE_COUNT {
	CPU_ERROR,
	CPU_COMM,
	CPU_FTL,
	CPU_POLLING,
	CPU_BKDN_TYPE_COUNT
};

static const char *cpu_bkdn_type_str[] = {
	"CPU_ERROR",
	"CPU_COMM",
	"CPU_FTL",
	"CPU_POLLING",
	"CPU_BKDN_TYPE_COUNT"
};

struct ssd_stats {
	uint64_t par_lat[LAT_BKDN_TYPE_COUNT];
	uint64_t CPU_util[CPU_BKDN_TYPE_COUNT];
	uint64_t req_cnt, culm_req_cnt;
	uint64_t trans_sz;
	uint64_t start_time;
};

struct ssd
{
	struct ssdparams sp;
	struct ssd_channel *ch;
	struct ppa *Nor_maptbl; /* page level mapping table */
	uint64_t *Nor_rmap;			/* reverse mapptbl, assume it's stored in OOB */
	struct ppa *SGX_maptbl;
	uint64_t *SGX_rmap;
	struct write_pointer wp[21]; /* support up to 21 FTL threads */
	struct line_mgmt lm[21];
	int next_lun[21][100];
	int lun_cnt[21];

	struct ssd_embeded_core *cores, *encrp_cores;
	int cur_core, cur_Nor_encrp_core, cur_SGX_encrp_core;

	uint64_t *SGXcache_avail_time;
	bool *SGXcache_dirty;
	uint64_t *freeSGXID;
	uint64_t freeSGXID_cnt;

	struct ssd_stats nor_stats, SGX_stats;

	struct nvmev_flight_io *flight_ios, *flight_io_poll, **pq;

	std::list<time_slot> *normal_lun, *SGX_lun;

	FTLcache_t HMBcache, localcache;
	std::list<uint64_t> HMBlist, locallist;
	std::unordered_map<uint64_t, uint64_t> localcache_avail_time, HMBcache_avail_time;
	std::unordered_map<uint64_t, bool> localcache_dirty, HMBcache_dirty;
	std::list<uint64_t> freelocalID, freeHMBID;

	/* lockless ring for communication with NVMe IO thread */
	// struct rte_ring **to_ftl;
	// struct rte_ring **to_poller;
	bool *dataplane_started_ptr;
};

struct FTL_SGXcache_t {
	std::list<uint64_t> SGXlist;
	FTLcache_t SGXcache;
};

uint64_t ssd_read(struct ssd *ssd, NvmeRequest *req, int FTLidx, struct FTL_SGXcache_t &SGXcache);
uint64_t ssd_write(struct ssd *ssd, NvmeRequest *req, int FTLidx, struct FTL_SGXcache_t &SGXcache);
void ssd_init(struct ssd *ssd);
int nvme_register_bbssd(FemuCtrl *n);

void *ftl_thread(void *arg);

#ifdef FEMU_DEBUG_FTL
#define ftl_debug(fmt, ...)                        \
	do                                               \
	{                                                \
		printf("[FEMU] FTL-Dbg: " fmt, ##__VA_ARGS__); \
	} while (0)
#else
#define ftl_debug(fmt, ...) \
	do                        \
	{                         \
	} while (0)
#endif

#define ftl_err(fmt, ...)                        \
	do                                               \
	{                                                \
		printf("[FEMU] FTL-Err: " fmt, ##__VA_ARGS__); \
	} while (0)

/* FEMU assert() */
// #define FEMU_DEBUG_FTL
#ifdef FEMU_DEBUG_FTL
#define ftl_assert(expression) assert(expression)
#else
#define ftl_assert(expression)
#endif

#endif
