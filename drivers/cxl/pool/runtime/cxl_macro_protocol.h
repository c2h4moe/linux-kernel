#ifndef CXL_MACRO_PROTOCOL_H
#define CXL_MACRO_PROTOCOL_H

#include <stdatomic.h>
#include <stdint.h>

#define CXL_MACRO_CTL_MAGIC 0x434D43544C4D4143ULL /* "CMCTLMAC" */
#define CXL_MACRO_CTL_VERSION 4

#define CXL_MACRO_MAX_MACHINES 8
#define CXL_MACRO_QUEUE_DEPTH 128
#define CXL_MACRO_MAX_BARRIERS 1024
#define CXL_MACRO_MAX_MAP_RECS 1024

enum cxl_cmd_type {
	CXL_CMD_INVALID = 0,
	CXL_CMD_MAP_REQ = 1,
	CXL_CMD_UNMAP_REQ = 2,
	CXL_CMD_SPAWN_REQ = 3,
};

struct cxl_cmd {
	uint32_t type;
	uint32_t src_machine;
	uint32_t dst_machine;
	uint32_t entry_id;
	uint64_t cmd_id;

	uint64_t alloc_id;
	uint64_t va_offset;
	uint64_t size;
	int32_t prot;
	int32_t pad0;

	uint64_t arg_u64;
	uint64_t thread_id;
};

struct cxl_queue_slot {
	atomic_uint_fast64_t ready_seq;
	struct cxl_cmd cmd;
};

struct cxl_machine_queue {
	atomic_uint_fast64_t prod_seq;
	atomic_uint_fast64_t cons_seq;
	_Atomic uint32_t doorbell;
	struct cxl_queue_slot slots[CXL_MACRO_QUEUE_DEPTH];
};

struct cxl_barrier {
	atomic_uint_fast64_t in_use;
	atomic_uint_fast64_t cmd_id;
	atomic_uint_fast64_t expected_mask;
	atomic_uint_fast64_t ack_mask;
	atomic_int status;
	_Atomic uint32_t futex_word;
};

struct cxl_map_record {
	atomic_uint_fast64_t active;
	uint64_t alloc_id;
	uint64_t va_offset;
	uint64_t size;
	int32_t prot;
	int32_t pad0;
};

struct cxl_control_plane {
	uint64_t magic;
	uint64_t version;
	uint32_t nr_machines;
	uint32_t reserved0;
	uint64_t window_base;
	uint64_t window_size;

	atomic_uint_fast64_t global_cmd_id;
	atomic_uint_fast64_t va_next;
	_Atomic uint32_t launch_ready;
	_Atomic uint32_t launch_go;
	_Atomic uint32_t launch_futex;
	_Atomic uint32_t reserved1;
	_Atomic uint32_t app_mailbox32[4];

	struct cxl_machine_queue queues[CXL_MACRO_MAX_MACHINES];
	struct cxl_barrier barriers[CXL_MACRO_MAX_BARRIERS];
	struct cxl_map_record maps[CXL_MACRO_MAX_MAP_RECS];
};

#endif
