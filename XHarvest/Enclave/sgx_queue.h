#ifndef __SGX_NVMEV_QUEUE_H
#define __SGX_NVMEV_QUEUE_H
#include <stdint.h>
#include <time.h>
#include <atomic>
// #include <linux/types.h>

typedef unsigned short __u16;
typedef unsigned char __u8;


struct nvmev_io_cmd {
	int sqid;
	int cqid;
	int sq_entry;
	unsigned int status;
	unsigned int result0;
	unsigned int result1;

	__u16 command_id;
	__u8 opcode;
	uint64_t slpa;
	int nlb;
	std::atomic<int> is_completed;
	uint64_t nsecs;
	uint64_t revserv;
};

struct nvmev_flight_io {
	int poller_id;
	int sqid;
	int cqid;
	int sq_entry;
	__u16 command_id;
	uint64_t start_time;
	uint64_t end_time;
	struct nvmev_flight_io *next;
};

struct internal_cmd {
	__u16 rsvd;
	__u16 opcode;
	std::atomic<int> is_completed;
	uint64_t param;
};

void enqueue_io_req_to_poller(int dispatcher_id, int FTL_id, int sqid, int cqid, uint64_t slpn, uint64_t nlp, struct nvmev_io_cmd *cmd);

void dequeue_io_req_from_poller(int dispatcher_id, int FTL_id, int sqid, int cqid, struct nvmev_io_cmd *cmd);

#endif /* SGX_QEMU_SYS_QUEUE_H */
