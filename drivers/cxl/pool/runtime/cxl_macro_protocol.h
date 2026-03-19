#ifndef CXL_MACRO_PROTOCOL_H
#define CXL_MACRO_PROTOCOL_H

#include <stdint.h>
#include <stdatomic.h>

#define CXL_MACRO_CTL_MAGIC 0x434D43544C4D4143ULL /* "CMCTLMAC" */
#define CXL_MACRO_CTL_VERSION 1

#define CXL_MACRO_MAX_MACHINES 8
#define CXL_MACRO_MAX_MACROS 16
#define CXL_MACRO_QUEUE_DEPTH 128
#define CXL_MACRO_MAX_BARRIERS 1024
#define CXL_MACRO_MAX_MAP_RECS 1024

enum cxl_cmd_type {
	CXL_CMD_INVALID = 0,
	CXL_CMD_MAP_REQ = 1,
	CXL_CMD_UNMAP_REQ = 2,
	CXL_CMD_SPAWN_REQ = 3,
	CXL_CMD_FATAL = 4,
};

struct cxl_cmd {
	uint32_t type;
	uint32_t macro_id;
	uint32_t src_machine;
	uint32_t dst_machine;
	uint64_t cmd_id;

	uint64_t alloc_id;
	uint64_t va_offset;
	uint64_t size;
	int32_t prot;
	int32_t pad0;

	uint32_t entry_id;
	uint32_t pad1;
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
	atomic_uint_fast32_t doorbell;
	struct cxl_queue_slot slots[CXL_MACRO_QUEUE_DEPTH];
};

struct cxl_barrier {
	atomic_uint_fast64_t in_use;
	atomic_uint_fast64_t cmd_id;
	atomic_uint_fast64_t expected_mask;
	atomic_uint_fast64_t ack_mask;
	atomic_int status;
	atomic_uint_fast32_t futex_word;
};

struct cxl_map_record {
	atomic_uint_fast64_t active;
	uint64_t alloc_id;
	uint64_t va_offset;
	uint64_t size;
	int32_t prot;
	uint32_t macro_id;
};

struct cxl_macro_state {
	atomic_uint_fast64_t membership_mask;
	atomic_uint_fast64_t fatal_flag;
	atomic_uint_fast64_t va_next;
	uint64_t window_size;
};

struct cxl_control_plane {
	uint64_t magic;
	uint64_t version;
	uint32_t nr_machines;
	uint32_t reserved0;
	uint64_t window_base;
	uint64_t window_size;
	uint64_t heartbeat_timeout_ns;

	atomic_uint_fast64_t global_cmd_id;
	atomic_uint_fast64_t ready_mask;
	atomic_uint_fast64_t stop_flag;

	struct cxl_macro_state macros[CXL_MACRO_MAX_MACROS];
	atomic_uint_fast64_t machine_heartbeat_ns[CXL_MACRO_MAX_MACHINES];
	struct cxl_machine_queue queues[CXL_MACRO_MAX_MACHINES];
	struct cxl_barrier barriers[CXL_MACRO_MAX_BARRIERS];
	struct cxl_map_record maps[CXL_MACRO_MAX_MAP_RECS];
};

#endif
