#ifndef __SGX_FEMU_NVME_H
#define __SGX_FEMU_NVME_H

// #define PG_CC_BREAKDOWN

#include <cstdint>
#include <pthread.h>
#include "Enclave_t.h"

#define NVME_ID_NS_LBADS(ns) \
  ((ns)->id_ns.lbaf[NVME_ID_NS_FLBAS_INDEX((ns)->id_ns.flbas)].lbads)

#define NVME_ID_NS_LBADS_BYTES(ns) (1 << NVME_ID_NS_LBADS(ns))

#define NVME_ID_NS_MS(ns) \
  le16_to_cpu(            \
      ((ns)->id_ns.lbaf[NVME_ID_NS_FLBAS_INDEX((ns)->id_ns.flbas)].ms))

#define NVME_ID_NS_LBAF_DS(ns, lba_index) (ns->id_ns.lbaf[lba_index].lbads)
#define NVME_ID_NS_LBAF_MS(ns, lba_index) (ns->id_ns.lbaf[lba_index].ms)

#define TYPE_NVME "femu"
#define FEMU(obj) OBJECT_CHECK(FemuCtrl, (obj), TYPE_NVME)

struct FemuCtrl;
typedef struct FemuExtCtrlOps
{
  void *state;
  void (*init)(struct FemuCtrl *);
  void (*exit)(struct FemuCtrl *);
  int (*start_ctrl)(struct FemuCtrl *);
} FemuExtCtrlOps;

struct nvmev_config
{
  int FTL_split_units;
  int split_ratio;
  int dispatcher_num;
  int ftl_thread_num;
  int SGX_ftl_thread_num;
  unsigned int io_workers_cpu[32];
  uint64_t **in_que_start, **out_que_start;
  uint64_t in_que_size, out_que_size;
  uint64_t internal_msg_queue_size;
  uint64_t storage_size;
};

typedef struct FemuCtrl
{

  struct nvmev_config config;

  struct nvmev_io_cmd ***to_ftl, ***to_poller;
  int **to_ftl_head, **to_poller_tail;

  FemuExtCtrlOps ext_ops;
  struct ssd *ssd;

  pthread_t *ftler;

  struct internal_cmd **to_slave, **to_master;
  int *to_slave_head, *to_slave_tail, *to_master_tail, *to_master_head;

} FemuCtrl;


enum NvmeIoCommands {
    NVME_CMD_FLUSH              = 0x00,
    NVME_CMD_WRITE              = 0x01,
    NVME_CMD_READ               = 0x02,
    NVME_CMD_DSM                = 0x09,
};

enum InternalCommands {
    INTER_CMD_UNKNOWN              = 0x01,
    INTER_CMD_GC                = 0x01,
};

typedef struct NvmeFTLThreadArgument
{
  FemuCtrl *n;
  int index;
} NvmePollerThreadArgument;

// #define FEMU_DEBUG_NVME
#ifdef FEMU_DEBUG_NVME
#define femu_debug(fmt, ...)                   \
  do                                           \
  {                                            \
    ocall_print_string(fmt); \
  } while (0)
#else
#define femu_debug(fmt, ...) \
  do                         \
  {                          \
  } while (0)
#endif

#define femu_err(fmt, ...)                              \
  do                                                    \
  {                                                     \
    ocall_print_string(fmt); \
  } while (0)

#define femu_log(fmt, ...)                     \
  do                                           \
  {                                            \
    ocall_print_string(fmt); \
  } while (0)

#endif /* __SGX_FEMU_NVME_H */
